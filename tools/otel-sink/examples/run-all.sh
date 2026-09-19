#!/usr/bin/env bash
# Runs all example tests one after the other and prints a summary.
#
#   bash run-all.sh
#   SINK_TOKEN=secret bash run-all.sh              the container was started with OTEL_SINK_TOKEN=secret
#   SINK_HOST=192.168.1.20 bash run-all.sh         a container on another host
#
# Exit code: 0 all tests passed (skipped ones do not count as failed), 1 a test failed, 2 the sink is not reachable.

DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

# One run id for all scripts, so their data can be told apart from the data of earlier runs
export RUN_ID="${RUN_ID:-$(date +%H%M%S)-$$}"

TESTS="test-basic test-https test-token test-fail-port test-gzip test-errors test-parallel test-large"
RESULT=0
LINES=""

for TEST in $TESTS
do
  echo
  bash "$DIR/$TEST.sh"
  CODE=$?

  case "$CODE" in
    0) STATE="ok" ;;
    2) STATE="sink not reachable" ; RESULT=2 ;;
    *) STATE="FAILED" ; [ "$RESULT" -eq 0 ] && RESULT=1 ;;
  esac

  LINES="$LINES
  $(printf '%-18s' "$TEST") $STATE"

  [ "$CODE" -eq 2 ] && break
done

echo
echo "== summary =="
echo "$LINES" | sed '1d'
echo

exit "$RESULT"
