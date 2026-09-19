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
#   bash run_loadtest.sh --keep                             leave everything running afterwards
#   bash run_loadtest.sh --rebuild                          remove the images and build them again without the build cache
#   bash run_loadtest.sh --yes                              do not ask for the size of the test
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
REBUILD=""
LOADTEST_ARGS=()
SINK_STARTED=0
NGINX_STARTED=0

for Arg in "$@"; do
  case "$Arg" in
    --keep)    KEEP=1 ;;
    --rebuild) REBUILD="--rebuild" ;;
    --yes)     ASSUME_YES=1 ;;
    *)         LOADTEST_ARGS+=("$Arg") ;;
  esac
done

DEFAULT_THREADS=8
DEFAULT_REQUESTS=10000
MAX_EVENTS=20000000


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
export OTLP_PUSH_API_URL="${OTLP_PUSH_API_URL:-http://127.0.0.1:$SINK_PORT/v1/logs}"


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
