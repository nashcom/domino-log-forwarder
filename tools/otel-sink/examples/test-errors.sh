#!/usr/bin/env bash
# Unusual requests: a body which is not JSON, broken JSON, another HTTP method.
# The sink stores every body it gets and only uses the extension .bin for what it cannot parse as JSON.

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

echo "== errors and unusual requests =="

MARKER="errors-$RUN_ID"

require_sink

# Bytes which are not JSON, for example protobuf. Contains the marker to find the file again.
{ printf '\010\001\022\004'; printf '%s' "$MARKER-binary"; printf '\000\377\376'; } > "$WORK/binary.bin"

post "$(http_url /v1/traces)" "$WORK/binary.bin" -H 'Content-Type: application/x-protobuf'
check "a binary body is accepted (200)" test "$HTTP_CODE" = "200"

if have_files
then
  FILE="$(sink_grep "$MARKER-binary" | head -1)"

  check "it is stored as a .bin file" matches "$FILE" '\.bin$'

  if [ -n "$FILE" ]; then
    sink_cat "$FILE" > "$WORK/stored.bin"
    check "byte for byte identical" cmp -s "$WORK/binary.bin" "$WORK/stored.bin"
  fi
else
  skip "cannot read the received files"
fi

post_text "$(http_url /v1/logs)" "{\"broken\": \"$MARKER-broken\""
check "broken JSON is accepted (200)" test "$HTTP_CODE" = "200"

if have_files
then
  check "and stored as .bin" matches "$(sink_grep "$MARKER-broken" | head -1)" '\.bin$'
fi

HTTP_CODE="$(curl -s -o "$WORK/response" -w '%{http_code}' -X PUT "$(http_url /v1/logs)" ${AUTH[@]+"${AUTH[@]}"} -d '{}')"
check "PUT is refused with 405" test "$HTTP_CODE" = "405"

HTTP_CODE="$(curl -s -o "$WORK/response" -w '%{http_code}' -X POST "$(http_url '/../../etc/passwd?x=1')" ${AUTH[@]+"${AUTH[@]}"} -H 'Content-Type: application/json' -d '{}')"
check "a path with .. is harmless (200), the file stays in the received directory" test "$HTTP_CODE" = "200"

summary
