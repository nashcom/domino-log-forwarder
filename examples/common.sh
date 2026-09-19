#!/usr/bin/env bash
# Shared part of the example scripts. Source it, do not run it.
#
# Settings (environment variables):
#
#   OTELFWD_BIN          the forwarder. Default: ../otelfwd (built with "make"), otherwise otelfwd from the PATH
#   OTLP_PUSH_API_URL    where to push. Default: http://127.0.0.1:4318/v1/logs (the OTLP test container in tools/otel-sink)
#   OTLP_PUSH_TOKEN      bearer token, if the receiver needs one
#   OTLP_CA_FILE         CA file for https://, for example tools/otel-sink/otel-data/certs/cert.pem
#   OTELFWD_DATA_DIR     data directory of the forwarder (WAL, metrics). Default: a temporary directory,
#                        which is deleted afterwards. Set it to keep the WAL of a failed push.

if [ -z "${BASH_VERSION:-}" ]; then
  echo "these scripts need bash" >&2
  exit 2
fi

EXAMPLES_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

if [ -z "${OTELFWD_BIN:-}" ]; then
  if [ -x "$EXAMPLES_DIR/../otelfwd" ]; then
    OTELFWD_BIN="$EXAMPLES_DIR/../otelfwd"
  elif command -v otelfwd >/dev/null 2>&1; then
    OTELFWD_BIN="$(command -v otelfwd)"
  else
    echo "otelfwd was not found. Build it with \"make\" in the repository, or set OTELFWD_BIN." >&2
    exit 2
  fi
fi

export OTLP_PUSH_API_URL="${OTLP_PUSH_API_URL:-http://127.0.0.1:4318/v1/logs}"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"; [ -n "${FWD_PID:-}" ] && kill "$FWD_PID" 2>/dev/null; true' EXIT

if [ -z "${OTELFWD_DATA_DIR:-}" ]; then
  export OTELFWD_DATA_DIR="$WORK/data"
  KEEP_DATA=0
else
  KEEP_DATA=1
fi

mkdir -p "$OTELFWD_DATA_DIR"

# The forwarder finds the name of the Domino server task of a log line in pid.nbf
[ -e "$OTELFWD_DATA_DIR/pid.nbf" ] || cp "$EXAMPLES_DIR/pid.nbf" "$OTELFWD_DATA_DIR/pid.nbf"

PROM_FILE="${OTELFWD_PROM_FILE:-$OTELFWD_DATA_DIR/domino/stats/otelfwd.prom}"


# metric NAME: value of a metric of the metrics file the forwarder writes at shutdown
metric()
{
  grep -v '^#' "$PROM_FILE" 2>/dev/null | grep -F "otelfwd_$1 " | head -1 | awk '{print $NF}'
}


# report: what the forwarder did. Returns 0 if lines were pushed and no push failed.
report()
{
  local pushed errors
  pushed="$(metric 'push_total{result="success"}')"
  errors="$(metric 'push_total{result="error"}')"

  echo
  echo "lines received by the forwarder : $(metric lines_received_total)"
  echo "lines pushed                    : ${pushed:-0}"
  echo "lines which failed              : ${errors:-0}"

  if [ "${errors:-0}" != "0" ]; then
    echo "The push failed. Is the receiver running at $OTLP_PUSH_API_URL ?" >&2
    [ "$KEEP_DATA" = "1" ] && echo "The lines are in the WAL: $OTELFWD_DATA_DIR/otelfwd.wal" >&2
    return 1
  fi

  [ "${pushed:-0}" != "0" ]
}
