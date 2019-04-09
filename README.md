Control system for Daikin WiFi connected air conditioning units.

Simple command line to update settings, and get info.

Option to log settings and temperatures in mysql database.

Option to run as deamon as MQTT gateway, reporting settings and allowing changes.

Includes log to database every minute (or other period) (--log=database)
MQTT cmnd/[topic]/pow		0/1 for power
MQTT cmnd/[topic]/mode		1/2/3/4/6 for mode auto/dry/cool/heat/fan
MQTT cmnd/[topic]/stemp		Set target temp C
MQTT cmnd/[topic]/f_rate	A/B/3/4/5/6/7 for fan rate
MQTT cmnd/[topic]/f_dir		0/1/2/3 for fan direction
MQTT cmnd/[topic]/dt1		Change target temp for auto mode (used if atemp set)

Option to handle MQTT setting of separate air temperature
MQTT cmnd/[topic]/atemp to set actual temp in C. Ideally send every minute.
(if set, this sets heat/cool and adjusts target to make air temp match auto dt1 tempurature)

Option to build with snmp library and collect temperature directly every minute.

See --help for more info.
