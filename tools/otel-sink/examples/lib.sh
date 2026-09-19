#!/usr/bin/env bash
# Shared helpers of the example scripts. Source it, do not run it.
#
# The scripts talk to the otel-test-sink container with curl. Settings (environment variables):
#
#   SINK_HOST          address of the container                       default: 127.0.0.1
#   SINK_HTTP_PORT     OTLP/HTTP port                                 default: 4318
#   SINK_HTTPS_PORT    OTLP/HTTPS port                                default: 4319
#   SINK_FAIL_PORT     the port which always fails                    default: 4320
#   SINK_HTTPS_HOST    name used for HTTPS (has to be in the cert)    default: localhost
#   SINK_TOKEN         bearer token, when the container requires one  default: none
#   SINK_FAIL_STATUS   status code of the failing port                default: 503
#   SINK_DATA_DIR      the /data volume on the host                   default: ../otel-data (next to docker-compose.yml)
#   SINK_CONTAINER     container name, used when SINK_DATA_DIR does not exist  default: otel-test-sink
#
# The received files are read from SINK_DATA_DIR. If that directory does not exist, the scripts try
# "docker exec" on the container instead. Without access to the files the checks on the files are skipped.
# The scripts only add files to the sink. They never delete anything.

if [ -z "${BASH_VERSION:-}" ]; then
  echo "these scripts need bash" >&2
  exit 2
fi

EXAMPLES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

SINK_HOST="${SINK_HOST:-127.0.0.1}"
SINK_HTTP_PORT="${SINK_HTTP_PORT:-4318}"
SINK_HTTPS_PORT="${SINK_HTTPS_PORT:-4319}"
SINK_FAIL_PORT="${SINK_FAIL_PORT:-4320}"
SINK_HTTPS_HOST="${SINK_HTTPS_HOST:-localhost}"
SINK_TOKEN="${SINK_TOKEN:-}"
SINK_FAIL_STATUS="${SINK_FAIL_STATUS:-503}"
SINK_DATA_DIR="${SINK_DATA_DIR:-$EXAMPLES_DIR/../otel-data}"
SINK_CONTAINER="${SINK_CONTAINER:-otel-test-sink}"

# Marks the data of one run, so a script finds its own files among all the others
RUN_ID="${RUN_ID:-$(date +%H%M%S)-$$}"

PASSED=0
FAILED=0
SKIPPED=0

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

HTTP_CODE=""
RESPONSE=""
AUTH=()


set_auth()
{
  AUTH=()
  [ -n "${1:-}" ] && AUTH=(-H "Authorization: Bearer $1")
  return 0
}

set_auth "$SINK_TOKEN"


http_url()  { echo "http://$SINK_HOST:$SINK_HTTP_PORT$1"; }
https_url() { echo "https://$SINK_HTTPS_HOST:$SINK_HTTPS_PORT$1"; }
fail_url()  { echo "http://$SINK_HOST:$SINK_FAIL_PORT$1"; }


# ---------------------------------------------------------------------------------------------------------------
# Requests

json_escape()
{
  printf '%s' "$1" | sed -e 's/\\/\\\\/g' -e 's/"/\\"/g'
}


# otlp_body MESSAGE [SEVERITY_NUMBER]: an OTLP/JSON request with one log record. MESSAGE must not contain line breaks.
otlp_body()
{
  printf '{"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"otel-sink-example"}}]},"scopeLogs":[{"scope":{"name":"example"},"logRecords":[{"timeUnixNano":"%s","severityNumber":%s,"severityText":"INFO","body":{"stringValue":"%s"}}]}]}]}' \
    "$(date +%s)000000000" "${2:-9}" "$(json_escape "$1")"
}


# post URL FILE [curl options]: POSTs the file as JSON. Sets HTTP_CODE ("000" if curl failed) and RESPONSE.
post()
{
  local url="$1" file="$2"
  shift 2

  HTTP_CODE="$(curl -s -o "$WORK/response" -w '%{http_code}' -X POST "$url" -H 'Content-Type: application/json' \
               ${AUTH[@]+"${AUTH[@]}"} --data-binary "@$file" "$@")" || HTTP_CODE="000"
  RESPONSE="$(cat "$WORK/response" 2>/dev/null || true)"
}


# post_text URL TEXT [curl options]: the same for a short text
post_text()
{
  local url="$1" text="$2"
  shift 2

  printf '%s' "$text" > "$WORK/request"
  post "$url" "$WORK/request" "$@"
}


# get URL [curl options]: sets HTTP_CODE and RESPONSE
get()
{
  local url="$1"
  shift

  HTTP_CODE="$(curl -s -o "$WORK/response" -w '%{http_code}' ${AUTH[@]+"${AUTH[@]}"} "$url" "$@")" || HTTP_CODE="000"
  RESPONSE="$(cat "$WORK/response" 2>/dev/null || true)"
}


require_sink()
{
  get "$(http_url /healthz)"

  if [ "$HTTP_CODE" != "200" ]; then
    echo "The sink does not answer on $(http_url /healthz) (HTTP $HTTP_CODE)." >&2
    echo "Start it first: docker compose up  (in tools/otel-sink). Ports and host can be changed with SINK_HOST and SINK_HTTP_PORT." >&2
    exit 2
  fi
}


# ---------------------------------------------------------------------------------------------------------------
# The received files. Read from the data directory, or with docker exec

data_source()
{
  if [ -d "$SINK_DATA_DIR/received" ]; then
    echo dir
  elif command -v docker >/dev/null 2>&1 && docker inspect "$SINK_CONTAINER" >/dev/null 2>&1; then
    echo docker
  else
    echo none
  fi
}


have_files()
{
  [ "$(data_source)" != "none" ]
}


sink_list()
{
  case "$(data_source)" in
    dir)    ls -1 "$SINK_DATA_DIR/received" ;;
    docker) docker exec "$SINK_CONTAINER" ls -1 /data/received ;;
    *)      return 1 ;;
  esac
}


sink_count()
{
  sink_list | wc -l | tr -d ' '
}


# sink_cat FILE
sink_cat()
{
  case "$(data_source)" in
    dir)    cat "$SINK_DATA_DIR/received/$1" ;;
    docker) docker exec "$SINK_CONTAINER" cat "/data/received/$1" ;;
    *)      return 1 ;;
  esac
}


# sink_grep TEXT: names of the files which contain the text
sink_grep()
{
  case "$(data_source)" in
    dir)    grep -l -F -- "$1" "$SINK_DATA_DIR"/received/* 2>/dev/null | sed 's|.*/||' ;;
    docker) docker exec "$SINK_CONTAINER" sh -c 'grep -l -F -- "$1" /data/received/* 2>/dev/null' _ "$1" | sed 's|.*/||' ;;
    *)      return 1 ;;
  esac
  return 0
}


# sink_cert FILE: copies the certificate of the sink
sink_cert()
{
  case "$(data_source)" in
    dir)    cp "$SINK_DATA_DIR/certs/cert.pem" "$1" ;;
    docker) docker cp "$SINK_CONTAINER:/data/certs/cert.pem" "$1" >/dev/null ;;
    *)      return 1 ;;
  esac
}


# ---------------------------------------------------------------------------------------------------------------
# Results

pass() { PASSED=$((PASSED + 1)); echo "  PASS  $*"; }
fail() { FAILED=$((FAILED + 1)); echo "  FAIL  $*"; }
skip() { SKIPPED=$((SKIPPED + 1)); echo "  SKIP  $*"; }


# check DESCRIPTION COMMAND [ARGS]: PASS if the command succeeds, for example: check "status is 200" test "$HTTP_CODE" = 200
check()
{
  local description="$1"
  shift

  if "$@"; then
    pass "$description"
  else
    fail "$description (HTTP $HTTP_CODE)"
  fi
}


contains() { case "$1" in *"$2"*) return 0 ;; *) return 1 ;; esac; }

# matches TEXT REGEX: extended regular expression
matches() { [[ "$1" =~ $2 ]]; }


summary()
{
  echo
  echo "$(basename "$0"): $PASSED passed, $FAILED failed, $SKIPPED skipped"

  [ "$FAILED" -eq 0 ]
}
