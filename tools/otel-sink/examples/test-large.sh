#!/usr/bin/env bash
# Large request test: one POST with many log records (default 3000, about 550 KB, as a batch of otelfwd can be),
# and a request above the limit of 32 MB, which NGINX refuses.
#
#   LARGE_RECORDS=10000 bash test-large.sh

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

N="${LARGE_RECORDS:-3000}"
MARKER="large-$RUN_ID"

echo "== large: $N log records in one POST =="

require_sink

awk -v n="$N" -v id="$MARKER" 'BEGIN {
  printf "{\"resourceLogs\":[{\"scopeLogs\":[{\"scope\":{\"name\":\"large\"},\"logRecords\":["
  for (i = 1; i <= n; i++) {
    if (i > 1) printf ","
    printf "{\"timeUnixNano\":\"1785760554890000000\",\"body\":{\"stringValue\":\"%s record %05d xxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxxx\"}}", id, i
  }
  printf "]}]}]}"
}' > "$WORK/large.json"

SIZE="$(wc -c < "$WORK/large.json" | tr -d ' ')"
echo "  request size: $SIZE bytes"

post "$(http_url /v1/logs)" "$WORK/large.json"
check "POST answered 200" test "$HTTP_CODE" = "200"

if have_files
then
  FILE="$(sink_grep "$MARKER record $(printf '%05d' "$N")" | head -1)"

  check "the last record is in a file" test -n "$FILE"

  if [ -n "$FILE" ]; then
    sink_cat "$FILE" > "$WORK/stored.json"
    check "all $N records were stored" test "$(grep -c -F "$MARKER record" "$WORK/stored.json")" = "$N"
  fi
else
  skip "cannot read the received files"
fi

echo "== above the limit: 33 MB =="

head -c 34603008 /dev/zero | tr '\0' 'x' > "$WORK/huge.txt"
post "$(http_url /v1/logs)" "$WORK/huge.txt"
check "a 33 MB request is refused with 413 (NGINX limit is 32 MB)" test "$HTTP_CODE" = "413"

summary
