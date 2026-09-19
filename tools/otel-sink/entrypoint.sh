#!/bin/sh
# Entrypoint of the OTLP test container: starts otel-sink and NGINX and stops both together.
#
# Settings (environment variables of the container):
#   OTEL_SINK_TOKEN        require "Authorization: Bearer <token>" on ports 4318 and 4319, otherwise answer 401
#   OTEL_SINK_FAIL_STATUS  status code of port 4320, which always fails (default: 503)
#   OTEL_SINK_SAN          subject alternative names of the generated certificate (default: DNS:localhost,IP:127.0.0.1)
#   OTEL_SINK_RAW          1: write the bodies as received, do not pretty print JSON
#
# The paths can be changed to run this script outside of the container (used for testing it):
#   OTEL_SINK_DATA, OTEL_SINK_TMP, OTEL_SINK_BIN, OTEL_SINK_TEMPLATE, OTEL_SINK_NGINX

set -eu

DATA="${OTEL_SINK_DATA:-/data}"
TMP="${OTEL_SINK_TMP:-/tmp/otel-sink}"
SINK_BIN="${OTEL_SINK_BIN:-/usr/local/bin/otel-sink}"
TEMPLATE="${OTEL_SINK_TEMPLATE:-/etc/otel-sink/nginx.conf.template}"
NGINX_BIN="${OTEL_SINK_NGINX:-nginx}"
TOKEN="${OTEL_SINK_TOKEN:-}"
FAIL_STATUS="${OTEL_SINK_FAIL_STATUS:-503}"
SAN="${OTEL_SINK_SAN:-DNS:localhost,IP:127.0.0.1}"
RAW="${OTEL_SINK_RAW:-0}"

OUT="$DATA/received"
CERTS="$DATA/certs"
SOCK="$TMP/sink.sock"


die()
{
  echo "otel-sink: $*" >&2
  exit 1
}


# The values end up in the NGINX configuration and in a certificate request: allow only harmless characters

case "$FAIL_STATUS" in
  [45][0-9][0-9]) ;;
  *) die "OTEL_SINK_FAIL_STATUS has to be a status code from 400 to 599" ;;
esac

case "$TOKEN" in
  *[!A-Za-z0-9._~+/=-]*) die "OTEL_SINK_TOKEN may only contain letters, digits and . _ ~ + / = -" ;;
esac

case "$SAN" in
  *[!A-Za-z0-9.:,_-]*) die "OTEL_SINK_SAN may only contain letters, digits and . : , _ -  (example: DNS:localhost,IP:127.0.0.1)" ;;
esac


mkdir -p "$OUT" "$CERTS" "$TMP/body" "$TMP/proxy" "$TMP/fastcgi" "$TMP/uwsgi" "$TMP/scgi" \
  || die "cannot create directories in $DATA or $TMP. Mount a volume writable for the container user, or use docker run --user"

[ -w "$OUT" ] || die "$OUT is not writable for user $(id -u). Use a volume writable for that user, or docker run --user"


# Self-signed certificate. It is kept in the volume, so the client can trust it (OTLP_CA_FILE) and it survives a restart.
# A new one is generated if it is missing or the requested names changed.

if [ ! -s "$CERTS/cert.pem" ] || [ ! -s "$CERTS/key.pem" ] || [ "$(cat "$CERTS/san.txt" 2>/dev/null)" != "$SAN" ]; then
  openssl req -x509 -newkey rsa:2048 -nodes -days 825 -subj "/CN=otel-test-sink" -addext "subjectAltName=$SAN" \
    -keyout "$CERTS/key.pem" -out "$CERTS/cert.pem" >/dev/null 2>&1 || die "cannot generate the certificate"
  chmod 600 "$CERTS/key.pem"
  echo "$SAN" > "$CERTS/san.txt"
  echo "otel-sink: generated certificate $CERTS/cert.pem ($SAN)"
fi


# NGINX configuration

if [ -n "$TOKEN" ]; then
  TOKEN_CHECK="if (\$http_authorization != \"Bearer $TOKEN\") { return 401 '{\"error\":\"unauthorized\"}'; }"
else
  TOKEN_CHECK=""
fi

cat > "$TMP/locations.conf" <<EOF
location = /healthz
{
    default_type text/plain;
    return 200 "ok\n";
}

location /
{
    $TOKEN_CHECK
    proxy_pass http://unix:$SOCK:;
}
EOF

sed -e "s|@TMP@|$TMP|g" -e "s|@DATA@|$DATA|g" -e "s|@CERT@|$CERTS/cert.pem|g" -e "s|@KEY@|$CERTS/key.pem|g" \
    -e "s|@FAIL_STATUS@|$FAIL_STATUS|g" "$TEMPLATE" > "$TMP/nginx.conf" || die "cannot read $TEMPLATE"


# Start otel-sink, wait for its socket, then NGINX

SINK_ARGS=""
[ "$RAW" = "1" ] && SINK_ARGS="--raw"

# shellcheck disable=SC2086
"$SINK_BIN" --socket "$SOCK" --dir "$OUT" $SINK_ARGS &
SINK_PID=$!

I=0
while [ ! -S "$SOCK" ]
do
  kill -0 "$SINK_PID" 2>/dev/null || die "otel-sink did not start"
  I=$((I + 1))
  [ "$I" -gt 100 ] && die "otel-sink did not create $SOCK"
  sleep 0.1
done

"$NGINX_BIN" -c "$TMP/nginx.conf" -e stderr -g "daemon off;" &
NGINX_PID=$!

echo "otel-sink: listening on 4318 (http), 4319 (https) and 4320 (always fails with $FAIL_STATUS)"
[ -n "$TOKEN" ] && echo "otel-sink: bearer token required on 4318 and 4319"


stop_all()
{
  kill "$NGINX_PID" "$SINK_PID" 2>/dev/null || true
  wait "$NGINX_PID" 2>/dev/null || true
  wait "$SINK_PID" 2>/dev/null || true
}

trap 'stop_all; exit 0' TERM INT


# Runs until a signal arrives or one of the two processes ends. "sleep & wait" lets the trap run immediately.

while kill -0 "$NGINX_PID" 2>/dev/null && kill -0 "$SINK_PID" 2>/dev/null
do
  sleep 1 &
  wait $!
done

kill -0 "$NGINX_PID" 2>/dev/null || echo "otel-sink: NGINX stopped" >&2
kill -0 "$SINK_PID" 2>/dev/null || echo "otel-sink: otel-sink stopped" >&2

stop_all
exit 1
