#!/usr/bin/env bash
# gzip test: OTLP exporters may compress the request (Content-Encoding: gzip). The sink stores the decompressed JSON.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

echo "== gzip: compressed request body =="

MARKER="gzip-$RUN_ID"

require_sink

if ! command -v gzip >/dev/null 2>&1
then
  skip "gzip is not installed"
  summary
  exit $?
fi

otlp_body "$MARKER" | gzip -c > "$WORK/body.gz"

post "$(http_url /v1/logs)" "$WORK/body.gz" -H 'Content-Encoding: gzip'
check "compressed POST answered 200" test "$HTTP_CODE" = "200"

if have_files
then
  FILE="$(sink_grep "$MARKER" | head -1)"

  check "the file contains the decompressed record" test -n "$FILE"
  [ -n "$FILE" ] && check "and it is pretty printed JSON" contains "$(sink_cat "$FILE")" '  "resourceLogs"'
else
  skip "cannot read the received files"
fi

post_text "$(http_url /v1/logs)" "this is not gzip" -H 'Content-Encoding: gzip'
check "a body which is not gzip is refused with 400" test "$HTTP_CODE" = "400"

summary
