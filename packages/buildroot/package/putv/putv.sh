#!/bin/sh
#
# Starts putv.
#

BINDIR="/usr/bin/"
DAEMON="putv"
PIDFILE="/var/run/$DAEMON.pid"
WEBAPPDIR="/srv/www-putv/htdocs"
WEBSOCKETDIR="/var/run/websocket"
WEBSOCKETNAME=$DAEMON
LOGFILE="/var/log/$DAEMON.log"
FILTER="pcm?stereo"
USER=
MEDIA=
SERVER=0
OUTPUT=""

OPTIONS=""
if [ -r "/etc/default/$DAEMON" ]; then
	. "/etc/default/$DAEMON"
else
	[ -r "/etc/default/$0" ] && . "/etc/default/$0"
fi

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
	if [ "${MEDIA}" != "" ]; then
		OPTIONS="${OPTIONS} -m ${MEDIA}"
#		OPTIONS="${OPTIONS} -a -l -r"
		OPTIONS="${OPTIONS} -a"
	fi
	OPTIONS="${OPTIONS} -R ${WEBSOCKETDIR}"
	OPTIONS="${OPTIONS} -n ${WEBSOCKETNAME}"
	if [ -d ${WEBAPPDIR} ]; then
		OPTIONS="${OPTIONS} -d ${WEBAPPDIR}"
	fi
	if [ "${USER}" != "" ]; then
		OPTIONS="${OPTIONS} -u ${USER}"
	fi
	OPTIONS="${OPTIONS} -L ${LOGFILE}"
	OPTIONS="${OPTIONS} -f ${FILTER}"
	if [ "${PRIORITY}" != "" ]; then
		OPTIONS="${OPTIONS} -P ${PRIORITY}"
	fi
	if [ "${OUTPUT}" != "" ]; then
		OPTIONS="${OPTIONS} -o ${OUTPUT}"
	fi
	mkdir -p /tmp/run/wesocket
	if [ ! -e ${WEBSOCKETDIR} ]; then
		ln -s /tmp/run/wesocket ${WEBSOCKETDIR}
	fi
	rm -f ${WEBSOCKETDIR}/${WEBSOCKETNAME}

	if [ -e /etc/gpiod/rules.d/putv.conf ] && [ -z "$GPIO" ]; then
		exit 0
	fi
	printf 'Starting %s: ' "$DAEMON"
	start-stop-daemon -b -m -p ${PIDFILE} -S -q -x "${BINDIR}${DAEMON}" \
		-- $OPTIONS
	status=$?
	if [ "$status" -eq 0 ]; then
		echo "OK"
	else
		echo "FAIL"
	fi
	chmod a+rwx ${WEBSOCKETDIR}
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
