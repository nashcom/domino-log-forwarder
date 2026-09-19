#!/usr/bin/env bash
# Starts everything for the NGINX test container:
#
#   1. creates the directory of the syslog socket, /tmp/otelfwd-syslog (owned by the user who runs otelfwd and the container)
#   2. starts otelfwd in standalone mode with the syslog socket /tmp/otelfwd-syslog/syslog.sock
#   3. waits until the socket exists
#   4. starts the NGINX test container (docker compose up --build)
#
#   bash run.sh             start everything and stay in the foreground. Ctrl-C stops the container, removes it
#                           (docker compose down) and stops the otelfwd which was started here
#   bash run.sh --detach    start everything in the background and return. otelfwd writes otelfwd-data/otelfwd.log
#   bash run.sh --stop      stop what --detach (or another run.sh) started: otelfwd, then docker compose down
#   bash run.sh --rebuild   first remove the image (docker compose down --rmi local) and build it again without the build cache
#   bash run.sh --clean     first delete the WAL and the metrics file of an earlier run (clean start for a load test)
#
# --detach, --rebuild and --clean can be combined.
#
# Settings (environment variables):
#
#   OTLP_PUSH_API_URL      where otelfwd pushes to. Default: http://127.0.0.1:4318/v1/logs (the OTLP test container in tools/otel-sink)
#   OTLP_PUSH_TOKEN        bearer token, if the receiver needs one
#   OTLP_CA_FILE           CA file for https://
#   OTELFWD_BIN            the forwarder. Default: ../otelfwd (built with "make"), otherwise otelfwd from the PATH
#   OTELFWD_SOCKET_DIR     directory of the syslog socket. It is mounted at /run/otelfwd in the container. Default: /tmp/otelfwd-syslog
#                          It has to be on a Linux file system. Sockets cannot be created on /mnt/c or /mnt/d in WSL.
#   OTELFWD_DATA_DIR       data directory of otelfwd (WAL, metrics). Default: otelfwd-data next to this script (not shared with another otelfwd)
#   OTELFWD_NGINX_PORT     port of the test NGINX on 127.0.0.1 of the host. Default: 18080
#
# The socket DIRECTORY is mounted into the container and not the socket file: otelfwd creates the file again every time it
# starts, and NGINX finds the new file on its own. A mounted file would keep pointing to the old one.

if [ -z "${BASH_VERSION:-}" ]; then
  echo "this script needs bash" >&2
  exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

CLEAN=0
DETACH=0
STOP=0
REBUILD=0

for Arg in "$@"; do
  case "$Arg" in
    --clean)   CLEAN=1 ;;
    --detach)  DETACH=1 ;;
    --stop)    STOP=1 ;;
    --rebuild) REBUILD=1 ;;
    *)
      echo "usage: bash run.sh [--detach] [--rebuild] [--clean] | --stop" >&2
      exit 2
      ;;
  esac
done

if [ "$STOP" = "1" ] && [ "$((CLEAN + DETACH + REBUILD))" != "0" ]; then
  echo "--stop cannot be combined with other options" >&2
  exit 2
fi

export OTLP_PUSH_API_URL="${OTLP_PUSH_API_URL:-http://127.0.0.1:4318/v1/logs}"
export OTELFWD_SOCKET_DIR="${OTELFWD_SOCKET_DIR:-/tmp/otelfwd-syslog}"
export OTELFWD_NGINX_PORT="${OTELFWD_NGINX_PORT:-18080}"
export OTELFWD_DATA_DIR="${OTELFWD_DATA_DIR:-$HERE/otelfwd-data}"
export BUILDKIT_PROGRESS="${BUILDKIT_PROGRESS:-plain}"

SOCKET="$OTELFWD_SOCKET_DIR/syslog.sock"
export OTELFWD_SYSLOG_SOCKET="$SOCKET"
PIDFILE="$OTELFWD_DATA_DIR/otelfwd.pid"
LOGFILE="$OTELFWD_DATA_DIR/otelfwd.log"
FWD_PID=""
COMPOSE_UP=0
KEEP=0


if ! command -v docker >/dev/null 2>&1; then
  echo "docker was not found" >&2
  exit 2
fi


# Stops an otelfwd by the pid in its pid file, if that pid still is an otelfwd. Nothing else is touched.
stop_pidfile_otelfwd()
{
  local Pid=""

  [ -f "$PIDFILE" ] || return 0

  Pid="$(cat "$PIDFILE" 2>/dev/null)"

  if [ -n "$Pid" ] && kill -0 "$Pid" 2>/dev/null && grep -q otelfwd "/proc/$Pid/comm" 2>/dev/null; then
    kill -TERM "$Pid" 2>/dev/null

    for _ in $(seq 1 100); do
      kill -0 "$Pid" 2>/dev/null || break
      sleep 0.1
    done

    echo "Stopped otelfwd (pid $Pid)"
  fi

  rm -f "$PIDFILE"
}


if [ "$STOP" = "1" ]; then
  stop_pidfile_otelfwd
  cd "$HERE" || exit 1
  docker compose down
  exit 0
fi


if [ -z "${OTELFWD_BIN:-}" ]; then
  if [ -x "$HERE/../otelfwd" ]; then
    OTELFWD_BIN="$HERE/../otelfwd"
  elif command -v otelfwd >/dev/null 2>&1; then
    OTELFWD_BIN="$(command -v otelfwd)"
  else
    echo "otelfwd was not found. Build it with \"make\" in the repository, or set OTELFWD_BIN." >&2
    exit 2
  fi
fi

# An otelfwd from before the syslog input ignores OTELFWD_SYSLOG_SOCKET and would use the default socket of the Domino add-in instead
# (otelfwd needs an existing data directory even for -help, and the one of this script is created further down: use /tmp for this check)
if ! OTELFWD_DATA_DIR=/tmp "$OTELFWD_BIN" -help 2>&1 | grep -q OTELFWD_SYSLOG_SOCKET; then
  echo "$OTELFWD_BIN has no syslog input. It was built before the syslog input was added. Build it again with \"make otelfwd\" in the repository." >&2
  exit 2
fi

case "$OTELFWD_SOCKET_DIR" in
  /mnt/*)
    echo "OTELFWD_SOCKET_DIR=$OTELFWD_SOCKET_DIR is on a Windows drive. Sockets cannot be created there, use a Linux directory like /tmp/otelfwd-syslog." >&2
    exit 2
    ;;
esac

# otelfwd and the container run as the same user. Then the socket (mode 0600) belongs to the user of the container.
#
# As root that does not work: NGINX started as root runs its worker processes as its own "nginx" user, and the workers write
# to the socket. They cannot open a socket which belongs to root ("connect() failed (13: Permission denied) while logging to syslog").
# So as root, otelfwd and the container run as the owner of this directory (the user who owns the repository).
RUN_UID="$(id -u)"
RUN_GID="$(id -g)"
DROP_PRIVS=""

if [ "$RUN_UID" = "0" ]; then
  RUN_UID="$(stat -c %u "$HERE")"
  RUN_GID="$(stat -c %g "$HERE")"

  if [ "$RUN_UID" = "0" ]; then
    RUN_UID=1000
    RUN_GID=1000
  fi

  if ! command -v setpriv >/dev/null 2>&1; then
    echo "Running as root needs setpriv (util-linux) to run otelfwd as another user. Run this script as a normal user instead." >&2
    exit 2
  fi

  DROP_PRIVS="setpriv --reuid=$RUN_UID --regid=$RUN_GID --clear-groups"
  echo "Running as root: otelfwd and the container run as uid $RUN_UID, gid $RUN_GID (owner of $HERE)."
fi

export OTELFWD_UID="$RUN_UID"
export OTELFWD_GID="$RUN_GID"


# The directory of the socket has to belong to the user of otelfwd and of the container. If Docker creates it for the mount,
# it belongs to root and otelfwd cannot create its socket in it.
mkdir -p "$OTELFWD_SOCKET_DIR" || exit 1

if [ "$(id -u)" = "0" ]; then
  chown "$RUN_UID:$RUN_GID" "$OTELFWD_SOCKET_DIR" || exit 1
fi

if [ "$(stat -c %u "$OTELFWD_SOCKET_DIR")" != "$RUN_UID" ] || ! chmod 700 "$OTELFWD_SOCKET_DIR" 2>/dev/null; then
  echo "$OTELFWD_SOCKET_DIR does not belong to uid $RUN_UID (owner: $(stat -c %U "$OTELFWD_SOCKET_DIR")). otelfwd could not create its socket there." >&2
  echo "Remove it (it was probably created by Docker or by another user), or set OTELFWD_SOCKET_DIR to another directory." >&2
  exit 1
fi

# The data directory of otelfwd (WAL and metrics) is separate from the one of any other otelfwd. Two instances must not share a WAL.
mkdir -p "$OTELFWD_DATA_DIR" || exit 1

if [ "$(id -u)" = "0" ]; then
  chown -R "$RUN_UID:$RUN_GID" "$OTELFWD_DATA_DIR" || exit 1
fi

if [ "$(stat -c %u "$OTELFWD_DATA_DIR")" != "$RUN_UID" ]; then
  echo "$OTELFWD_DATA_DIR does not belong to uid $RUN_UID (owner: $(stat -c %U "$OTELFWD_DATA_DIR")). otelfwd could not write its WAL there." >&2
  exit 1
fi

# An otelfwd of an earlier --detach which still runs would own the socket. Say so instead of failing later
if [ -f "$PIDFILE" ] && kill -0 "$(cat "$PIDFILE" 2>/dev/null)" 2>/dev/null; then
  echo "otelfwd is already running (pid $(cat "$PIDFILE")). Stop it with: bash run.sh --stop" >&2
  exit 1
fi

if [ "$CLEAN" = "1" ]; then
  rm -f "$OTELFWD_DATA_DIR/otelfwd.wal" "$OTELFWD_DATA_DIR/otelfwd.wal.commit" "$OTELFWD_DATA_DIR/domino/stats/otelfwd.prom"
  echo "Clean start: deleted the WAL and the metrics file in $OTELFWD_DATA_DIR"
fi


# Runs when the script ends. Stops the otelfwd which was started here and removes the container. Nothing else is touched.
# A detached start keeps both.
cleanup()
{
  if [ "$KEEP" = "1" ]; then
    return
  fi

  if [ -n "$FWD_PID" ] && kill -0 "$FWD_PID" 2>/dev/null; then
    kill -TERM "$FWD_PID" 2>/dev/null
    wait "$FWD_PID" 2>/dev/null
  fi

  FWD_PID=""
  rm -f "$PIDFILE"

  if [ "$COMPOSE_UP" = "1" ]; then
    (cd "$HERE" && docker compose down)
  fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM


# Waits until a TCP port of 127.0.0.1 accepts connections
wait_port()
{
  local Port="$1"
  local Seconds="$2"

  for _ in $(seq 1 "$((Seconds * 5))"); do
    if (exec 3<>"/dev/tcp/127.0.0.1/$Port") 2>/dev/null; then
      return 0
    fi

    sleep 0.2
  done

  return 1
}


cd "$HERE" || exit 1

if [ "$REBUILD" = "1" ]; then
  echo "Removing the image and building it again without the build cache"
  docker compose down --rmi local || exit 1
  docker compose build --no-cache || exit 1
fi

echo "Starting otelfwd: $OTELFWD_BIN -nostdin"
echo "  syslog socket : $SOCKET"
echo "  OTLP endpoint : $OTLP_PUSH_API_URL"
echo "  data directory: $OTELFWD_DATA_DIR"

if [ "$DETACH" = "1" ]; then
  # Its own session, so it keeps running when this script and the terminal are gone
  $DROP_PRIVS setsid "$OTELFWD_BIN" -nostdin >> "$LOGFILE" 2>&1 < /dev/null &
  FWD_PID=$!
  echo "  log file      : $LOGFILE"
else
  $DROP_PRIVS "$OTELFWD_BIN" -nostdin &
  FWD_PID=$!
fi

echo "$FWD_PID" > "$PIDFILE"

# otelfwd ends within a moment if it cannot start, for example if another one uses the socket.
# A socket file of an earlier run may already exist, so the file alone does not prove that this otelfwd created it.
sleep 1

# Wait until this otelfwd is running and the socket exists
for _ in $(seq 1 100); do
  if ! kill -0 "$FWD_PID" 2>/dev/null; then
    FWD_PID=""
    rm -f "$PIDFILE"
    echo "otelfwd ended before the socket was ready." >&2

    # A detached otelfwd writes to the log file: show what it said
    if [ "$DETACH" = "1" ] && [ -f "$LOGFILE" ]; then
      echo "The end of $LOGFILE:" >&2
      tail -n 4 "$LOGFILE" | sed 's/^/  /' >&2

      if tail -n 8 "$LOGFILE" | grep -q "in use by another process"; then
        echo "Another otelfwd or run.sh still uses the socket $SOCKET. See who: ps -eo pid,user,args | grep -E 'otelfwd|run.sh'" >&2
        echo "Stop it (Ctrl-C in its terminal, or kill its pid), then start again." >&2
      fi
    fi

    exit 1
  fi

  [ -S "$SOCKET" ] && break

  sleep 0.1
done

if [ ! -S "$SOCKET" ]; then
  echo "The socket $SOCKET was not created within 10 seconds" >&2
  exit 1
fi

echo

if [ "$DETACH" = "1" ]; then
  echo "Starting the NGINX test container in the background"
  COMPOSE_UP=1
  docker compose up -d --build || exit 1

  if ! wait_port "$OTELFWD_NGINX_PORT" 60; then
    echo "The test NGINX did not answer on port $OTELFWD_NGINX_PORT within 60 seconds" >&2
    exit 1
  fi

  KEEP=1
  echo "Running. Send requests to http://127.0.0.1:$OTELFWD_NGINX_PORT/ . Stop everything with: bash run.sh --stop"
  exit 0
fi

echo "Starting the NGINX test container. Send requests to http://127.0.0.1:$OTELFWD_NGINX_PORT/ . Ctrl-C stops everything."
echo

COMPOSE_UP=1
docker compose up --build
