#!/usr/bin/env bash
# Sends one log record to the sink. The smallest example.
#
#   bash send-log.sh "message"                       to the HTTP port
#   bash send-log.sh "message" 17                    with a severity number (9 = INFO, 13 = WARN, 17 = ERROR)
#   SINK_TOKEN=secret bash send-log.sh "message"     when the container requires a token
#   SINK_HTTP_PORT=14318 bash send-log.sh "message"  another port

. "$(dirname "${BASH_SOURCE[0]}")/lib.sh"

MESSAGE="${1:-hello from send-log.sh}"

require_sink

post_text "$(http_url /v1/logs)" "$(otlp_body "$MESSAGE" "${2:-9}")"

echo "HTTP $HTTP_CODE  $RESPONSE"

[ "$HTTP_CODE" = "200" ]
