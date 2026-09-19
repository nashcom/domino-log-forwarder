#!/usr/bin/env bash
# Failing port test: port 4320 always answers with an error. It is used to test retries and the WAL of a client.
# The status code is 503 unless the container was started with another OTEL_SINK_FAIL_STATUS
# (then set SINK_FAIL_STATUS to the same value).

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

echo "== fail port: always answers $SINK_FAIL_STATUS =="

MARKER="fail-$RUN_ID"

require_sink

have_files && BEFORE="$(sink_count)"

post_text "$(fail_url /v1/logs)" "$(otlp_body "$MARKER")"
check "POST is answered with $SINK_FAIL_STATUS" test "$HTTP_CODE" = "$SINK_FAIL_STATUS"
check "the answer is a JSON error" contains "$RESPONSE" "simulated failure"

post_text "$(fail_url /some/other/path)" "$(otlp_body "$MARKER-2")"
check "every path fails" test "$HTTP_CODE" = "$SINK_FAIL_STATUS"

get "$(fail_url /healthz)"
check "healthz on the failing port still answers 200" test "$HTTP_CODE" = "200"

if have_files
then
  check "nothing was stored" test -z "$(sink_grep "$MARKER")"
  check "the number of files did not change" test "$(sink_count)" = "$BEFORE"
else
  skip "cannot read the received files"
fi

summary
