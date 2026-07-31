#!/bin/sh

### BEGIN INIT INFO
# Provides:          aesdsocket
# Required-Start:    $remote_fs $syslog
# Required-Stop:     $remote_fs $syslog
# Default-Start:     2 3 4 5
# Default-Stop:      0 1 6
# Short-Description: Start aesdsocket at boot time
### END INIT INFO

case "$1" in
	start)
		echo "INIT: Starting AESD Socket Server!"
		start-stop-daemon -S -x /usr/bin/aesdsocket -- -d
		;;
	stop)
		echo "INIT: Stopping AESD Socker Server!"
		start-stop-daemon -K -x /usr/bin/aesdsocket
		;;
	*)
		echo "INIT: Wrong Argument passed. Valid Arguments: start/stop"
		;;
esac
