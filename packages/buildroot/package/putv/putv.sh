#!/bin/sh
#
# Starts putv.
#

BINDIR="/usr/bin/"
DAEMON="putv"
PIDFILE="/var/run/$DAEMON.pid"
MEDIA="file:///media"
WEBSOCKETDIR="/var/run/websocket"
USER="www-data"
LOGFILE="/var/log/$DAEMON.log"
FILTER="pcm?stereo"
OUTPUT=""

OPTIONS=""
[ -r "/etc/default/$DAEMON" ] && . "/etc/default/$DAEMON"

prepare() {
	if echo $MEDIA | grep -q db:// ; then
		DBFILE=$(echo $MEDIA | cut -b 6-)
		MUSICDIR=$(dirname $DBFILE)
		NEWFILES=$(find ${MUSICDIR}/ -newer $DBFILE -iname "*.mp3")
		NEWFILES="$NEWFILES $(find ${MUSICDIR}/ -newer $DBFILE -iname "*.flac")"
		echo $NEWFILES
	fi
}

start() {
#	OPTIONS="${OPTIONS} -m ${MEDIA}"
#	OPTIONS="${OPTIONS} -a -l -r"
# 	OPTIONS="${OPTIONS} -a -r"
	OPTIONS="${OPTIONS} -R ${WEBSOCKETDIR}"
#	OPTIONS="${OPTIONS} -u ${USER}"
	OPTIONS="${OPTIONS} -L ${LOGFILE}"
	OPTIONS="${OPTIONS} -p ${PIDFILE}"
	OPTIONS="${OPTIONS} -f ${FILTER}"
	if [ "${PRIORITY}" != "" ]; then
		OPTIONS="${OPTIONS} -P ${PRIORITY}"
	fi
	if [ "${OUTPUT}" != "" ]; then
		OPTIONS="${OPTIONS} -o ${OUTPUT}"
	fi
	OPTIONS="${OPTIONS} -D"

	if [ -e /etc/gpiod/rules.d/putv.conf ] && [ -z "$GPIO" ]; then
		exit 0
	fi
	printf 'Starting %s: ' "$DAEMON"
	start-stop-daemon -S -q -x "${BINDIR}${DAEMON}" \
		-- $OPTIONS
	status=$?
	if [ "$status" -eq 0 ]; then
		echo "OK"
	else
		echo "FAIL"
	fi
	return "$status"
}

stop() {
	printf 'Stopping %s: ' "$DAEMON"
	start-stop-daemon -K -q -p "$PIDFILE"
	status=$?
	if [ "$status" -eq 0 ]; then
		rm -f "$PIDFILE"
		echo "OK"
	else
		echo "FAIL"
	fi
	return "$status"
}

restart() {
	stop
	sleep 1
	start
}

case "$1" in
	start|stop|restart|prepare)
		"$1";;
	reload)
		# Restart, since there is no true "reload" feature.
		restart;;
	*)
		echo "Usage: $0 {start|stop|restart|reload}"
		exit 1
esac
