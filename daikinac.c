// Daikin A/C control / monitoring

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/file.h>
#include <fcntl.h>
#include <netdb.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <err.h>
#include <signal.h>
#include <math.h>
#include <curl/curl.h>
#ifdef SQLLIB
#include <sqllib.h>
#endif
#ifdef LIBMQTT
#include <mosquitto.h>
#endif
#if	defined(SQLLIB) || defined(LIBMQTT)
#include <axl.h>
#endif

#define	controlfields			\
	c(pow, Power, 0/1)		\
	c(mode, Mode, 1-7)		\
	c(stemp, Temp, C)		\
	c(shum, Numidity, %)		\
	c(f_rate, Fan, A/B/3-7)		\
	c(f_dir, Fan dir, 0-3)		\

const char *modename[] = { "None", "Auto", "Dry", "Cool", "Heat", "Five", "Fan", "Auto" };

int mqttdebug = 0;
int curldebug = 0;
int debug = 0;

#ifdef LIBMQTT                  // Auto settings are done based on MQTT cmnd/[name]/atemp periodically
double maxtemp = 30;            // Aircon temp range allowed
double mintemp = 18;
double flip = 3;                // Max offset for flip
double maxroffset = 3;          // Max offset to apply (reverse)
double maxfoffset = 5;          // Max offset to apply (forward)
double margin = 1;              // Delta adjust threshold
int mqttperiod = 60;            // Logging period
int resetlag = 600;             // Wait for any major change to stabilise
int maxsamples = 60;            // For average logic
int minsamples = 10;            // For average logic
const char *mqttid = NULL;      // MQTT settings
const char *mqtthost = NULL;
const char *mqttuser = NULL;
const char *mqttpass = NULL;
const char *mqtttopic = "diakin";
const char *mqttcmnd = "cmnd";
const char *mqtttele = "tele";

        // This function does automatic temperature adjust
        // If SQL available it is called at start with recent data, in order to catch up any state it needs
        // Its job is to process current temp and settings and make any needed changes to settings
typedef struct temp_s temp_t;
struct temp_s
{
   temp_t *next;
   time_t updated;
   double temp;
   double pow;
};
typedef struct tempq_s tempq_t;
struct tempq_s
{
   temp_t *first,
    *last;
   int num;
   double sum;
   double sumpow;
};
tempq_t atempq = { };
tempq_t stempq = { };
tempq_t stemplagq = { };

double
addtemp (tempq_t * q, time_t updated, double temp, double pow)
{
   if (!updated)
      return 0;                 // Not set
   if (q->last && q->last->updated >= updated)
      return 0;                 // Not new
   double last = 0;
   if (q->last)
      last = q->last->temp;
   temp_t *t = malloc (sizeof (*t));
   if (!t)
      errx (1, "malloc");
   t->next = NULL;
   t->updated = updated;
   t->temp = temp;
   t->pow = pow;
   q->sum += temp;
   q->sumpow += pow;
   q->num++;
   if (q->last)
      q->last->next = t;
   else
      q->first = t;
   q->last = t;
   return temp - last;
}

void
flushtemp (tempq_t * q, time_t ref, tempq_t * req)
{
   while (q->first && q->first->updated <= ref)
   {
      temp_t *t = q->first;
      q->num--;
      q->sum -= t->temp;
      q->sumpow -= t->pow;
      q->first = t->next;
      if (req)
         addtemp (req, t->updated, t->temp, t->pow);
      free (t);
      if (!q->first)
         q->last = NULL;
   }
}

void
doauto (double *stempp, char *f_ratep, int *modep,      //
        int pow, int mompow, time_t updated, double atemp, double target)
{                               // Temp control. stemp/f_rate/mode are inputs and outputs
   // Get values
   double stemp = *stempp;
   char f_rate = *f_ratep;
   int mode = *modep;
   // state
   static double lasttarget = -999;     // Last values to spot changes
   static int lastmode = 0;     //
   static char lastf_rate = 0;  //
   static double offset = 0;    // Offset from target to set
   static time_t reset = 0;     // Change caused reset - this is when to start collecting data again
   static time_t stepchange = 0;        // Hold off step changes
   static double delta = 0.01;  // Adjustment to offset (dynamic)
   static double *t = NULL;     // Samples for averaging data
   static int sample = 0;
   if (!t)
      t = malloc (sizeof (*t) * maxsamples);    // Averaging data

   // Default
   *stempp = target + offset;   // Default

   // Changes
   if (lasttarget != target)
   {                            // Assume offset still OK
      if (debug > 1 && reset < updated)
         warnx ("Target change - resetting");
      lasttarget = target;
      reset = updated + resetlag;
   }
   if (lastf_rate != f_rate)
   {                            // Assume offset needs resetting
      if (debug > 1 && reset < updated)
         warnx ("Fan change - resetting");
      lastf_rate = f_rate;
      offset = 0;
      reset = updated + resetlag;
   }
   if (lastmode != mode)
   {                            // Assume offset needs resetting
      if (debug > 1 && reset < updated)
         warnx ("Mode change to %s - resetting", modename[mode]);
      lastmode = mode;
      offset = 0;
      reset = updated + resetlag;
   }
   if (!pow)
   {                            // Power off - assume offset needs resetting
      if (debug > 1 && reset < updated)
         warnx ("Power off - resetting");
      offset = 0;
      reset = updated + resetlag;
   }
   if (mode == 2 || mode == 6)
   {
      if (debug > 1 && reset < updated)
         warnx ("Mode %s - not running automatic control", modename[mode]);
      return;
   }

   int s;
   if (updated < reset)
   {                            // Waiting for startup or major change - reset data
      for (s = 0; s < maxsamples; s++)
         t[s] = -99;
      delta = 0.01;             // Reset delta
      if (debug > 1)
         warnx ("Waiting to settle (%ds) %.1lf", (int) (reset - updated), atemp);
      return;
   }
   t[sample++] = atemp;
   if (sample >= maxsamples)
      sample = 0;
   double min = 0,
      max = 0;
   int count = 0;
   for (s = 0; s < maxsamples; s++)
   {
      if (t[s] == -99)
         continue;
      if (!count || t[s] < min)
         min = t[s];
      if (!count || t[s] > max)
         max = t[s];
      count++;
   }
   if (count < minsamples)
   {
      if (debug > 1)
         warnx ("Collecting samples (%d/%d) %.1lf", count, minsamples, atemp);
      return;
   }
   double ave = (min + max) / 2;        // Using this rather than moving average as phase of oscillations is unpredicable

   // Dynamic delta adjust (a tad experimental)
   if (max - min > margin && delta > 0.01)
      delta *= 0.9;
   else if (delta < 0.2 && (ave < target - delta * 2 || ave > target + delta * 2))
      delta /= 0.9;

   // Adjust offset
   if (stepchange < updated && (min > target || max < target))
   {                            // Step change
      if (debug > 1)
         warnx ("Step change by %.1lf", target - ave);
      stepchange = updated + resetlag * 2;
      offset += (target - ave);
      delta = 0.01;             // Reset delta
   } else if (ave < target)
      offset += delta;
   else if (ave > target)
      offset -= delta;

   // Check if we need to change mode
   if ((mode == 4 && offset <= -flip) || (mode != 3 && mode != 4 && ave >= target))
   {
      if (debug > 1)
         warnx ("Changing to cool mode");
      mode = 3;                 // Heating and we are still too high so switch to cool
   } else if ((mode == 3 && offset >= flip) || (mode != 3 && mode != 4 && ave <= target))
   {
      if (debug > 1)
         warnx ("Changing to heat mode");
      mode = 4;                 // Cooling and we are still too low so switch to head
   }
   // Limit offset
   if (mode == 4 && offset > maxfoffset)
   {
      if (f_rate == 'B')
         f_rate = 'A';          // Give up on night mode
      offset = maxfoffset;
   } else if (mode == 4 && offset < -maxroffset)
      offset = -maxroffset;
   else if (mode == 4 && offset < -maxfoffset)
   {
      if (f_rate == 'B')
         f_rate = 'A';          // Give up on night mode
      offset = -maxfoffset;
   } else if (mode == 4 && offset > maxroffset)
      offset = maxroffset;

   // Apply new temp
   stemp = target + offset;     // Apply offset

   if (debug > 1)
      warnx ("Temp %.1lf Mode %s F_rate %c Target %.1lf Offset %.1lf Ave %.1lf(%d) Min %.1lf Max %.1lf Delta %.2lf", atemp,
             modename[mode], f_rate, target, offset, ave, count, min, max, delta);

   // Write back
   *stempp = stemp;
   *f_ratep = f_rate;
   *modep = mode;
}
#endif


int
main (int argc, const char *argv[])
{
#define c(x,t,v) char *set##x=NULL;     // Args
   controlfields;
#undef	c
   // AC constants
#ifdef SQLLIB
   const char *db = NULL;
   const char *table = "daikin";
   const char *svgdate = NULL;
   int svgl = 2;                // Low C
   int svgt = 32;               // High C
   int svgc = 25;               // Per C spacing height
   int svgh = 60;               // Per hour spacing width
   int svgwidth = 24 * svgh;
   int svgheight = (svgt - svgl) * svgc;
#endif
   int info = 0,
      modeoff = 0,
      modeon = 0,
      modeauto = 0,
      modeheat = 0,
      modecool = 0,
      modedry = 0,
      modefan = 0,
      dolock = 0;
   int retries = 5;
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
#define	c(x,t,v)	{#x, 0, POPT_ARG_STRING, &set##x, 0, #t, #v},
         controlfields
#undef c
		 // *INDENT-OFF*
         { "on", 0, POPT_ARG_NONE, &modeon, 0, "On"},
         { "off", 0, POPT_ARG_NONE, &modeoff, 0, "Off"},
         { "auto", 'A', POPT_ARG_NONE, &modeauto, 0, "Auto"},
         { "heat", 'H', POPT_ARG_NONE, &modeheat, 0, "Heat"},
         { "cool", 'C', POPT_ARG_NONE, &modecool, 0, "Cool"},
         { "dry", 'D', POPT_ARG_NONE, &modedry, 0, "Dry"},
         { "fan", 'F', POPT_ARG_NONE, &modefan, 0, "Fan"},
         { "info", 'i', POPT_ARG_NONE, &info, 0, "Show info"},
#ifdef SQLLIB
         { "log", 'l', POPT_ARG_STRING, &db, 0, "Log", "database"},
         { "table", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &table, 0, "Table", "table"},
         { "svg", 0, POPT_ARG_STRING, &svgdate, 0, "Make SVG", "YYYY-MM-DD"},
         { "sql-debug", 0, POPT_ARG_NONE, &sqldebug, 0, "Debug"},
#endif
#ifdef LIBMQTT
         { "mqtt-host", 'h', POPT_ARG_STRING, &mqtthost, 0, "MQTT host", "hostname"},
         { "mqtt-id", 0, POPT_ARG_STRING, &mqttid, 0, "MQTT id", "id"},
         { "mqtt-user", 0, POPT_ARG_STRING, &mqttuser, 0, "MQTT user", "username"},
         { "mqtt-pass", 0, POPT_ARG_STRING, &mqttpass, 0, "MQTT pass", "password"},
         { "mqtt-topic", 't', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &mqtttopic, 0, "MQTT topic", "topic"},
         { "mqtt-cmnd", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &mqttcmnd, 0, "MQTT cmnd prefix", "prefix"},
         { "mqtt-tele", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &mqtttele, 0, "MQTT tele prefix", "prefix"},
         { "mqtt-period", 0, POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &mqttperiod, 0, "MQTT reporting interval", "seconds"},
         { "flip", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &flip, 0, "Max reverse offset to flip modes", "C"},
         { "max-samples", 0, POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &maxsamples, 0, "Max samples used for averaging", "N"},
         { "min-samples", 0, POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &minsamples, 0, "Min samples used for averaging", "N"},
         { "max-forward", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &maxfoffset, 0, "Max forward offset to apply", "C"},
         { "max-reverse", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &maxroffset, 0, "Max reverse offset to apply", "C"},
         { "lock", 0, POPT_ARG_NONE, &dolock, 0, "Lock operation"},
         { "mqtt-debug", 0, POPT_ARG_NONE, &mqttdebug, 0, "Debug"},
#endif
         { "curl-debug", 0, POPT_ARG_NONE, &curldebug, 0, "Debug"},
         { "curl-retries", 0, POPT_ARG_INT| POPT_ARGFLAG_SHOW_DEFAULT, &retries, 0, "HTTP retries to A/C"},
         { "debug", 0, POPT_ARG_NONE, &debug, 0, "Debug"},
	 POPT_AUTOHELP { }
		 // *INDENT-ON*
      };

      optCon = poptGetContext (NULL, argc, argv, optionsTable, 0);
      poptSetOtherOptionHelp (optCon, "[IP]");

      int c;
      if ((c = poptGetNextOpt (optCon)) < -1)
         errx (1, "%s: %s\n", poptBadOption (optCon, POPT_BADOPTION_NOALIAS), poptStrerror (c));

      if (!poptPeekArg (optCon) || modeauto + modeheat + modecool + modedry + modefan + (setmode ? 1 : 0) > 1
          || modeoff + modeon + (setpow ? 1 : 0) > 1)
      {
         poptPrintUsage (optCon, stderr, 0);
         return -1;
      }
      // Set mode number
      if (modeauto)
         setmode = "7";
      if (modeheat)
         setmode = "4";
      if (modecool)
         setmode = "3";
      if (modedry)
         setmode = "2";
      if (modefan)
         setmode = "6";
      // Power
      if (modeon)
         setpow = "1";
      if (modeoff)
         setpow = "0";

      CURL *curl = curl_easy_init ();
      curl_easy_setopt (curl, CURLOPT_CONNECTTIMEOUT, 10L);
      curl_easy_setopt (curl, CURLOPT_TIMEOUT, 60L);
      if (curldebug)
         curl_easy_setopt (curl, CURLOPT_VERBOSE, 1L);

      char *get (char *url)
      {                         // Get from URL (frees URL, malloced reply)
         curl_easy_setopt (curl, CURLOPT_HTTPGET, 1L);
         curl_easy_setopt (curl, CURLOPT_URL, url);
         char *reply = NULL;
         size_t replylen = 0;
         FILE *o = open_memstream (&reply, &replylen);
         curl_easy_setopt (curl, CURLOPT_WRITEDATA, o);
         CURLcode result = curl_easy_perform (curl);
         fclose (o);
         long code = 0;
         if (!result)
            curl_easy_getinfo (curl, CURLINFO_RESPONSE_CODE, &code);
         if ((code / 100) != 2)
         {
            syslog (LOG_INFO, "Failed %s", url);
            if (debug)
               warnx ("Fail %s\n", url);
            if (reply)
               free (reply);
            free (url);
            return NULL;
         }
         if (curldebug)
            fprintf (stderr, "Request:\t%s\nReply:\t%s\n", url, reply);
         free (url);
         return reply;
      }

#ifdef SQLLIB
      SQL sql;
      SQL_RES *fields = NULL;
      if (db)
      {                         // Database fields
         sql_safe_connect (&sql, NULL, NULL, NULL, db, 0, NULL, 0);
         fields = sql_safe_query_store_free (&sql, sql_printf ("SELECT * FROM `%#S` WHERE false", table));
         sql_fetch_row (fields);
      }
#endif

#ifdef SQLLIB
      if (svgdate)
      {                         // Make an SVG for a date from the logs
         if (!db)
            errx (1, "No database");
         const char *ip = NULL;
         while ((ip = poptGetArg (optCon)))
         {
            xml_t svg = xml_tree_new ("svg");
            xml_element_set_namespace (svg, xml_namespace (svg, NULL, "http://www.w3.org/2000/svg"));
            xml_addf (svg, "@width", "%d", svgwidth);
            xml_addf (svg, "@height", "%d", svgheight + 25);
            // Graph data
            int stempref = -1,
               lastmode = 0,
               lasty = 0,
               x = 0;
            size_t atemplen = 0,
               htemplen = 0,
               otemplen = 0,
               mompowlen = 0,
               heatlen = 0,
               coollen = 0,
               dt1len = 0;
            char *atempbuf = NULL,
               *htempbuf = NULL,
               *otempbuf = NULL,
               *mompowbuf = NULL,
               *heatbuf = NULL,
               *coolbuf = NULL,
               *dt1buf = NULL;
            char atempm = 'M',
               htempm = 'M',
               otempm = 'M',
               mompowm = 'M',
               dt1m = 'M';
            FILE *atemp = open_memstream (&atempbuf, &atemplen);
            FILE *htemp = open_memstream (&htempbuf, &htemplen);
            FILE *otemp = open_memstream (&otempbuf, &otemplen);
            FILE *mompow = open_memstream (&mompowbuf, &mompowlen);
            FILE *dt1 = open_memstream (&dt1buf, &dt1len);
            FILE *heat = open_memstream (&heatbuf, &heatlen);
            FILE *cool = open_memstream (&coolbuf, &coollen);
            SQL_RES *res = sql_safe_query_store_free (&sql,
                                                      sql_printf ("SELECT * FROM `%#S` WHERE `Updated` LIKE '%#S%%' AND `IP`=%#s",
                                                                  table,
                                                                  svgdate, ip));
            while (sql_fetch_row (res))
            {
               char *v = sql_col (res, "Updated");
               if (strlen (v) < 19)
                  continue;
               x = (atoi (v + 11) * 3600 + atoi (v + 14) * 60 + atoi (v + 17)) * svgh / 3600;
               double d;
               v = sql_col (res, "atemp");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (atemp, "%c%d,%d", atempm, x, (int) (svgheight - (d - svgl) * svgc));
                  atempm = 'L';
               }
               v = sql_col (res, "htemp");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (htemp, "%c%d,%d", htempm, x, (int) (svgheight - (d - svgl) * svgc));
                  htempm = 'L';
               }
               v = sql_col (res, "otemp");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (otemp, "%c%d,%d", otempm, x, (int) (svgheight - (d - svgl) * svgc));
                  otempm = 'L';
               }
               v = sql_col (res, "mompow");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (mompow, "%c%d,%d", mompowm, x, (int) (svgheight + d));
                  mompowm = 'L';
               }
               v = sql_col (res, "dt1");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (dt1, "%c%d,%d", dt1m, x, (int) (svgheight - (d - svgl) * svgc));
                  dt1m = 'L';
               }
               v = sql_col (res, "stemp");
               int pow = atoi (sql_colz (res, "pow"));
               int mode = atoi (sql_colz (res, "mode"));
               if (!pow)
                  mode = -1;    // Not on
               if (mode != lastmode && stempref >= 0)
               {                // Close box
                  if (lastmode == 3)
                     fprintf (cool, "L%d %dL%d %dL%d %dZ", x, lasty, x, 0, stempref, 0);
                  if (lastmode == 4)
                     fprintf (heat, "L%d %dL%d %dL%d %dZ", x, lasty, x, svgheight, stempref, svgheight);
                  stempref = -1;
               }
               if (mode == 3 || mode == 4)
               {
                  d = strtod (v, NULL);
                  if (stempref >= 0)
                     fprintf (mode == 3 ? cool : heat, "L%d %dL", x, lasty);
                  else
                     fprintf (mode == 3 ? cool : heat, "M");
                  fprintf (mode == 3 ? cool : heat, "%d %d", x, lasty = (int) (svgheight - (d - svgl) * svgc));
                  if (stempref < 0)
                     stempref = x;
               }
               lastmode = mode;
            }
            x += svgh / 60;     // Assume minute stats to draw last bar
            if (lastmode == 3)
               fprintf (cool, "L%d %dL%d %dL%d %dZ", x, lasty, x, 0, stempref, 0);
            if (lastmode == 4)
               fprintf (heat, "L%d %dL%d %dL%d %dZ", x, lasty, x, svgheight, stempref, svgheight);
            fclose (atemp);
            fclose (htemp);
            fclose (otemp);
            fclose (mompow);
            fclose (dt1);
            fclose (heat);
            fclose (cool);
            xml_addf (svg, "+path@fill=red@stroke=none@opacity=0.25@d", heatbuf);
            xml_addf (svg, "+path@fill=blue@stroke=none@opacity=0.25@d", coolbuf);
            xml_addf (svg, "+path@fill=none@stroke=red@stroke-linecap=round@stroke-linejoin=round@d", atempbuf);
            xml_addf (svg, "+path@fill=none@stroke=green@stroke-linecap=round@stroke-linejoin=round@d", htempbuf);
            xml_addf (svg, "+path@fill=none@stroke=blue@stroke-linecap=round@stroke-linejoin=round@d", otempbuf);
            xml_addf (svg, "+path@fill=none@stroke=black@stroke-linecap=round@stroke-linejoin=round@d", mompowbuf);
            xml_addf (svg, "+path@fill=none@stroke=black@stroke-dasharray=1@d", dt1buf);
            free (atempbuf);
            free (htempbuf);
            free (otempbuf);
            free (mompowbuf);
            free (dt1buf);
            free (heatbuf);
            free (coolbuf);
            {
               // Time of day and headings
               int x,
                 y;
               for (x = 0; x < svgwidth; x += svgh)
               {
                  xml_addf (svg, "+path@stroke=grey@fill=none@opacity=0.5@stroke-dasharray=1@stroke-width=0.5@d", "M%d 0v%d", x,
                            svgheight);
                  xml_t t = xml_addf (svg, "+text", "%02d", x / svgh);
                  xml_addf (t, "@x", "%d", x);
                  xml_addf (t, "@y", "%d", svgheight);
                  xml_add (t, "@text-anchor", "middle");
               }
               for (y = svgc; y < svgheight; y += svgc)
               {
                  xml_addf (svg, "+path@stroke=grey@fill=none@opacity=0.5@stroke-dasharray=1@stroke-width=0.5@d", "M0 %dh%d",
                            svgheight - y, svgwidth);
                  xml_t t = xml_addf (svg, "+text", "%d℃", y / svgc + svgl);
                  xml_addf (t, "@x", "%d", 0);
                  xml_addf (t, "@y", "%d", svgheight - y);
                  xml_add (t, "@alignment-baseline", "middle");
               }
            }
            sql_free_result (res);
            sql_close (&sql);
            xml_write (stdout, svg);
            xml_tree_delete (svg);
         }
         return 0;
      }
#endif

      int changed = 0;
      char *sensor = NULL;
      char *control = NULL;
#ifdef	LIBMQTT
      int thispow = 0;
      int thismompow = 0;
      int thismode = 0;
      double thisstemp = 0,
         thisdt1 = 0;
      char thisf_rate = 0;
      time_t atempset = 0;      // Time last set
      double atemp = 0;         // Last set
#endif
      const char *ip;
#define c(x,t,v) char *x=NULL;  // Current settings
      controlfields;
#undef	c
#if	defined(SQLLIB) || defined(LIBMQTT)
      typedef void found_t (char *tag, char *val);
      void scan (char *reply, found_t * found)
      {
         char *p = reply;
         while (*p)
         {
            char *e = p;
            while (*e && *e != '=')
               e++;
            if (*e != '=')
               break;
            *e = 0;
            char *c = e + 1;
            while (*c && *c != ',')
               c++;
            char *n = c;
            if (*n)
               *n++ = 0;
            found (p, e + 1);
            *e = '=';
            if (n > c)
               *c = ',';
            p = n;
         }
      }
#endif
      int lock = -1;
      // Get status
      int getstatus (void)
      {
         if (dolock)
         {
            char *fn = NULL;
            if (asprintf (&fn, "/tmp/daikinac-%s", ip) < 0)
               errx (1, "malloc");
            lock = open (fn, O_CREAT, 0777);
            if (lock < 0)
            {
               warn ("Cannot make lock file %s", fn);
               free (fn);
               return 0;        // Uh?
            }
            free (fn);
            flock (lock, LOCK_EX);
         }
         // Reset
         changed = 0;
#ifdef	LIBMQTT
         thisstemp = 0;
         thisf_rate = 0;
         thisdt1 = 0;
         thispow = 0;
         thismompow = 0;
#endif
         char *url;
         int tries = retries;
         while (tries--)
         {
            if (asprintf (&url, "http://%s/aircon/get_sensor_info", ip) < 0)
               errx (1, "malloc");
            sensor = get (url);
            if (sensor)
               break;
         }
         if (!sensor)
            return 0;
         tries = retries;
         while (tries--)
         {
            if (asprintf (&url, "http://%s/aircon/get_control_info", ip) < 0)
               errx (1, "malloc");
            control = get (url);
            if (control)
               break;
         }
         if (!control)
            return 0;
         return 1;              // OK
      }
      void freestatus (void)
      {
         if (dolock)
         {
            if (lock >= 0)
            {
               flock (lock, LOCK_UN);
               close (lock);
               lock = -1;
            }
         }
         if (sensor)
         {
            free (sensor);
            sensor = NULL;
         }
         if (control)
         {
            free (control);
            control = NULL;
         }
#define	c(x,t,v) if(x)free(x);x=NULL;
         controlfields;
#undef c
      }
#ifdef	LIBMQTT
      // Update status
      void updatestatus ()
      {
         void check (char *tag, char *val)
         {
            if (info && strcmp (tag, "ret"))
               printf ("%s\t%s\n", tag, val);
#define	c(x,t,v) if(!strcmp(#x,tag))if(val&&(!x||strcmp(x,val))){if(x)free(x);x=strdup(val);}
            controlfields;
#undef c
            // Note some settings
            if (!strcmp (tag, "pow"))
               thispow = atoi (val);
            else if (!strcmp (tag, "mode"))
            {
               thismode = atoi (val);
               if (thismode < 0 || thismode >= sizeof (modename) / sizeof (*modename))
                  thismode = 0;
            } else if (!strcmp (tag, "mompow"))
               thismompow = atoi (val);
            else if (!strcmp (tag, "f_rate"))
               thisf_rate = *val;
            else if (!strcmp (tag, "stemp"))
               thisstemp = strtod (val, NULL);
            else if (!strcmp (tag, "dt1"))
               thisdt1 = strtod (val, NULL);
         }
         scan (sensor, check);
         scan (control, check);
      }
#else
#define	updatestatus(s,c)
#endif
      void updatesettings ()
      {                         // Set new control
         char *url = NULL;
         size_t len = 0;
         FILE *o = open_memstream (&url, &len);
         fprintf (o, "http://%s/aircon/set_control_info?", ip);
#define c(x,t,v) fprintf(o,"%s=%s&",#x,x);
         controlfields;
#undef c
         fclose (o);
         url[--len] = 0;
         char *ok = get (url);
         free (ok);
      }

      void updatedb (void)
      {
#ifdef SQLLIB
         if (!db)
            return;
         sql_string_t s = { };
         sql_sprintf (&s, "INSERT INTO `%#S` SET `ip`=%#s", table, ip);
         void update (char *tag, char *val)
         {
            int f = sql_colnum (fields, tag);
            if (f < 0)
               return;
#define c(x,t,v) if(!strcmp(#x,tag)&&x)val=x;   // Use the setting we now have
            controlfields;
#undef c
            sql_sprintf (&s, ",`%#S`=%#s", tag, val);
         }
         scan (sensor, update);
         scan (control, update);
#ifdef	LIBMQTT
         if (atempset && atempset > time (0) - mqttperiod * 2 && sql_colnum (fields, "atemp") >= 0)
            sql_sprintf (&s, ",`atemp`=%.1lf", atemp);
#endif
         sql_safe_query_s (&sql, &s);
#endif
      }

#ifdef LIBMQTT
      if (mqtthost)
      {                         // Handling MQTT only
         openlog ("daikinac", LOG_CONS | LOG_PID, LOG_USER);
         ip = poptGetArg (optCon);
         if (poptPeekArg (optCon))
            errx (1, "One aircon only for MQTT operation");
#ifdef	SQLLIB
         if (db)
         {                      // Re-run history from database so auto can catch up to current state
            SQL_RES *res = sql_safe_query_store_free (&sql,
                                                      sql_printf
                                                      ("SELECT * FROM `%#S` WHERE `ip`=%#s AND `Updated`>=date_sub(now(),interval 1 day) ORDER BY `Updated`",
                                                       table, ip));
            while (sql_fetch_row (res))
            {
               char *v = sql_col (res, "atemp");
               if (!v)
                  continue;
               double atemp = strtod (v, NULL);
               v = sql_col (res, "stemp");
               if (!v)
                  continue;
               double stemp = strtod (v, NULL);
               v = sql_col (res, "dt1");
               if (!v)
                  continue;
               double target = strtod (v, NULL);
               int mode = atoi (sql_colz (res, "mode"));
               char f_rate = *sql_colz (res, "f_rate");
               time_t updated = xml_time (sql_colz (res, "updated"));
               int mompow = atoi (sql_colz (res, "mompow"));
               int pow = atoi (sql_colz (res, "pow"));
               doauto (&stemp, &f_rate, &mode, pow, mompow, updated, atemp, target);
            }
            sql_free_result (res);
         }
#endif
         time_t next = time (0) + mqttperiod;
         int e = mosquitto_lib_init ();
         if (e)
            errx (1, "MQTT init failed %s", mosquitto_strerror (e));
         struct mosquitto *mqtt = mosquitto_new (mqttid ? : ip, 1, NULL);
         void connect (struct mosquitto *mqtt, void *obj, int rc)
         {
            obj = obj;
            rc = rc;
            char *sub = NULL;
            asprintf (&sub, "%s/%s/#", mqttcmnd, mqtttopic);
            if (mqttdebug)
               warnx ("MQTT connect %s for %s", mqtthost, sub);
            syslog (LOG_INFO, "%s MQTT connected %s", mqtttopic, mqtthost);
            int e = mosquitto_subscribe (mqtt, NULL, sub, 0);
            if (e)
               errx (1, "MQTT subscribe failed %s", mosquitto_strerror (e));
            free (sub);
         }
         void disconnect (struct mosquitto *mqtt, void *obj, int rc)
         {
            obj = obj;
            rc = rc;
            if (mqttdebug)
               warnx ("MQTT disconnect %s", mqtthost);
            syslog (LOG_INFO, "%s MQTT disconnected %s (reconnecting)", mqtttopic, mqtthost);
            e = mosquitto_reconnect (mqtt);
            if (e)
               errx (1, "MQTT reconnect failed (%s) %s", mqtthost, mosquitto_strerror (e));
         }
         void message (struct mosquitto *mqtt, void *obj, const struct mosquitto_message *msg)
         {
            obj = obj;
            char *topic = msg->topic;
            if (mqttdebug)
               warnx ("MQTT message %s %.*s", topic, msg->payloadlen, (char *) msg->payload);
            syslog (LOG_INFO, "%s MQTT message %s %.*s", mqtttopic, topic, msg->payloadlen, (char *) msg->payload);
            int l = strlen (mqttcmnd);
            if (strncmp (topic, mqttcmnd, l) || topic[l] != '/')
               return;
            topic += l + 1;
            l = strlen (mqtttopic);
            if (strncmp (topic, mqtttopic, l) || topic[l] != '/')
               return;
            topic += l + 1;
            l = msg->payloadlen;
            char *p = msg->payload;
            if (l > 2 && *p == '"' && p[l - 1] == '"')
            {
               p++;
               l -= 2;
            }
            char *val = malloc (l + 1);
            memcpy (val, p, l);
            if (getstatus ())
            {
               updatestatus ();
               val[l] = 0;
#define	c(x,t,v) if(!strcmp(#x,topic)){if(val&&(!x||strcmp(x,val))){if(x)free(x);x=strdup(val);changed=1;}}
               controlfields;
#undef c
               if (!strcmp (topic, "atemp"))
               {
                  atemp = strtod (val, NULL);
                  next = atempset = time (0);
               }
               if (topic[0] == 'd' && topic[1] == 't' && isdigit (topic[2]) && !topic[3])
               {                // Special case, setting dtN means setting a mode and stemp
                  char *url = NULL;
                  size_t len = 0;
                  FILE *o = open_memstream (&url, &len);
                  fprintf (o, "http://%s/aircon/set_control_info?", ip);
#define c(x,t,v) if(!strcmp(#x,"stemp"))fprintf(o,"%s=%s&",#x,val); else if(!strcmp(#x,"mode"))fprintf(o,"%s=%s&",#x,topic+2); else fprintf(o,"%s=%s&",#x,x);
                  controlfields
#undef c
                     fclose (o);
                  url[--len] = 0;
                  char *ok = get (url);
                  free (ok);
                  changed = 1;  // Force setting back to right mode
               }
               if (changed)
                  updatesettings (sensor, control);
            }
            freestatus ();
            free (val);
         }
         mosquitto_connect_callback_set (mqtt, connect);
         mosquitto_disconnect_callback_set (mqtt, disconnect);
         mosquitto_message_callback_set (mqtt, message);
         e = mosquitto_connect (mqtt, mqtthost, 1883, 60);
         if (e)
            errx (1, "MQTT connect failed (%s) %s", mqtthost, mosquitto_strerror (e));
         if (debug)
         {
            debug++;
            warnx ("Starting service");
         }
         while (1)
         {
            time_t now = time (0);
            int to = next - now;
            if (to <= 0)
            {                   // stat
               next += mqttperiod;
               if (getstatus ())
               {
                  updatestatus ();
                  if (atempset)
                  {             // Automatic processing
                     double newstemp = thisstemp;
                     char newf_rate = thisf_rate;
                     int newmode = thismode;
                     doauto (&newstemp, &newf_rate, &newmode, thispow, thismompow, atempset, atemp, thisdt1);
                     {          // Rounding temp to 0.5C with error dither
                        static double dither = 0;
                        double rtemp = newstemp;
                        newstemp = round ((newstemp - dither) * 2) / 2; // It gets upset if not .0 or .5
                        dither += newstemp - rtemp;
                     }
                     if (newstemp > maxtemp)
                        newstemp = maxtemp;
                     else if (newstemp < mintemp)
                        newstemp = mintemp;
                     // Apply changes
                     if (newstemp != thisstemp)
                     {
                        if (stemp)
                           free (stemp);
                        if (asprintf (&stemp, "%.1lf", newstemp) < 0)
                           errx (1, "malloc");
                        changed = 1;
                     }
                     if (newf_rate != thisf_rate)
                     {
                        if (f_rate)
                           free (f_rate);
                        if (asprintf (&f_rate, "%c", newf_rate) < 0)
                           errx (1, "malloc");
                        changed = 1;
                     }
                     if (newmode != thismode)
                     {
                        if (mode)
                           free (mode);
                        if (asprintf (&mode, "%d", newmode) < 0)
                           errx (1, "malloc");
                        changed = 1;
                     }
                  }

                  if (changed)
                     updatesettings (sensor, control);
                  updatedb ();
                  xml_t stat = xml_tree_new (NULL);
                  void check (char *tag, char *val)
                  {
                     // Only some things we report
                     if (!strncmp (tag, "b_", 2)
                         || (strncmp (tag, "f_", 2) && !strstr (tag, "pow") && !strstr (tag, "temp") && strcmp (tag, "mode")
                             && !strstr (tag, "hum") && strcmp (tag, "adv")))
                        return;
                     xml_attribute_set (stat, tag, val);
                  }
                  scan (sensor, check);
                  scan (control, check);
                  if (atempset && atempset > now - mqttperiod * 2)
                     xml_addf (stat, "@atemp", "%.lf", atemp);
                  char *statbuf = NULL;
                  size_t statlen = 0;
                  FILE *s = open_memstream (&statbuf, &statlen);
                  xml_write_json (s, stat);
                  fclose (s);
                  char *topic = NULL;
                  asprintf (&topic, "%s/%s/STATE", mqtttele, mqtttopic);
                  e = mosquitto_publish (mqtt, NULL, topic, strlen (statbuf), statbuf, 0, 1);
                  if (mqttdebug)
                     warnx ("Publish %s %s", topic, statbuf);
                  free (topic);
                  free (statbuf);
                  xml_tree_delete (stat);
                  if (atempset)
                     next += 10;        // Expect temp to be set again around next period
               } else
                  next = now;   // Try again!
               freestatus ();
               to = next - now;
            }
            if (to < 1)
               to = 1;
            e = mosquitto_loop (mqtt, to * 1000, 1);
            if (e)
               errx (1, "MQTT loop failed %s", mosquitto_strerror (e));
         }
         mosquitto_destroy (mqtt);
         mosquitto_lib_cleanup ();
      }
#endif
      while ((ip = poptGetArg (optCon)))
      {                         // Process for each IP
         if (getstatus ())
         {
            updatestatus (sensor, control);
#define	c(x,t,v) if(set##x&&x&&strcmp(x,set##x)){changed=1;if(x)free(x);x=strdup(set##x);}
            controlfields;
#undef c
            if (changed)
               updatesettings (sensor, control);
            updatedb ();
         }
         freestatus ();
      }

      poptFreeContext (optCon);
#ifdef SQLLIB
      if (db)
      {
         if (fields)
            sql_free_result (fields);
         sql_close (&sql);
      }
#endif
   }

   return 0;
}
