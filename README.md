Command line control for WiFi connected Daikin air conditioning units.

Also, MQTT gateway allowing reporting and control via MQTT.

The MQTT gateway daemon mode also allows logging to database, and auto temperature control. The idea is you have a temp sensor feed real readings via MQTT and the mode (heat/cool) and target temp are fine tuned to keep the measured temperature very close to the target (the temp set in auto mode). In tests we have managed to keep within +/- 0.2C of target temperature. This also includes a flip between heat/cool when needed, but generally stays on one mode unless amiant temp changes drastically. This is way tighter than the auto mode provided by the a/c itself.

There is also an option to produce an SVG graph from the database logging.

Use --help to see args. Have fun.

