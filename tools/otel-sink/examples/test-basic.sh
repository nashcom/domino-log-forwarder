#!/usr/bin/env bash
# Basic test: health check, one POST, and what the sink wrote for it.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

echo "== basic: health, POST, file =="

MARKER="basic-$RUN_ID"

require_sink
check "healthz answers 200" test "$HTTP_CODE" = "200"

post_text "$(http_url /v1/logs)" "$(otlp_body "$MARKER")"
check "POST answered 200" test "$HTTP_CODE" = "200"
check "the answer is an empty OTLP response {}" test "$RESPONSE" = "{}"

if have_files
then
  FILE="$(sink_grep "$MARKER" | head -1)"

  check "a file with the record was written" test -n "$FILE"

  if [ -n "$FILE" ]; then
    check "file name is <number>_<UTC time>_v1-logs.json" matches "$FILE" '^[0-9]{6}_[0-9]{8}T[0-9]{6}\.[0-9]{3}Z_v1-logs\.json$'
    check "the JSON is pretty printed" contains "$(sink_cat "$FILE")" '  "resourceLogs"'
  fi

  post_text "$(http_url /otlp/v1/logs)" "$(otlp_body "$MARKER-path")"
  check "another path is accepted as well" test "$HTTP_CODE" = "200"
  check "the path is part of the file name" contains "$(sink_grep "$MARKER-path" | head -1)" "_otlp-v1-logs.json"
else
  skip "cannot read the received files (set SINK_DATA_DIR, or start from tools/otel-sink)"
fi

get "$(http_url /)"
check "GET / is answered by the sink" contains "$RESPONSE" "otel-sink"

summary
