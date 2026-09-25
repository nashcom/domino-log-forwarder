#!/usr/bin/env bash
# Test: two otelfwd instances must not share an OTELFWD_DATA_DIR.
#
# The WAL is locked, so the second instance on the same data directory cannot open it. What it does then depends on the mode:
#
#   - with -nostdin it exits with the code 1: it would run without keeping failed pushes, and nothing reads a pipe which
#     an exit would close
#   - in pipe mode it keeps running (an exit would close the pipe of the server), and says so in its output
#
# A second instance with a data directory of its own is not affected.
#
# What the script needs: the built forwarder (make), or OTELFWD_BIN, see common.sh. It uses UNIX sockets in a temporary directory
# only: no TCP port, no network. The push URL points to a port where nothing listens, and no record is sent.
#
#   make otelfwd && ./tests/test_shared_data_dir.sh

. "$(dirname "${BASH_SOURCE[0]}")/common.sh" || exit 2

if ! command -v timeout >/dev/null 2>&1; then
  echo "this script needs the timeout command" >&2
  exit 2
fi

LOG_NAMES="first second third fourth"

# Runs otelfwd with its own data directory and UNIX socket. The rest of the arguments are the arguments of otelfwd
run_fwd() {
  local Data="$1" Socket="$2"

  shift 2
  OTELFWD_DATA_DIR="$Data" OTELFWD_UNIX_SOCKET="$Socket" OTLP_PUSH_API_URL="http://127.0.0.1:1/v1/logs" "$OTELFWD_BIN" "$@"
}

DATA_A="$WORK/data-a"
DATA_B="$WORK/data-b"
mkdir -p "$DATA_A" "$DATA_B"

# 1. The first instance owns the data directory A
run_fwd "$DATA_A" "$WORK/a.sock" -nostdin > "$WORK/first.log" 2>&1 &
FIRST=$!
PIDS="$PIDS $FIRST"

wait_for_socket "$WORK/a.sock"
check $? "the first instance starts and opens its socket"

# 2. -nostdin on the same data directory, with a socket of its own: exit code 1, and the reason is in the output
timeout 15 env OTELFWD_DATA_DIR="$DATA_A" OTELFWD_UNIX_SOCKET="$WORK/second.sock" OTLP_PUSH_API_URL="http://127.0.0.1:1/v1/logs" "$OTELFWD_BIN" -nostdin > "$WORK/second.log" 2>&1
Rc=$?

if [ "$Rc" = "1" ]; then check 0 "-nostdin on a shared data directory exits with the code 1"; else check 1 "-nostdin on a shared data directory exits with the code 1 (the code was $Rc, 124 is the timeout: it did not exit)"; fi

grep -q "in use by another" "$WORK/second.log"
check $? "the output says that the WAL is in use by another process"

grep -q "needs a WAL" "$WORK/second.log"
check $? "the output says that -nostdin needs a WAL"

# 3. Pipe mode on the same data directory: it keeps running (a pipe is open), and the output says that there is no WAL
mkfifo "$WORK/stdin.fifo"
run_fwd "$DATA_A" "$WORK/third.sock" < "$WORK/stdin.fifo" > "$WORK/third.log" 2>&1 &
THIRD=$!
PIDS="$PIDS $THIRD"
exec 9> "$WORK/stdin.fifo"

wait_for_socket "$WORK/third.sock"
sleep 1
kill -0 "$THIRD" 2>/dev/null
check $? "pipe mode on a shared data directory keeps running"

grep -q "in use by another" "$WORK/third.log"
check $? "pipe mode says that the WAL is in use by another process"

grep -q "cannot be opened, failed pushes cannot be kept" "$WORK/third.log"
check $? "pipe mode shows in its summary that failed pushes cannot be kept"

exec 9>&-
wait "$THIRD" 2>/dev/null

# 4. -nostdin with a data directory of its own is not affected, and stops cleanly on SIGTERM
run_fwd "$DATA_B" "$WORK/b.sock" -nostdin > "$WORK/fourth.log" 2>&1 &
FOURTH=$!
PIDS="$PIDS $FOURTH"

wait_for_socket "$WORK/b.sock"
check $? "an instance with a data directory of its own starts next to the first one"

kill -0 "$FOURTH" 2>/dev/null
check $? "and it keeps running"

kill -TERM "$FOURTH" 2>/dev/null
wait "$FOURTH" 2>/dev/null
Rc=$?
check "$([ "$Rc" = "0" ] && echo 0 || echo 1)" "and it ends with the code 0 on SIGTERM (the code was $Rc)"

kill -0 "$FIRST" 2>/dev/null
check $? "the first instance was not disturbed by any of this"

finish
