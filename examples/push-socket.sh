#!/usr/bin/env bash
# Standalone mode: otelfwd runs without STDIN ("otelfwd -nostdin") and receives records on a socket.
# The script starts it, sends sample-records.jsonl (one JSON record per line), and stops it again.
#
#   bash push-socket.sh                  TCP on 127.0.0.1:4390. Needs nothing but bash
#   bash push-socket.sh unix             a Unix socket. Needs socat or nc (netcat with -U)
#
#   OTELFWD_EXAMPLE_PORT=4400 bash push-socket.sh      another TCP port
#
# The forwarder needs an OTLP endpoint (OTLP_PUSH_API_URL), because the received records are pushed there.
#
# The same by hand, in two terminals:
#
#   OTLP_PUSH_API_URL=http://127.0.0.1:4318/v1/logs OTELFWD_TCP_LISTEN=127.0.0.1:4390 ../otelfwd -nostdin
#   cat sample-records.jsonl > /dev/tcp/127.0.0.1/4390         (bash)     or:  nc 127.0.0.1 4390 < sample-records.jsonl

. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

MODE="${1:-tcp}"
PORT="${OTELFWD_EXAMPLE_PORT:-4390}"
INPUT="${2:-$EXAMPLES_DIR/sample-records.jsonl}"

case "$MODE" in
  tcp)
    export OTELFWD_TCP_LISTEN="127.0.0.1:$PORT"
    ;;
  unix)
    export OTELFWD_UNIX_SOCKET="$WORK/otelfwd.sock"
    if ! command -v socat >/dev/null 2>&1 && ! command -v nc >/dev/null 2>&1; then
      echo "The unix example needs socat or nc (netcat). Use the tcp example instead: bash push-socket.sh" >&2
      exit 2
    fi
    ;;
  *)
    echo "usage: push-socket.sh [tcp|unix] [file]" >&2
    exit 2
    ;;
esac

echo "mode     : $MODE ${OTELFWD_TCP_LISTEN:-$OTELFWD_UNIX_SOCKET}"
echo "input    : $INPUT ($(wc -l < "$INPUT" | tr -d ' ') records)"
echo "endpoint : $OTLP_PUSH_API_URL"
echo

"$OTELFWD_BIN" -nostdin &
FWD_PID=$!

# Wait until the forwarder accepts connections
READY=0
for _ in $(seq 1 50)
do
  if [ "$MODE" = "tcp" ]; then
    (exec 3<>"/dev/tcp/127.0.0.1/$PORT") 2>/dev/null && READY=1
  else
    [ -S "$OTELFWD_UNIX_SOCKET" ] && READY=1
  fi

  [ "$READY" = "1" ] && break
  kill -0 "$FWD_PID" 2>/dev/null || break
  sleep 0.1
done

if [ "$READY" != "1" ]
then
  echo "The forwarder did not start (is port $PORT free?)." >&2
  exit 1
fi

# Send the records: one connection, closed at the end of the file
if [ "$MODE" = "tcp" ]; then
  cat "$INPUT" > "/dev/tcp/127.0.0.1/$PORT"
elif command -v socat >/dev/null 2>&1; then
  socat -u "OPEN:$INPUT" "UNIX-CONNECT:$OTELFWD_UNIX_SOCKET"
else
  nc -N -U "$OTELFWD_UNIX_SOCKET" < "$INPUT"
fi

# Give the forwarder a moment to read the data, then stop it with SIGTERM. It pushes what it received before it ends.
sleep 1
kill -TERM "$FWD_PID"
wait "$FWD_PID"
FWD_PID=""

echo
echo "records accepted on the socket  : $(metric "socket_lines_total{source=\"$([ "$MODE" = tcp ] && echo tcp || echo unix)\",result=\"accepted\"}")"
report
