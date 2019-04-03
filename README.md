Control system for Daikin WiFi connected air conditioning units.

Simple command line to update settings.

Option to log settings and temperatures in mysql database.

Option to run as deamon as MQTT gateway
- Includes log to database every minute (or other period)

Option to handle MQTT setting of actual air temperature and adjust hot/cold settings to allow for temperature gradient/difference in the room.
MQTT cmnd/[topic]/atemp to set actual temp in C. Ideally send every minute.

See --help for more info.
