#!/usr/bin/env bash
# Bearer token test. Only meaningful when the container was started with a token:
#
#   OTEL_SINK_TOKEN=secret docker compose up
#   SINK_TOKEN=secret bash test-token.sh
#
# Without a token in the container the test is skipped.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

echo "== token: Authorization: Bearer =="

MARKER="token-$RUN_ID"

require_sink

set_auth ""
post_text "$(http_url /v1/logs)" "$(otlp_body "$MARKER-none")"

if [ "$HTTP_CODE" = "200" ]
then
  skip "the container does not require a token (start it with OTEL_SINK_TOKEN=secret)"
  summary
  exit $?
fi

check "no token: 401" test "$HTTP_CODE" = "401"
check "the 401 answer is a JSON error" contains "$RESPONSE" "unauthorized"

set_auth "definitely-wrong"
post_text "$(http_url /v1/logs)" "$(otlp_body "$MARKER-wrong")"
check "wrong token: 401" test "$HTTP_CODE" = "401"

set_auth "definitely-wrong"
post_text "$(https_url /v1/logs)" "$(otlp_body "$MARKER-wrong-tls")" --insecure
check "wrong token over HTTPS: 401" test "$HTTP_CODE" = "401"

get "$(http_url /healthz)"
check "healthz needs no token" test "$HTTP_CODE" = "200"

if [ -z "$SINK_TOKEN" ]
then
  skip "SINK_TOKEN is not set, so the right token cannot be tested"
else
  set_auth "$SINK_TOKEN"
  post_text "$(http_url /v1/logs)" "$(otlp_body "$MARKER-ok")"
  check "right token: 200" test "$HTTP_CODE" = "200"
fi

if have_files
then
  check "refused requests were not stored" test -z "$(sink_grep "$MARKER-none"; sink_grep "$MARKER-wrong")"

  [ -n "$SINK_TOKEN" ] && check "the accepted request was stored" test -n "$(sink_grep "$MARKER-ok" | head -1)"
fi

summary
