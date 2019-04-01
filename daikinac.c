// Daikin A/C control / monitoring

#include <stdio.h>
#include <string.h>
#include <popt.h>
#include <time.h>
#include <sys/time.h>
#include <syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <stdlib.h>
#include <signal.h>
#include <sys/wait.h>
#include <unistd.h>
#include <ctype.h>
#include <err.h>
#include <signal.h>
#include <curl/curl.h>
#ifdef SQLLIB
#include <sqllib.h>
#else
int sqldebug = 0;               // General debug
#endif
#ifdef LIBMQTT
#include <mosquitto.h>
#endif

#define	controlfields			\
	c(pow, Power, 0/1)		\
	c(mode, Mode, 1-7)		\
	c(stemp, Temp, C)		\
	c(shum, Numidity, %)		\
	c(f_rate, Fan, A/B/3-7)		\
	c(f_dir, Fan dir, 0-3)		\

int
main (int argc, const char *argv[])
{
#define c(x,t,v) char *set##x=NULL;     // Args
   controlfields
#undef	c
#ifdef SQLLIB
   const char *db = NULL;
#endif
   const char *table = "daikin";
   char *atemp = NULL;
   int info = 0,
      hotcold = 0,
      modeoff = 0,
      modeon = 0,
      modeauto = 0,
      modeheat = 0,
      modecool = 0,
      modedry = 0,
      modefan = 0;
   double hdelta = 4,           // Auto delta internal (allows for wrong reading as own heating/cooling impacts it)
      odelta = 0,               // Auto delta external (main criteria for hot-cold control)
      adelta = 1;               // Auto air temp delta
#ifdef LIBMQTT
   int mqttreport = 60;
   const char *mqttid = NULL;
   const char *mqtthost = NULL;
   const char *mqttuser = NULL;
   const char *mqttpass = NULL;
   const char *mqtttopic = "diakin";
   const char *mqttcmnd = "cmnd";
   const char *mqttstat = "stat";
#endif
   {                            // POPT
      poptContext optCon;       // context for parsing command-line options
      const struct poptOption optionsTable[] = {
#define	c(x,t,v)	{#x, 0, POPT_ARG_STRING, &set##x, 0, #t, #v},
         controlfields
#undef c
#ifdef SQLLIB
         {"log", 'l', POPT_ARG_STRING, &db, 0, "Log", "database"},
#endif
#ifdef LIBMQTT
         {"mqtt-host", 'h', POPT_ARG_STRING, &mqtthost, 0, "MQTT host", "hostname"},
         {"mqtt-id", 0, POPT_ARG_STRING, &mqttid, 0, "MQTT id", "id"},
         {"mqtt-user", 0, POPT_ARG_STRING, &mqttuser, 0, "MQTT user", "username"},
         {"mqtt-pass", 0, POPT_ARG_STRING, &mqttpass, 0, "MQTT pass", "password"},
         {"mqtt-topic", 't', POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &mqtttopic, 0, "MQTT topic", "topic"},
         {"mqtt-cmnd", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &mqttcmnd, 0, "MQTT cmnd prefix", "prefix"},
         {"mqtt-stat", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &mqttstat, 0, "MQTT stat prefix", "prefix"},
         {"mqtt-repot", 0, POPT_ARG_INT | POPT_ARGFLAG_SHOW_DEFAULT, &mqttreport, 0, "MQTT reporting interval", "seconds"},
#endif
         {"hot-cold", 'a', POPT_ARG_NONE, &hotcold, 0, "Auto set hot/cold"},
         {"atemp", 0, POPT_ARG_STRING, &atemp, 0, "Air temp", "C"},
         {"on", 0, POPT_ARG_NONE, &modeon, 0, "On"},
         {"off", 0, POPT_ARG_NONE, &modeoff, 0, "Off"},
         {"auto", 'A', POPT_ARG_NONE, &modeauto, 0, "Auto"},
         {"heat", 'H', POPT_ARG_NONE, &modeheat, 0, "Heat"},
         {"cool", 'C', POPT_ARG_NONE, &modecool, 0, "Cool"},
         {"dry", 'D', POPT_ARG_NONE, &modedry, 0, "Dry"},
         {"fan", 'F', POPT_ARG_NONE, &modefan, 0, "Fan"},
         {"table", 0, POPT_ARG_STRING | POPT_ARGFLAG_SHOW_DEFAULT, &table, 0, "Table", "table"},
         {"info", 'i', POPT_ARG_NONE, &info, 0, "Show info"},
         {"hdelta", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &hdelta, 0, "Inside delta for auto", "C"},
         {"odelta", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &odelta, 0, "Outside delta for auto", "C"},
         {"adelta", 0, POPT_ARG_DOUBLE | POPT_ARGFLAG_SHOW_DEFAULT, &adelta, 0, "Air temp delta for auto", "C"},
         {"debug", 'v', POPT_ARG_NONE, &sqldebug, 0, "Debug"},
         POPT_AUTOHELP {}
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
      // Grrr, snmpget is a pain
      if (atemp && *atemp == '"' && atemp[1] && atemp[strlen (atemp) - 1] == '"')
      {                         // Strip "
         atemp++;
         atemp[strlen (atemp) - 1] = 0;
      }

      CURL *curl = curl_easy_init ();

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
            warnx ("Result %ld for %s\n", code, url);
            if (reply)
               free (reply);
            return NULL;
         }
         if (sqldebug)
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

      int changed = 0;
      int otemp = 0,
         htemp = 0,
         temp = 0;
      char *sensor = NULL;
      char *control = NULL;
      const char *ip;
#define c(x,t,v) char *x=NULL;  // Current settings
      controlfields
#undef	c
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
      // Get status
      int getstatus (void)
      {
         // Reset
         changed = 0;
         otemp = 0;
         htemp = 0;
         temp = 0;
         char *url;
         if (asprintf (&url, "http://%s/aircon/get_sensor_info", ip) < 0)
            errx (1, "malloc");
         sensor = get (url);
         if (asprintf (&url, "http://%s/aircon/get_control_info", ip) < 0)
            errx (1, "malloc");
         control = get (url);
         return sensor && control;
      }
      void freestatus (void)
      {
         if (sensor)
            free (sensor);
         if (control)
            free (control);
#define	c(x,t,v) if(x)free(x);x=NULL;
         controlfields
#undef c
      }
      // Update status
      void updatestatus ()
      {
         void check (char *tag, char *val)
         {
            if (info && strcmp (tag, "ret"))
               printf ("%s\t%s\n", tag, val);
#define	c(x,t,v) if(!strcmp(#x,tag))if(val&&(!x||strcmp(x,val))){changed=1;if(x)free(x);x=strdup(val);}
            controlfields
#undef c
               if (!strcmp (tag, "otemp"))
               otemp = strtod (val, NULL);
            if (!strcmp (tag, "htemp"))
               htemp = strtod (val, NULL);
            if (!strcmp (tag, "stemp"))
               temp = strtod (val, NULL);
         }
         scan (sensor, check);
         scan (control, check);
      }
      void updatesettings ()
      {                         // Set new control
         char *url = NULL;
         size_t len = 0;
         FILE *o = open_memstream (&url, &len);
         fprintf (o, "http://%s/aircon/set_control_info?", ip);
#define c(x,t,v) fprintf(o,"%s=%s&",#x,x);
         controlfields
#undef c
            fclose (o);
         url[--len] = 0;
         char *ok = get (url);
         free (ok);
      }
      void updatedb ()
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
            controlfields
#undef c
               sql_sprintf (&s, ",`%#S`=%#s", tag, val);
         }
         scan (sensor, update);
         scan (control, update);
         if (atemp && *atemp && sql_colnum (fields, "atemp") >= 0)
            sql_sprintf (&s, ",`atemp`=%#s", atemp);
         sql_safe_query_s (&sql, &s);
#endif
      }

#ifdef LIBMQTT
      if (mqtthost)
      {                         // Handling MQTT only
         ip = poptGetArg (optCon);
         if (poptPeekArg (optCon))
            errx (1, "One aircon only for MQTT operation");
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
            if (sqldebug)
               warnx ("MQTT connect %s for %s", mqtthost, sub);
            int e = mosquitto_subscribe (mqtt, NULL, sub, 0);
            if (e)
               errx (1, "MQTT subscribe failed %s", mosquitto_strerror (e));
            free (sub);
         }
         void disconnect (struct mosquitto *mqtt, void *obj, int rc)
         {
            obj = obj;
            rc = rc;
            if (sqldebug)
               warnx ("MQTT disconnect %s", mqtthost);
            e = mosquitto_reconnect (mqtt);
            if (e)
               errx (1, "MQTT reconnect failed (%s) %s", mqtthost, mosquitto_strerror (e));
         }
         void message (struct mosquitto *mqtt, void *obj, const struct mosquitto_message *msg)
         {
            obj = obj;
            char *topic = msg->topic;
            if (sqldebug)
               warnx ("MQTT message %s %.*s", topic, msg->payloadlen, (char *) msg->payload);
            int l = strlen (mqttcmnd);
            if (strncmp (topic, mqttcmnd, l) || topic[l] != '/')
               return;
            topic += l + 1;
            l = strlen (mqtttopic);
            if (strncmp (topic, mqtttopic, l) || topic[l] != '/')
               return;
            topic += l + 1;
            l = msg->payloadlen;
            char *val = malloc (l + 1);
            memcpy (val, msg->payload, l);
            if (getstatus ())
            {
               updatestatus ();
               val[l] = 0;
#define	c(x,t,v) if(!strcmp(#x,topic)){if(val&&(!x||strcmp(x,val))){if(x)free(x);x=strdup(val);changed=1;}}
               controlfields
#undef c
                  if (changed)
                  updatesettings (sensor, control);
            }
            free (val);
            freestatus ();
         }
         mosquitto_connect_callback_set (mqtt, connect);
         mosquitto_disconnect_callback_set (mqtt, disconnect);
         mosquitto_message_callback_set (mqtt, message);
         e = mosquitto_connect (mqtt, mqtthost, 1883, 60);
         if (e)
            errx (1, "MQTT connect failed (%s) %s", mqtthost, mosquitto_strerror (e));
         time_t next = time (0);;
         while (1)
         {
            time_t now = time (0);
            int to = next - now;
            if (to < 0)
            {                   // stat
               next += mqttreport;
               to = next - now;
               if (getstatus ())
               {
                  updatedb ();
                  void check (char *tag, char *val)
                  {
                     // Only some things we report
                     if (!strncmp (tag, "b_", 2)
                         || (strncmp (tag, "f_", 2) && !strstr (tag, "pow") && !strstr (tag, "temp") && strcmp (tag, "mode")
                             && !strstr (tag, "hum") && strcmp (tag, "adv")))
                        return;
                     char *topic = NULL;
                     asprintf (&topic, "%s/%s/%s", mqttstat, mqtttopic, tag);
                     e = mosquitto_publish (mqtt, NULL, topic, strlen (val), val, 0, 0);
                     if (sqldebug)
                        warnx ("Publish %s %s", topic, val);
                     free (topic);
                  }
                  scan (sensor, check);
                  scan (control, check);
               }
               freestatus ();
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

            if (hotcold)
            {                   // Temp control
               int oldmode = atoi (mode);
               if (oldmode != 2 && oldmode != 6)
               {
                  int newmode = 0;
                  if (atemp && *atemp)
                  {             // Use air temp as reference
                     double air = strtod (atemp, NULL);
                     if (air >= temp + adelta)
                        newmode = 3;    // force cool
                     else if (air < temp - adelta)
                        newmode = 4;    // force heat
                  } else
                  {             // Use outside or inside temp as reference
                     if (htemp < temp - hdelta)
                        newmode = 4;    // force heat
                     else if (htemp > temp + hdelta)
                        newmode = 3;    // force cool
                     else if (otemp < temp - odelta)
                        newmode = 4;    // force heat
                     else if (otemp > temp + odelta)
                        newmode = 4;    // force heat
                  }
                  if (newmode && newmode != oldmode)
                  {
                     if (asprintf (&mode, "%d", newmode) < 0)
                        errx (1, "malloc");
                     changed = 1;
                  }
               }
            }

            if (changed)
               updatesettings (sensor, control);
            updatedb (sensor, control);
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
