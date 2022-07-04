#
# Starts putv.
#

BINDIR="/usr/bin/"
PIDDIR="/var/run/"
DAEMON="putv"
WEBSOCKETDIR="/var/run/websocket"
USER="www-data"
CDISPLAY=${BINDIR}/putv_display
CINPUT=${BINDIR}/putv_input

OPTIONS=""
[ -r "/etc/default/$DAEMON" ] && . "/etc/default/$DAEMON"

OPTIONS_CLIENTS="-R ${WEBSOCKETDIR} -n ${DAEMON}"
OPTIONS_CINPUT="${OPTIONS_CINPUT} -i ${CINPUT_DEVICE} -m ${CINPUT_JSON}"

start() {
  chmod a+rwx ${WEBSOCKETDIR}
  #${CDISPLAY} -D ${OPTIONS_CLIENTS} -p ${PIDDIR}putv_display.pid
  if [ -c ${CINPUT_DEVICE} ]; then
    ${CINPUT} -D ${OPTIONS_CLIENTS} -p ${PIDDIR}putv_input.pid ${OPTIONS_CINPUT}
  fi
}

stop() {
  if [ -e ${PIDDIR}putv_display.pid ]; then
	kill -9 $(cat ${PIDDIR}putv_display.pid )
  fi
  if [ -e ${PIDDIR}putv_input.pid ]; then
	kill -9 $(cat ${PIDDIR}putv_input.pid )
  fi
}

case "$1" in
	start|stop)
		"$1";;
	restart)
		stop
		start
		;;
	reload)
		# Restart, since there is no true "reload" feature.
		restart;;
	*)
		echo "Usage: $0 {start|stop|restart|reload}"
		exit 1
esac
