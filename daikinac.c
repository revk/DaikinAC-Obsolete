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
#ifdef LIBSNMP
#include <net-snmp/net-snmp-config.h>
#include <net-snmp/net-snmp-includes.h>
#include <net-snmp/session_api.h>
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
double ripple = 0.1;            // allow some ripple
double startheat = -1;          // Where to start heating
double startcool = 1;           // Where to start cooling
double maxrheat = 2;            // Max offset to apply (reverse) - heating
double maxfheat = 3;            // Max offset to apply (forward) - heating
double maxrcool = 4;            // Max offset to apply (reverse) - cooling
double maxfcool = 3;            // Max offset to apply (forward) - cooling
double driftrate = 0.01;        // Per sample slow drift allowed
double driftback = 0.999;       // slow return to 0
int cmpfreqlow = 10;            // Low rate allowed
int mqttperiod = 60;            // Logging period
int resetlag = 900;             // Wait for any major change to stabilise
int maxsamples = 60;            // For average logic
int minsamples = 5;             // For average logic
const char *mqttid = NULL;      // MQTT settings
const char *mqtthost = NULL;
const char *mqttuser = NULL;
const char *mqttpass = NULL;
const char *mqtttopic = "diakin";
const char *mqttcmnd = "cmnd";
const char *mqtttele = "tele";
char *mqttatemp = NULL;

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
        int pow, int cmpfreq, int mompow, time_t updated, double atemp, double target)
{                               // Temp control. stemp/f_rate/mode are inputs and outputs
   // Get values
   double stemp = *stempp;
   char f_rate = *f_ratep;
   int mode = *modep;
   // state
   static double lastatemp = 0; // Last atemp
   static double lasttarget = -999;     // Last values to spot changes
   static int lastmode = 0;     //
   static char lastf_rate = 0;  //
   static double offset = 0;    // Offset from target to set
   static time_t reset = 0;     // Change caused reset - this is when to start collecting data again
   static time_t nextsample = 0;        // Make data collection reasonably regular
   static double *t = NULL;     // Samples for averaging data
   static int sample = 0;
   if (!t)
      t = malloc (sizeof (*t) * maxsamples);    // Averaging data

   int s;
   double atempdelta = atemp - lastatemp;       // Rate of change
   lastatemp = atemp;

   int overshootcheck (void)
   {                            // react to going to overshoot
      if ((mode == 4 && (atemp >= target + ripple || atemp + atempdelta > target + ripple) && cmpfreq > cmpfreqlow) ||
          (mode == 3 && (atemp <= target - ripple || atemp + atempdelta < target - ripple) && cmpfreq > cmpfreqlow))
      {                         // Time to stop compressor (setting temp 0 does this)
         *stempp = 0;
         reset = 0;             // We hit end stop, so can start collecting data now
         return 1;
      }
      return 0;                 // OK

   }
   void resetdata (time_t lag)
   {                            // Reset average (set to start collecting after a lag) - used when a change happens
      reset = updated + lag;
      for (s = 0; s < maxsamples; s++)
         t[s] = -99;
   }
   void resetoffset (time_t lag)
   {                            // Reset the offset
      offset = (mode == 4 ? startheat : mode==3?startcool:0);
      resetdata (lag);
   }
   if (lasttarget != target)
   {                            // Assume offset still OK
      if (debug > 1 && reset < updated && lasttarget)
         warnx ("Target change to %.1lf - resetting", target);
      lasttarget = target;
      resetdata (resetlag);
   }
   if (lastf_rate != f_rate)
   {                            // Assume offset needs resetting
      if (debug > 1 && reset < updated && lastf_rate)
         warnx ("Fan change to %c - resetting", f_rate);
      lastf_rate = f_rate;
      resetoffset (resetlag);
   }
   if (lastmode != mode)
   {                            // Assume offset needs resetting
      if (debug > 1 && reset < updated && lastmode)
         warnx ("Mode change to %s - resetting", modename[mode]);
      lastmode = mode;
      resetoffset (resetlag);
   }
   if (!pow)
   {                            // Power off - assume offset needs resetting
      if (debug > 1 && reset < updated)
         warnx ("Power off - resetting");
      resetoffset (resetlag);
   }
   if (mode == 2 || mode == 6)
   {
      if (debug > 1 && reset < updated)
         warnx ("Mode %s - not running automatic control", modename[mode]);
      return;
   }
   // Default
   *stempp = target + offset;   // Default

   if (updated < reset)
   {                            // Waiting for startup or major change - reset data
      if (debug > 1)
         warnx ("Waiting to settle (%ds) %.1lf", (int) (reset - updated), atemp);
      overshootcheck ();
      return;
   }

   if (updated < nextsample)
   {                            // Waiting for next sample at sensible time
      overshootcheck ();
      return;
   }
   if (nextsample < updated - mqttperiod)
      nextsample = updated;
   nextsample += mqttperiod;

   t[sample++] = atemp;
   if (sample >= maxsamples)
      sample = 0;
   double min = 0,
      max = 0,
      ave = 0;
   int count = 0;
   for (s = 0; s < maxsamples; s++)
   {
      if (t[s] == -99)
         continue;
      if (!count || t[s] < min)
         min = t[s];
      if (!count || t[s] > max)
         max = t[s];
      ave += t[s];
      count++;
   }
   if (count < minsamples)
   {
      if (debug > 1)
         warnx ("Collecting samples (%d/%d) %.1lf", count, minsamples, atemp);
      overshootcheck ();
      return;
   }
   ave /= count;                // Use mean for drift logic

   if (overshootcheck ())
      return;                   // We have set a rate to try and stop the compressor

   // Adjust offset
   if (min > target || max < target)
   {                            // Step change
      double step = target - (min > target ? min : max);
      if (debug > 1)
         warnx ("Step change by %+.1lf", step);
      offset += step;
      resetdata (resetlag / 3);
   } else if (ave < target - ripple)
      offset += driftrate;
   else if (ave > target + ripple)
      offset -= driftrate;
   else
      offset *= driftback;

   // Check if we need to change mode
   if ((mode == 4 && offset <= -maxrheat) || (mode != 3 && mode != 4 && ave >= target))
   {
      if (debug > 1)
         warnx ("Changing to cool mode");
      mode = 3;                 // Heating and we are still too high so switch to cool
      resetoffset (resetlag);
   } else if ((mode == 3 && offset >= maxrcool) || (mode != 3 && mode != 4 && ave <= target))
   {
      if (debug > 1)
         warnx ("Changing to heat mode");
      mode = 4;                 // Cooling and we are still too low so switch to head
      resetoffset (resetlag);
   }

   // Limit offset
   if (mode == 4 && offset > maxfheat)
   {
      offset = maxfheat;
      if (f_rate == 'B')
      {
         if (debug > 1)
            warnx ("Changing to fan mode Auto");
         f_rate = 'A';          // Give up on night mode
         resetoffset (resetlag);
      }
   } else if (mode == 4 && offset < -maxrheat)
      offset = -maxrheat;
   else if (mode == 3 && offset < -maxfcool)
   {
      offset = -maxfcool;
      if (f_rate == 'B')
      {
         if (debug > 1)
            warnx ("Changing to fan mode Auto");
         f_rate = 'A';          // Give up on night mode
         resetoffset (resetlag);
      }
   } else if (mode == 3 && offset > maxrcool)
      offset = maxrcool;
   // Apply new temp
   stemp = target + offset;     // Apply offset
   if (debug > 1)
      warnx ("Temp %.1lf Mode %s F_rate %c Target %.1lf Offset %+.2lf Ave %.2lf(%d) Min %.1lf Max %.1lf", atemp,
             modename[mode], f_rate, target, offset, ave, count, min, max);
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
   int maxcmpfreq = 100;
   int svgl = 2;                // Low C
   int svgt = 32;               // High C
   int svgc = 25;               // Per C spacing height
   int svgh = 60;               // Per hour spacing width
   int svgwidth = 24 * svgh;
   int svgheight = (svgt - svgl) * svgc;
#ifdef LIBSNMP
   const char *atempoid = "iso.3.6.1.4.1.42814.14.3.5.1.0";     // The nono temp sensors default
   const char *atempcommunity = "public";
   const char *atemphost = NULL;
#endif
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
         { "mqtt-debug", 0, POPT_ARG_NONE, &mqttdebug, 0, "Debug"},
	 { "mqtt-atemp", 0, POPT_ARG_STRING , &mqttatemp, 0, "MQTT topic to subscribe for setting atemp (default cmnd/[topic]/atemp)", "topic"},
         { "max-samples", 0, POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &maxsamples, 0, "Max samples used for averaging", "N"},
         { "min-samples", 0, POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &minsamples, 0, "Min samples used for averaging", "N"},
         { "lock", 0, POPT_ARG_NONE, &dolock, 0, "Lock operation"},
#endif
#ifdef LIBSNMP
	 { "atemp-oid", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &atempoid, 0, "SNMP temperature OID","OID"},
	 { "atemp-community", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &atempcommunity, 0, "SNMP temperature community","community"},
	 { "atemp-host", 0, POPT_ARG_STRING , &atemphost, 0, "SNMP temperature Hostname","Host/IP"},
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
               warnx ("Fail %s", url);
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
            xml_addf (svg, "@width", "%d", svgwidth + 1);
            xml_addf (svg, "@height", "%d", svgheight + maxcmpfreq + 1);        // Allow for mompow and cmpfreq
            // Graph data
            int lastmode = 0,
               lasty = 0;
            double x = 0,
               stempref = -1;
            char lastf_rate = 0;
            size_t atemplen = 0,
               htemplen = 0,
               otemplen = 0,
               mompowlen = 0,
               cmpfreqlen = 0,
               heatlen = 0,
               heatblen = 0,
               coollen = 0,
               coolblen = 0,
               dt1len = 0;
            char *atempbuf = NULL,
               *htempbuf = NULL,
               *otempbuf = NULL,
               *mompowbuf = NULL,
               *cmpfreqbuf = NULL,
               *heatbuf = NULL,
               *heatbbuf = NULL,
               *coolbuf = NULL,
               *coolbbuf = NULL,
               *dt1buf = NULL;
            char atempm = 'M',
               htempm = 'M',
               otempm = 'M',
               mompowm = 'M',
               cmpfreqm = 'M',
               dt1m = 'M';
            FILE *atemp = open_memstream (&atempbuf, &atemplen);
            FILE *htemp = open_memstream (&htempbuf, &htemplen);
            FILE *otemp = open_memstream (&otempbuf, &otemplen);
            FILE *mompow = open_memstream (&mompowbuf, &mompowlen);
            FILE *cmpfreq = open_memstream (&cmpfreqbuf, &cmpfreqlen);
            FILE *dt1 = open_memstream (&dt1buf, &dt1len);
            FILE *heat = open_memstream (&heatbuf, &heatlen);
            FILE *heatb = open_memstream (&heatbbuf, &heatblen);
            FILE *cool = open_memstream (&coolbuf, &coollen);
            FILE *coolb = open_memstream (&coolbbuf, &coolblen);
            SQL_RES *res = sql_safe_query_store_free (&sql,
                                                      sql_printf ("SELECT * FROM `%#S` WHERE `Updated` LIKE '%#S%%' AND `IP`=%#s",
                                                                  table, svgdate, ip));
            while (sql_fetch_row (res))
            {
               char *v = sql_col (res, "Updated");
               if (strlen (v) < 19)
                  continue;
               x = (double) (atoi (v + 11) * 3600 + atoi (v + 14) * 60 + atoi (v + 17)) * svgh / 3600;
               double d;
               v = sql_col (res, "atemp");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (atemp, "%c%.2lf,%d", atempm, x, (int) (svgheight - (d - svgl) * svgc));
                  atempm = 'L';
               }
               v = sql_col (res, "htemp");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (htemp, "%c%.2lf,%d", htempm, x, (int) (svgheight - (d - svgl) * svgc));
                  htempm = 'L';
               }
               v = sql_col (res, "otemp");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (otemp, "%c%.2lf,%d", otempm, x, (int) (svgheight - (d - svgl) * svgc));
                  otempm = 'L';
               }
               v = sql_col (res, "mompow");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (mompow, "%c%.2lf,%d", mompowm, x, (int) (svgheight + d));
                  mompowm = 'L';
               }
               v = sql_col (res, "cmpfreq");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (cmpfreq, "%c%.2lf,%d", cmpfreqm, x, (int) (svgheight + maxcmpfreq - d));
                  cmpfreqm = 'L';
               }
               v = sql_col (res, "dt1");
               if (v)
               {
                  d = strtod (v, NULL);
                  fprintf (dt1, "%c%.2lf,%d", dt1m, x, (int) (svgheight - (d - svgl) * svgc));
                  dt1m = 'L';
               }
               v = sql_col (res, "stemp");
               char f_rate = *sql_colz (res, "f_rate");
               int pow = atoi (sql_colz (res, "pow"));
               int mode = atoi (sql_colz (res, "mode"));
               if (!pow)
                  mode = -1;    // Not on
               if ((lastf_rate != f_rate || mode != lastmode) && stempref >= 0)
               {                // Close box
                  if (lastmode == 3)
                     fprintf (lastf_rate == 'B' ? coolb : cool, "L%.2lf,%dL%.2lf,%dL%.2lf,%dZ", x, lasty, x, 0, stempref, 0);
                  if (lastmode == 4)
                     fprintf (lastf_rate == 'B' ? heatb : heat, "L%.2lf,%dL%.2lf,%dL%.2lf,%dZ", x, lasty, x, svgheight, stempref,
                              svgheight);
                  stempref = -1;
               }
               if (mode == 3 || mode == 4)
               {
                  FILE *f = (mode == 3 ? f_rate == 'B' ? coolb : cool : f_rate == 'B' ? heatb : heat);
                  d = strtod (v, NULL);
                  if (stempref >= 0)
                     fprintf (f, "L%.2lf,%dL", x, lasty);
                  else
                     fprintf (f, "M");
                  fprintf (f, "%.2lf,%d", x, lasty = (int) (svgheight - (d - svgl) * svgc));
                  if (stempref < 0)
                     stempref = x;
               }
               lastmode = mode;
               lastf_rate = f_rate;
            }
            x += svgh / 60;     // Assume minute stats to draw last bar
            if (lastmode == 3)
               fprintf (lastf_rate == 'B' ? coolb : cool, "L%.2lf,%dL%.2lf,%dL%.2lf,%dZ", x, lasty, x, 0, stempref, 0);
            if (lastmode == 4)
               fprintf (lastf_rate == 'B' ? heatb : heat, "L%.2lf,%dL%.2lf,%dL%.2lf,%dZ", x, lasty, x, svgheight, stempref,
                        svgheight);
            fclose (atemp);
            fclose (htemp);
            fclose (otemp);
            fclose (mompow);
            fclose (cmpfreq);
            fclose (dt1);
            fclose (heat);
            fclose (heatb);
            fclose (cool);
            fclose (coolb);
            xml_addf (svg, "+path@fill=red@stroke=none@opacity=0.5@d", heatbuf);
            xml_addf (svg, "+path@fill=red@stroke=none@opacity=0.25@d", heatbbuf);
            xml_addf (svg, "+path@fill=blue@stroke=none@opacity=0.5@d", coolbuf);
            xml_addf (svg, "+path@fill=blue@stroke=none@opacity=0.25@d", coolbbuf);
            xml_addf (svg, "+path@fill=none@stroke=red@stroke-linecap=round@stroke-linejoin=round@d", atempbuf);
            xml_addf (svg, "+path@fill=none@stroke=green@stroke-linecap=round@stroke-linejoin=round@d", htempbuf);
            xml_addf (svg, "+path@fill=none@stroke=blue@stroke-linecap=round@stroke-linejoin=round@d", otempbuf);
            xml_addf (svg, "+path@fill=none@stroke=black@stroke-linecap=round@stroke-linejoin=round@d", mompowbuf);
            xml_addf (svg, "+path@fill=none@stroke=green@@opacity=0.5@stroke-linecap=round@stroke-linejoin=round@d", cmpfreqbuf);
            xml_addf (svg, "+path@fill=none@stroke=black@stroke-dasharray=1@d", dt1buf);
            free (atempbuf);
            free (htempbuf);
            free (otempbuf);
            free (mompowbuf);
            free (cmpfreqbuf);
            free (dt1buf);
            free (heatbuf);
            free (heatbbuf);
            free (coolbuf);
            free (coolbbuf);
            {
               // Time of day and headings
               int x,
                 y;
               for (x = 0; x < svgwidth + 1; x += svgh)
               {
                  xml_addf (svg, "+path@stroke=grey@fill=none@opacity=0.5@stroke-dasharray=1@stroke-width=0.5@d", "M%d 0v%d", x,
                            svgheight + maxcmpfreq);
                  xml_t t = xml_addf (svg, "+text", "%02d", (x / svgh) % 24);
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

#ifdef	LIBSNMP
      struct snmp_session session;
      struct snmp_session *sess_handle;
      struct snmp_pdu *pdu;
      struct snmp_pdu *response;


      oid id_oid[MAX_OID_LEN];

      size_t id_len = MAX_OID_LEN;

      if (atemphost)
      {

         init_snmp ("Temp Check");

         snmp_sess_init (&session);
         session.version = SNMP_VERSION_2c;
         session.community = (unsigned char *) atempcommunity;
         session.community_len = strlen ((char *) session.community);
         session.peername = (char *) atemphost;
         sess_handle = snmp_open (&session);

      }
#endif

      int changed = 0;
      char *sensor = NULL;
      char *control = NULL;
#ifdef	LIBMQTT
      int thispow = 0;
      int thismompow = 0;
      int thiscmpfreq = 0;
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
         thiscmpfreq = 0;
#endif
#ifdef	LIBSNMP
         if (atemphost)
         {
            pdu = snmp_pdu_create (SNMP_MSG_GET);
            read_objid (atempoid, id_oid, &id_len);
            snmp_add_null_var (pdu, id_oid, id_len);

            int status = snmp_synch_response (sess_handle, pdu, &response);
            if (status)
            {
               warnx ("SNMP error (%s): %s", atemphost, snmp_api_errstring (status));
               snmp_free_pdu (pdu);
            } else
            {                   // Got reply
               struct variable_list *vars;
               for (vars = response->variables; vars; vars = vars->next_variable)
               {
                  char temp[30];
                  int l = snprint_value (temp, sizeof (temp), vars->name, vars->name_length, vars);
                  if (l > 0)
                  {
                     if (!strncmp (temp, "STRING: \"", 9))
                     {          // Really, this is crap!
                        atemp = strtod (temp + 9, NULL);
                        atempset = time (0);
                        if (debug)
                           warnx ("atemp=%.1lf (SNMP)", atemp);
                     } else
                        warnx ("Unexpected value: %s", temp);
                  } else
                     warnx ("Bad value from SNMP");
               }
               snmp_free_pdu (response);
            }
         }
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
            } else if (!strcmp (tag, "cmpfreq"))
               thiscmpfreq = atoi (val);
            else if (!strcmp (tag, "mompow"))
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
         if (ok)
            free (ok);
      }

      void updatedb (void)
      {
#ifdef SQLLIB
         if (!db)
            return;
         sql_string_t s = {
         };
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
               atemp = strtod (v, NULL);
               v = sql_col (res, "stemp");
               if (!v)
                  continue;
               double stemp = strtod (v, NULL);
               v = sql_col (res, "dt1");
               if (!v)
                  continue;
               double target = strtod (v, NULL);
               v = sql_col (res, "cmpfreq");
               if (!v)
                  continue;
               double cmpfreq = strtod (v, NULL);
               int mode = atoi (sql_colz (res, "mode"));
               char f_rate = *sql_colz (res, "f_rate");
               atempset = xml_time (sql_colz (res, "updated"));
               int mompow = atoi (sql_colz (res, "mompow"));
               int pow = atoi (sql_colz (res, "pow"));
               doauto (&stemp, &f_rate, &mode, pow, cmpfreq, mompow, atempset, atemp, target);
            }
            sql_free_result (res);
         }
#endif
         time_t next = time (0) / mqttperiod * mqttperiod + mqttperiod;
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
            if (debug)
               warnx ("MQTT subscribed to: [%s]", sub);
            if (mqttatemp)
            {
               int e = mosquitto_subscribe (mqtt, NULL, mqttatemp, 0);
               if (e)
                  errx (1, "MQTT subscribe failed %s", mosquitto_strerror (e));
               if (debug)
                  warnx ("MQTT subscribed to: [%s]", mqttatemp);
            }
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
            int l = msg->payloadlen;
            char *p = msg->payload;
            if (l > 2 && *p == '"' && p[l - 1] == '"')
            {
               p++;
               l -= 2;
            }
            char *val = malloc (l + 1);
            memcpy (val, p, l);
            val[l] = 0;
            if (mqttatemp && !strcmp (topic, mqttatemp))
            {                   // Direct atemp topic set
               atemp = strtod (val, NULL);
               next = atempset = time (0);
               if (debug)
                  warnx ("atemp=%.1lf (MQTT)", atemp);
            } else
            {
               l = strlen (mqttcmnd);
               if (strncmp (topic, mqttcmnd, l) || topic[l] != '/')
                  return;
               topic += l + 1;
               l = strlen (mqtttopic);
               if (strncmp (topic, mqtttopic, l) || topic[l] != '/')
                  return;
               topic += l + 1;
               if (getstatus ())
               {
                  updatestatus ();
#define	c(x,t,v) if(!strcmp(#x,topic)){if(val&&(!x||strcmp(x,val))){if(x)free(x);x=strdup(val);changed=1;}}
                  controlfields;
#undef c
                  if (!mqttatemp && !strcmp (topic, "atemp"))
                  {
                     atemp = strtod (val, NULL);
                     next = atempset = time (0);
                     if (debug)
                        warnx ("atemp=%.1lf (MQTT)", atemp);
                  }
                  if (topic[0] == 'd' && topic[1] == 't' && isdigit (topic[2]) && !topic[3])
                  {             // Special case, setting dtN means setting a mode and stemp
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
                     if (ok)
                        free (ok);
                     if (mode && atoi (mode) && atoi (mode) != atoi (topic + 2))
                        changed = 1;    // Force setting back to right mode
                  }
                  if (changed)
                     updatesettings (sensor, control);
               }
               freestatus ();
            }
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
                     doauto (&newstemp, &newf_rate, &newmode, thispow, thiscmpfreq, thismompow, atempset, atemp, thisdt1);
                     if (newstemp)
                     {          // Rounding temp to 0.5C with error dither
                        static double dither = 0;
                        static double lasterr = 0;
                        static time_t lastset = 0;
                        double rtemp = newstemp;
                        if (!lastset)
                           lastset = now;
                        dither += lasterr * (now - lastset) / mqttperiod;
                        newstemp = round ((newstemp - dither) * 2) / 2; // It gets upset if not .0 or .5
                        lasterr = newstemp - rtemp;
                        lastset = now;
                        if (debug)
                           warnx ("Set %.2lf as %.1lf dither error was %+.2lf", rtemp, newstemp, dither);
                     } else
                     {          // Compressor stop
                        newstemp = (newmode == 4 ? mintemp : maxtemp);
                        next = now + 10;        // Re check that it stopped
                        if (debug)
                           warnx ("Compressor stop at %.1lf", atemp);
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
                     xml_addf (stat, "@atemp", "%.1lf", atemp);
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
               } else
                  next = now;   // Try again!
               freestatus ();
               to = next - now;
            }
            if (to < 1)
               to = 1;
            e = mosquitto_loop (mqtt, to * 1000, 1);
            if (e)
               errx (1, "MQTT loop failed %s (to %d)", mosquitto_strerror (e), to * 1000);
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
#ifdef	LIBSNMP
      if (atemphost)
      {
         snmp_free_pdu (response);
         snmp_close (sess_handle);
      }
#endif
   }

   return 0;
}
