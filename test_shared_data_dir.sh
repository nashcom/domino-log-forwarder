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
# What the script needs: the built forwarder (make), or OTELFWD_BIN. It uses UNIX sockets in a temporary directory only: no TCP
# port, no network. The push URL points to a port where nothing listens, and no record is sent, so nothing is pushed.
#
#   make otelfwd && ./test_shared_data_dir.sh

if [ -z "${BASH_VERSION:-}" ]; then
  echo "this script needs bash" >&2
  exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
USER_BIN="${OTELFWD_BIN:-}"
OTELFWD_BIN="${OTELFWD_BIN:-$HERE/otelfwd}"

if [ ! -x "$OTELFWD_BIN" ]; then
  echo "otelfwd was not found: $OTELFWD_BIN. Build it with \"make\" in the repository, or set OTELFWD_BIN." >&2
  exit 2
fi

# A forwarder which is older than its source does not have the behaviour under test, and the failures look like a bug of the
# program. Only checked for the forwarder of this repository, not for one which was named in OTELFWD_BIN
if [ -z "$USER_BIN" ] && [ -f "$HERE/otelfwd.cpp" ] && [ "$OTELFWD_BIN" -ot "$HERE/otelfwd.cpp" ]; then
  echo "otelfwd is older than otelfwd.cpp. Build it first: make otelfwd" >&2
  exit 2
fi

if ! command -v timeout >/dev/null 2>&1; then
  echo "this script needs the timeout command" >&2
  exit 2
fi

WORK="$(mktemp -d)"
PIDS=""
PASSED=0
FAILED=0

cleanup() {
  local Pid

  exec 9>&-
  for Pid in $PIDS; do kill "$Pid" 2>/dev/null; done
  wait 2>/dev/null
  rm -rf "$WORK"
}

trap cleanup EXIT

check() {
  local Ok="$1" Name="$2"

  if [ "$Ok" = "0" ]; then
    echo "[PASS]  $Name"
    PASSED=$((PASSED + 1))
  else
    echo "[FAIL]  $Name"
    FAILED=$((FAILED + 1))
  fi
}

# Waits until a UNIX socket exists, up to 10 seconds
wait_for_socket() {
  local Socket="$1" i

  for i in $(seq 1 100); do
    if [ -S "$Socket" ]; then return 0; fi
    sleep 0.1
  done

  return 1
}

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

echo
echo "$PASSED passed, $FAILED failed"

if [ "$FAILED" != "0" ]; then
  for Log in first second third fourth; do
    if [ -s "$WORK/$Log.log" ]; then
      echo >&2
      echo "--- output of the $Log instance (the last 12 lines) ---" >&2
      tail -n 12 "$WORK/$Log.log" >&2
    fi
  done

  exit 1
fi

exit 0
