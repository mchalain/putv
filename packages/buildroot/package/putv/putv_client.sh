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
CINPUT_JSON=/etc/putv/config.json
CCMDLINE=${BINDIR}/putv_cmdline
CCMDLINE_SCRIPT=/etc/default/putv.cmdline

OPTIONS=""
[ -r "/etc/default/$DAEMON" ] && . "/etc/default/$DAEMON"
DAEMON=putv

OPTIONS_CLIENTS="-R ${WEBSOCKETDIR} -n ${DAEMON} -m ${CINPUT_JSON}"
OPTIONS_CINPUT="${OPTIONS_CINPUT} -i ${CINPUT_DEVICE}"

start() {
  chmod a+rwx ${WEBSOCKETDIR}
  #${CDISPLAY} -D ${OPTIONS_CLIENTS} -p ${PIDDIR}putv_display.pid
  if [ -e ${CCMDLINE_SCRIPT} ]; then
    ${CCMDLINE} -K -D ${OPTIONS_CLIENTS} -p ${PIDDIR}putv_cmdline.pid -i ${CCMDLINE_SCRIPT}
  fi
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
  if [ -e ${PIDDIR}putv_cmdline.pid ]; then
	kill -9 $(cat ${PIDDIR}putv_cmdline.pid )
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
