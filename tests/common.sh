#!/usr/bin/env bash
# Shared part of the test scripts in this directory. Source it, do not run it.
#
# What a script gets from it:
#
#   OTELFWD_BIN     the forwarder: the one in the repository (built with "make otelfwd"), or the one named in OTELFWD_BIN.
#                   The script stops with the code 2 if it is not there, or if it is older than otelfwd.cpp: a forwarder which
#                   does not have the behaviour under test makes the failures look like a bug of the program
#   WORK            a temporary directory, deleted when the script ends
#   PIDS            the processes to stop when the script ends: add every process which was started in the background
#   LOG_NAMES       set it to the names of the logs, $WORK/<name>.log: the last lines of each are shown if a check failed
#   check           check <0 for pass, anything else for fail> "the name of the check". Counts, and prints [PASS] or [FAIL]
#   wait_for_socket wait_for_socket <path>: waits up to 10 seconds until a UNIX socket exists, returns 1 if it does not
#   finish          prints the totals, the logs if something failed, and exits: 0 if every check passed, else 1
#
# What every script does not need: a network, or a TCP port. They use UNIX sockets in the temporary directory, and the push URL
# points to a port where nothing listens.

if [ -z "${BASH_VERSION:-}" ]; then
  echo "these scripts need bash" >&2
  exit 2
fi

TESTS_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
ROOT_DIR="$(cd "$TESTS_DIR/.." && pwd)"

USER_BIN="${OTELFWD_BIN:-}"
OTELFWD_BIN="${OTELFWD_BIN:-$ROOT_DIR/otelfwd}"

if [ ! -x "$OTELFWD_BIN" ]; then
  echo "otelfwd was not found: $OTELFWD_BIN. Build it with \"make otelfwd\" in the repository, or set OTELFWD_BIN." >&2
  exit 2
fi

# Only checked for the forwarder of this repository, not for one which was named in OTELFWD_BIN
if [ -z "$USER_BIN" ] && [ -f "$ROOT_DIR/otelfwd.cpp" ] && [ "$OTELFWD_BIN" -ot "$ROOT_DIR/otelfwd.cpp" ]; then
  echo "otelfwd is older than otelfwd.cpp. Build it first: make otelfwd" >&2
  exit 2
fi

WORK="$(mktemp -d)"
PIDS=""
LOG_NAMES=""
PASSED=0
FAILED=0

cleanup() {
  local Pid

  # A script which opened the file descriptor 9 (a pipe to STDIN of a forwarder) closes it here, so nothing waits for it
  exec 9>&- 2>/dev/null
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

finish() {
  local Log

  echo
  echo "$PASSED passed, $FAILED failed"

  if [ "$FAILED" != "0" ]; then
    for Log in $LOG_NAMES; do
      if [ -s "$WORK/$Log.log" ]; then
        echo >&2
        echo "--- output of the $Log instance (the last 12 lines) ---" >&2
        tail -n 12 "$WORK/$Log.log" >&2
      fi
    done

    exit 1
  fi

  exit 0
}
