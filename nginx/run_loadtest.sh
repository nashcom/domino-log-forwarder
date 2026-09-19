#!/usr/bin/env bash
# Runs the load test NGINX -> otelfwd -> otel-sink with everything started for it:
#
#   1. builds otelfwd and loadtest (make: only what is missing or older than its sources)
#   2. starts the OTLP test container in the background (tools/otel-sink/run.sh --detach)
#   3. starts otelfwd and the NGINX test container in the background, with a clean WAL (run.sh --detach --clean)
#   4. runs loadtest against them
#   5. stops what it started, and removes the containers (docker compose down)
#
#   bash run_loadtest.sh                                    the defaults of loadtest: 8 threads x 10000 requests
#   bash run_loadtest.sh --threads 32 --requests 100000     all options of this script which are not listed below go to loadtest
#   bash run_loadtest.sh --fail-for 10 --wait 420           WAL test: the sink fails for 10 seconds
#   bash run_loadtest.sh --backup                           failover test: the primary endpoint of otelfwd is the port of the sink which always
#                                                           fails (503), the backup endpoint is the normal port. Every event has to arrive
#                                                           through the backup, and otelfwd has to make a failover
#   bash run_loadtest.sh --reject                           test of refused data: the same, but the primary answers 400 (bad data). Nothing
#                                                           may arrive: the data is dropped, not kept for a retry, not sent to the backup
#   bash run_loadtest.sh --keep                             leave everything running afterwards
#   bash run_loadtest.sh --rebuild                          remove the images and build them again without the build cache
#   bash run_loadtest.sh --yes                              do not ask for the size of the test
#   bash run_loadtest.sh --help                             list the options of this script and of loadtest
#
# The exit code is the one of loadtest: 0 PASS, 1 FAIL, 2 the test could not run. See README.md and "loadtest --help".
#
# Settings (environment variables): those of run.sh in this directory and in tools/otel-sink, for example OTELFWD_NGINX_PORT,
# OTELFWD_DATA_DIR and OTEL_SINK_TOKEN.

if [ -z "${BASH_VERSION:-}" ]; then
  echo "this script needs bash" >&2
  exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"

KEEP=0
ASSUME_YES=0
SHOW_HELP=0
REBUILD=""
ENDPOINT_TEST=""
LOADTEST_ARGS=()
SINK_STARTED=0
NGINX_STARTED=0

for Arg in "$@"; do
  case "$Arg" in
    --keep)    KEEP=1 ;;
    --rebuild) REBUILD="--rebuild" ;;
    --yes)     ASSUME_YES=1 ;;
    --help|-h) SHOW_HELP=1 ;;
    --backup|--reject)
      if [ -n "$ENDPOINT_TEST" ] && [ "$ENDPOINT_TEST" != "$Arg" ]; then
        echo "--backup and --reject cannot be used together" >&2
        exit 2
      fi

      ENDPOINT_TEST="$Arg"
      ;;
    *)         LOADTEST_ARGS+=("$Arg") ;;
  esac
done

DEFAULT_THREADS=8
DEFAULT_REQUESTS=10000
MAX_EVENTS=20000000


# The options of this script, and then those of loadtest. Nothing is built or started
show_help()
{
  cat <<EOF
run_loadtest.sh - load test of NGINX -> otelfwd -> otel-sink, with everything started for it

Usage: ./run_loadtest.sh [options of this script] [options of loadtest]

Options of this script:

  --yes        Do not ask for the size of the test. In a terminal, without --threads and --requests, the script asks for them
               and Enter takes the default ($DEFAULT_THREADS threads with $DEFAULT_REQUESTS requests each). With --yes, or without a
               terminal (a script, CI), it does not ask. It uses the defaults, or the values which are given
  --backup     Failover test: the primary endpoint of otelfwd is the port of the sink which always fails (503), the backup endpoint
               is the normal port. Every event has to arrive through the backup, and otelfwd has to make a failover
  --reject     Test of refused data: the same, but the primary answers 400 (bad data). Nothing may arrive: the data is dropped,
               not kept for a retry, and not sent to the backup
  --keep       Leave everything running afterwards. Stop it later with: run.sh --stop and tools/otel-sink/run.sh --stop
  --rebuild    Remove the images and build them again without the build cache
  --help, -h   This text

Every other option goes to loadtest. Its options:

EOF

  if [ -x "$ROOT/loadtest" ]; then
    "$ROOT/loadtest" --help
  else
    echo "  (loadtest is not built yet. Run \"make loadtest\" in the repository, or start a test once, then this text shows its options)"
  fi

  cat <<EOF

The exit code of this script is the one of loadtest.

Settings (environment variables): those of run.sh in this directory and of tools/otel-sink/run.sh, for example
OTELFWD_NGINX_PORT, OTELFWD_DATA_DIR and OTEL_SINK_TOKEN.
EOF
}

if [ "$SHOW_HELP" = "1" ]; then
  show_help
  exit 0
fi


# 0 (true) if the option was given on the command line
has_opt()
{
  local Option

  for Option in "${LOADTEST_ARGS[@]}"; do
    [ "$Option" = "$1" ] && return 0
  done

  return 1
}


# Asks for a positive whole number. Enter takes the default. Prints only the number, the prompt goes to the terminal
ask_number()
{
  local Label="$1"
  local Default="$2"
  local Value=""

  for _ in 1 2 3; do
    read -r -p "  $Label [$Default]: " Value || Value=""

    [ -z "$Value" ] && Value="$Default"

    case "$Value" in
      *[!0-9]*|0|"") echo "  Please enter a positive whole number." >&2 ;;
      *)             echo "$Value"; return 0 ;;
    esac
  done

  echo "$Default"
}


# In a terminal, without --threads or --requests: propose the size of the test and ask. Not without a terminal (script, CI) or with --yes
if [ -t 0 ] && [ "$ASSUME_YES" != "1" ]; then
  if ! has_opt --threads || ! has_opt --requests; then
    Threads=""
    Requests=""

    echo "Size of the load test. Default: $DEFAULT_THREADS threads x $DEFAULT_REQUESTS requests = $((DEFAULT_THREADS * DEFAULT_REQUESTS)) events."
    echo "Every thread sends its requests one after the other. Enter takes the value in brackets."

    if ! has_opt --threads; then
      Threads="$(ask_number "threads" "$DEFAULT_THREADS")"
      LOADTEST_ARGS+=(--threads "$Threads")
    fi

    if ! has_opt --requests; then
      Requests="$(ask_number "requests per thread" "$DEFAULT_REQUESTS")"
      LOADTEST_ARGS+=(--requests "$Requests")
    fi

    if [ -n "$Threads" ] && [ -n "$Requests" ]; then
      echo "  = $((Threads * Requests)) events"

      if [ "$((Threads * Requests))" -gt "$MAX_EVENTS" ]; then
        echo "  Warning: the sink accepts at most $MAX_EVENTS events per test. The test will not start." >&2
      fi
    fi

    echo
  fi
fi

export OTELFWD_NGINX_PORT="${OTELFWD_NGINX_PORT:-18080}"
export OTELFWD_DATA_DIR="${OTELFWD_DATA_DIR:-$HERE/otelfwd-data}"

SINK_PORT="${OTEL_SINK_PORT:-4318}"
SINK_FAIL_PORT="${OTEL_SINK_FAIL_PORT:-4320}"
FAIL_PORT_WAIT_SEC="${OTEL_SINK_FAIL_PORT_WAIT_SEC:-30}"
export OTLP_PUSH_API_URL="${OTLP_PUSH_API_URL:-http://127.0.0.1:$SINK_PORT/v1/logs}"

# The tests of the two endpoints: the primary is the port of the sink which always fails, the backup is the normal port
if [ -n "$ENDPOINT_TEST" ]; then
  export OTLP_PUSH_API_URL="http://127.0.0.1:$SINK_FAIL_PORT/v1/logs"
  export OTLP_PUSH_API_URL_BACKUP="http://127.0.0.1:$SINK_PORT/v1/logs"

  if [ "$ENDPOINT_TEST" = "--reject" ]; then
    # The sink answers 400 on that port. Only used if this script starts the sink: a sink which is running keeps its setting
    export OTEL_SINK_FAIL_STATUS=400
    LOADTEST_ARGS+=(--expect-reject)
  else
    LOADTEST_ARGS+=(--expect-failover)
  fi
fi


port_open()
{
  (exec 3<>"/dev/tcp/127.0.0.1/$1") 2>/dev/null
}


# 0 (true) if the program on the sink port answers /test/stats like the sink with the load test ledger
sink_has_ledger()
{
  local Auth=""
  local Reply=""

  if [ -n "${OTEL_SINK_TOKEN:-}" ]; then
    Auth="Authorization: Bearer $OTEL_SINK_TOKEN"$'\r\n'
  fi

  Reply="$( { exec 3<>"/dev/tcp/127.0.0.1/$SINK_PORT" && printf 'GET /test/stats HTTP/1.0\r\nHost: localhost\r\n%s\r\n' "$Auth" >&3 && cat <&3; } 2>/dev/null )"

  case "$Reply" in
    *'"expected"'*) return 0 ;;
  esac

  return 1
}


# The HTTP status which the port of the sink which always fails answers to a push request. Empty if the port does not answer
fail_port_status()
{
  local Reply=""

  Reply="$( { exec 3<>"/dev/tcp/127.0.0.1/$SINK_FAIL_PORT" && printf 'POST /v1/logs HTTP/1.0\r\nHost: localhost\r\nContent-Type: application/json\r\nContent-Length: 2\r\n\r\n{}' >&3 && head -n 1 <&3; } 2>/dev/null )"
  Reply="${Reply#* }"

  echo "${Reply%% *}"
}


# --backup needs a port which fails with a status which is tried again (503), --reject one which answers 400. The status is set
# when the sink starts. A sink which was already running has the status it was started with, so it is looked at
check_fail_port()
{
  local Status=""
  local Tries=0

  [ -n "$ENDPOINT_TEST" ] || return 0

  # Docker publishes the port before the sink behind it is ready, and "run.sh --detach" of the sink only waits until a connection is
  # accepted. So the port may not answer at first. Ask again for a while
  while [ "$Tries" -lt "$((FAIL_PORT_WAIT_SEC * 2))" ]; do
    Status="$(fail_port_status)"
    [ -n "$Status" ] && break
    Tries=$((Tries + 1))
    sleep 0.5
  done

  if [ -z "$Status" ]; then
    echo "Port $SINK_FAIL_PORT of the sink does not answer, also not after $FAIL_PORT_WAIT_SEC seconds. Is the sink running? Look at: docker logs otel-test-sink" >&2
    return 1
  fi

  case "$ENDPOINT_TEST:$Status" in
    --reject:400)
      return 0
      ;;
    --reject:*)
      echo "Port $SINK_FAIL_PORT of the sink answers '${Status:-nothing}', but --reject needs 400. The sink was started with another OTEL_SINK_FAIL_STATUS." >&2
      ;;
    --backup:400|--backup:2??|--backup:)
      echo "Port $SINK_FAIL_PORT of the sink answers '${Status:-nothing}', but --backup needs a failing status which is tried again, for example 503 (the default)." >&2
      echo "400 is refused as bad data and is not sent to the backup: that is what --reject tests." >&2
      ;;
    *)
      return 0
      ;;
  esac

  echo "Stop the sink, then run this script again: bash $ROOT/tools/otel-sink/run.sh --stop (a sink which was started from another directory: docker rm -f otel-test-sink)." >&2
  return 1
}


# Stops what was started here. A sink which was already running is left running.
cleanup()
{
  if [ "$KEEP" = "1" ]; then
    echo "Left running (--keep). Stop it with: bash $HERE/run.sh --stop and bash $ROOT/tools/otel-sink/run.sh --stop"
    return
  fi

  if [ "$NGINX_STARTED" = "1" ]; then
    bash "$HERE/run.sh" --stop
  fi

  if [ "$SINK_STARTED" = "1" ]; then
    bash "$ROOT/tools/otel-sink/run.sh" --stop
  fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM


if ! command -v make >/dev/null 2>&1; then
  echo "make was not found" >&2
  exit 2
fi

echo "== building otelfwd and loadtest =="
make -C "$ROOT" otelfwd loadtest || exit 2

if port_open "$OTELFWD_NGINX_PORT"; then
  echo "Port $OTELFWD_NGINX_PORT is already in use. Is the NGINX test container running? Stop it with: bash $HERE/run.sh --stop" >&2
  exit 2
fi

echo
echo "== the OTLP test container =="

if port_open "$SINK_PORT"; then
  # Something answers on the port of the sink. It can be a sink which was started somewhere else, for example from another directory.
  # Starting ours would fail on the container name. It is used if it has the load test ledger, and never stopped here.
  if sink_has_ledger; then
    echo "A sink with the load test ledger answers on port $SINK_PORT. It is used as it is, and left running afterwards."
  else
    echo "Port $SINK_PORT answers, but it is not a sink with the load test ledger (an older image, or another program)." >&2
    echo "Stop it, then run this script again: bash $ROOT/tools/otel-sink/run.sh --stop (a sink which was started from another directory: docker rm -f otel-test-sink)." >&2
    echo "The next start builds the current image." >&2
    exit 2
  fi
else
  SINK_STARTED=1
  bash "$ROOT/tools/otel-sink/run.sh" --detach $REBUILD || exit 2
fi

if [ -n "$ENDPOINT_TEST" ]; then
  check_fail_port || exit 2

  echo
  echo "== two endpoints ($ENDPOINT_TEST) =="
  echo "primary: $OTLP_PUSH_API_URL  (the port of the sink which always fails)"
  echo "backup : $OTLP_PUSH_API_URL_BACKUP"
fi

echo
echo "== starting otelfwd and the NGINX test container =="
NGINX_STARTED=1
bash "$HERE/run.sh" --detach --clean $REBUILD || exit 2

echo
echo "== load test =="

LT=("$ROOT/loadtest"
    --nginx "http://127.0.0.1:$OTELFWD_NGINX_PORT"
    --sink "http://127.0.0.1:$SINK_PORT"
    --otelfwd-prom "$OTELFWD_DATA_DIR/domino/stats/otelfwd.prom")

if [ -n "${OTEL_SINK_TOKEN:-}" ]; then
  LT+=(--token "$OTEL_SINK_TOKEN")
fi

"${LT[@]}" "${LOADTEST_ARGS[@]}"
RC=$?

exit "$RC"
