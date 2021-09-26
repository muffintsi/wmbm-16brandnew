#!/bin/sh

[ ! -d /wmbusmeters_data/logs/meter_readings ] && mkdir -p /wmbusmeters_data/logs/meter_readings
[ ! -d /wmbusmeters_data/etc/wmbusmeters.d ] && mkdir -p /wmbusmeters_data/etc/wmbusmeters.d
[ ! -f /wmbusmeters_data/etc/wmbusmeters.conf ] && echo -e "loglevel=normal\ndevice=auto\nlistento=t1\nlogtelegrams=false\nformat=json\nmeterfiles=/wmbusmeters_data/logs/meter_readings\nmeterfilesaction=overwrite\nlogfile=/wmbusmeters_data/logs/wmbusmeters.log" > /wmbusmeters_data/etc/wmbusmeters.conf

/wmbusmeters/wmbusmeters --useconfig=/wmbusmeters_data
