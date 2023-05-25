#!/bin/sh
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

start_initcmd() {
  result=1
  if [ -e ${CCMDLINE_SCRIPT} ]; then
    ${CCMDLINE} -K -D ${OPTIONS_CLIENTS} -p ${PIDDIR}putv_cmdline.pid -i ${CCMDLINE_SCRIPT}
    result=$?
  fi
  return $result
}

start_inputcmd() {
  result=1
  if [ -c ${CINPUT_DEVICE} ]; then
    ${CINPUT} -D ${OPTIONS_CLIENTS} -p ${PIDDIR}putv_input.pid ${OPTIONS_CINPUT}
    result=$?
  fi
  return $result
}


start_display() {
  ${CDISPLAY} -D ${OPTIONS_CLIENTS} -p ${PIDDIR}putv_display.pid
}

stop_inputcmd() {
  if [ -e ${PIDDIR}putv_input.pid ]; then
	kill -9 $(cat ${PIDDIR}putv_input.pid )
  fi
}

stop_display() {
  if [ -e ${PIDDIR}putv_display.pid ]; then
	kill -9 $(cat ${PIDDIR}putv_display.pid )
  fi
}

stop_initcmd() {
  if [ -e ${PIDDIR}putv_cmdline.pid ]; then
	kill -9 $(cat ${PIDDIR}putv_cmdline.pid )
  fi
}

if [ -z "$CLIENTS" ]; then
  CLIENTS="initcmd inputcmd"
fi
case "$1" in
  start|stop)
    chmod a+rwx ${WEBSOCKETDIR}
    for client in $CLIENTS
    do
      $1_$client
    done
  ;;
  restart|reload)
    for client in $CLIENTS
    do
      stop_$client
      start_$client
    done
  ;;
  *)
    echo "Usage: $0 {start|stop|restart|reload}"
    exit 1
  ;;
esac
