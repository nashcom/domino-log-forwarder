#!/usr/bin/env bash
# Parallel test: many clients at the same time. Every request has to be stored once, with its own file number.
#
#   PARALLEL=50 bash test-parallel.sh

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

N="${PARALLEL:-20}"
MARKER="parallel-$RUN_ID"

echo "== parallel: $N requests at the same time =="

require_sink

for i in $(seq 1 "$N")
do
  otlp_body "$MARKER-$i" > "$WORK/req-$i.json"
done

for i in $(seq 1 "$N")
do
  curl -s -o /dev/null -w '%{http_code}\n' -X POST "$(http_url /v1/logs)" -H 'Content-Type: application/json' \
       ${AUTH[@]+"${AUTH[@]}"} --data-binary "@$WORK/req-$i.json" >> "$WORK/codes" &
done

wait

check "all $N requests answered 200" test "$(grep -c '^200$' "$WORK/codes")" = "$N"

if have_files
then
  sink_grep "$MARKER" > "$WORK/files"

  check "$N files were written" test "$(wc -l < "$WORK/files" | tr -d ' ')" = "$N"
  check "every file number is used only once" test -z "$(cut -c1-6 "$WORK/files" | sort | uniq -d)"

  MISSING=0
  for i in $(seq 1 "$N")
  do
    [ -n "$(sink_grep "$MARKER-$i\"" | head -1)" ] || MISSING=$((MISSING + 1))
  done
  check "every request is in a file of its own" test "$MISSING" = "0"
else
  skip "cannot read the received files"
fi

summary
