#!/usr/bin/env bash
# Pipe mode: pushes log lines from STDIN, the way otelfwd runs behind a Domino server ("server | otelfwd").
#
#   bash push-stdin.sh                              pushes sample-console.log
#   bash push-stdin.sh /path/to/some.log            pushes another file
#   OTLP_PUSH_API_URL=https://otel.example.com:4318/v1/logs OTLP_PUSH_TOKEN=secret bash push-stdin.sh
#
# The same without the script:
#
#   cat sample-console.log | OTLP_PUSH_API_URL=http://127.0.0.1:4318/v1/logs ../otelfwd
#
# Note: "otelfwd -cfg" only prints the configuration and ends, it does not read STDIN.

. "$(dirname "${BASH_SOURCE[0]}")/common.sh"

INPUT="${1:-$EXAMPLES_DIR/sample-console.log}"

echo "input    : $INPUT ($(wc -l < "$INPUT" | tr -d ' ') lines)"
echo "endpoint : $OTLP_PUSH_API_URL"
echo "forwarder: $OTELFWD_BIN"
echo

# The forwarder ends when STDIN is closed. Its own messages go to stderr.
cat "$INPUT" | "$OTELFWD_BIN"

report
