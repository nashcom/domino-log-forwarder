#!/usr/bin/env bash
# HTTPS test: the sink generates a self-signed certificate. curl only trusts it when it is given with --cacert.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

echo "== https: generated certificate =="

MARKER="https-$RUN_ID"

require_sink

if ! sink_cert "$WORK/cert.pem" 2>/dev/null
then
  skip "cannot read the certificate (needs SINK_DATA_DIR/certs/cert.pem, or docker exec access to the container)"
  summary
  exit $?
fi

check "the certificate file is a PEM certificate" contains "$(head -1 "$WORK/cert.pem")" "BEGIN CERTIFICATE"

post_text "$(https_url /v1/logs)" "$(otlp_body "$MARKER")" --cacert "$WORK/cert.pem"
check "POST over HTTPS with --cacert answered 200" test "$HTTP_CODE" = "200"

if have_files
then
  check "the record was written to a file" test -n "$(sink_grep "$MARKER" | head -1)"
fi

curl -s -o /dev/null "$(https_url /healthz)"
CURL_EXIT=$?
check "without --cacert curl refuses the self-signed certificate (exit code 60)" test "$CURL_EXIT" = "60"

curl -s -o /dev/null --cacert "$WORK/cert.pem" "$(https_url /healthz)"
check "healthz over HTTPS with --cacert" test "$?" = "0"

summary
