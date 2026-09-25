#!/usr/bin/env bash
# Test: the metrics file of otelfwd (Prometheus text format), and the label OTELFWD_INSTANCE which tells instances apart.
#
# otelfwd writes the file every 10 seconds and once more when it stops, so each case starts an instance, stops it with SIGTERM
# and reads the file. What is checked:
#
#   1. without OTELFWD_INSTANCE: every metric line is well formed (a name, labels in braces if there are any, an integer), there is
#      no label otelfwd_instance anywhere, and the # HELP and # TYPE lines have no labels
#   2. with OTELFWD_INSTANCE=mail: every metric line has the label otelfwd_instance="mail", the # HELP and # TYPE lines still have
#      none, the lines are still well formed, and the metrics are the same ones as in 1 (the label changes no name)
#   3. a name with a double quote in it is escaped in the file
#   4. two instances with different names write no series which the other one writes too, and two without a name do: that is the
#      clash of a collector which reads both files, which the label is for
#
# What the script needs: the built forwarder (make), or OTELFWD_BIN, see common.sh. UNIX sockets in a temporary directory only: no
# TCP port, no network. The push URL points to a port where nothing listens, and no record is sent.
#
#   make otelfwd && ./tests/test_metrics.sh

. "$(dirname "${BASH_SOURCE[0]}")/common.sh" || exit 2

LOG_NAMES="plain mail quote domino plain2"

# Starts an instance, waits until it listens, stops it with SIGTERM (it writes its metrics file when it stops) and waits until
# it is gone. Arguments: the name of the case, and OTELFWD_INSTANCE (nothing, or an empty string, for no name). The metrics
# file is $WORK/<case>.prom. Returns 1 if the instance did not start, did not stop or wrote no file
run_case() {
  local Case="$1" Instance="${2:-}" Pid i

  mkdir -p "$WORK/$Case-data"

  OTELFWD_INSTANCE="$Instance" OTELFWD_DATA_DIR="$WORK/$Case-data" OTELFWD_UNIX_SOCKET="$WORK/$Case.sock" OTELFWD_PROM_FILE="$WORK/$Case.prom" \
    OTLP_PUSH_API_URL="http://127.0.0.1:1/v1/logs" "$OTELFWD_BIN" -nostdin > "$WORK/$Case.log" 2>&1 &
  Pid=$!
  PIDS="$PIDS $Pid"

  if ! wait_for_socket "$WORK/$Case.sock"; then return 1; fi

  kill -TERM "$Pid" 2>/dev/null

  for i in $(seq 1 150); do
    if ! kill -0 "$Pid" 2>/dev/null; then break; fi
    sleep 0.1
  done

  if kill -0 "$Pid" 2>/dev/null; then
    kill -KILL "$Pid" 2>/dev/null
    return 1
  fi

  wait "$Pid" 2>/dev/null

  [ -s "$WORK/$Case.prom" ]
}

# The metric lines of a file: no comment lines, no empty lines
metric_lines() {
  grep -v '^#' "$1" | grep -v '^$'
}

LINE='^otelfwd_[a-z0-9_]+(\{[^}]*\})? [0-9]+$'

# 1. Without a name
run_case plain
check $? "an instance without OTELFWD_INSTANCE starts, stops, and writes its metrics file"

N="$(metric_lines "$WORK/plain.prom" | wc -l)"
if [ "$N" -ge 10 ]; then check 0 "the file has metric lines ($N)"; else check 1 "the file has metric lines (only $N)"; fi

BAD="$(metric_lines "$WORK/plain.prom" | grep -vcE "$LINE")"
check "$([ "$BAD" = "0" ] && echo 0 || echo 1)" "every metric line is well formed: a name, labels in braces if there are any, an integer ($BAD are not)"

COUNT="$(grep -c 'otelfwd_instance' "$WORK/plain.prom")"
check "$([ "$COUNT" = "0" ] && echo 0 || echo 1)" "without OTELFWD_INSTANCE there is no label otelfwd_instance in the file ($COUNT lines have it)"

COUNT="$(grep '^#' "$WORK/plain.prom" | grep -c '{')"
check "$([ "$COUNT" = "0" ] && echo 0 || echo 1)" "the # HELP and # TYPE lines have no labels ($COUNT do)"

COUNT="$(grep -c '^# HELP otelfwd_health ' "$WORK/plain.prom")"
check "$([ "$COUNT" = "1" ] && echo 0 || echo 1)" "the # HELP line of otelfwd_health is there once ($COUNT times)"

# 2. With a name
run_case mail mail
check $? "an instance with OTELFWD_INSTANCE=mail starts, stops, and writes its metrics file"

BAD="$(metric_lines "$WORK/mail.prom" | grep -vc 'otelfwd_instance="mail"')"
check "$([ "$BAD" = "0" ] && echo 0 || echo 1)" "every metric line has the label otelfwd_instance=\"mail\" ($BAD do not)"

BAD="$(metric_lines "$WORK/mail.prom" | grep -vcE '^otelfwd_[a-z0-9_]+\{otelfwd_instance="mail"(,[^}]*)?\} [0-9]+$')"
check "$([ "$BAD" = "0" ] && echo 0 || echo 1)" "the label is the first one in the braces and the lines are well formed ($BAD are not)"

COUNT="$(grep '^#' "$WORK/mail.prom" | grep -c 'otelfwd_instance')"
check "$([ "$COUNT" = "0" ] && echo 0 || echo 1)" "the # HELP and # TYPE lines still have no label ($COUNT have it)"

# The same metrics as without a name: take the label away from the names of the second file and compare the names, not the values
metric_lines "$WORK/plain.prom" | awk '{print $1}' | sort > "$WORK/plain.names"
metric_lines "$WORK/mail.prom" | awk '{print $1}' | sed -e 's/otelfwd_instance="mail",//' -e 's/{otelfwd_instance="mail"}//' | sort > "$WORK/mail.names"
diff -q "$WORK/plain.names" "$WORK/mail.names" > /dev/null
check $? "the label changes no metric: the names are the same as without it"

# 3. A double quote in the name
run_case quote 'a"b'
check $? "an instance with a double quote in its name starts, stops, and writes its metrics file"

grep -qF 'otelfwd_instance="a\"b"' "$WORK/quote.prom"
check $? "the double quote in the name is escaped in the file"

# 4. Two instances, read together
run_case domino domino
check $? "a second named instance writes its metrics file"

SAME="$( (metric_lines "$WORK/mail.prom"; metric_lines "$WORK/domino.prom") | awk '{print $1}' | sort | uniq -d | wc -l)"
check "$([ "$SAME" = "0" ] && echo 0 || echo 1)" "two instances with different names write no series which the other one writes too ($SAME are the same)"

run_case plain2
SAME="$( (metric_lines "$WORK/plain.prom"; metric_lines "$WORK/plain2.prom") | awk '{print $1}' | sort | uniq -d | wc -l)"
check "$([ "$SAME" -gt 0 ] && echo 0 || echo 1)" "two instances without a name do write the same series ($SAME): this is the clash which the label is for"

finish
