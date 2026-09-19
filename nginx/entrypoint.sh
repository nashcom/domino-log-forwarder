#!/bin/sh
# Entrypoint of the NGINX test container.
#
# The container uses the network of the host (network_mode: host), so NGINX has to listen on the port which was chosen for it:
# OTELFWD_NGINX_PORT, default 18080. NGINX cannot read an environment variable in its configuration. This script writes the
# configuration from the template, with the port filled in, and starts NGINX in the foreground.

PORT="${OTELFWD_NGINX_PORT:-18080}"

case "$PORT" in
  ""|*[!0-9]*)
    echo "OTELFWD_NGINX_PORT has to be a port number, not \"$PORT\"" >&2
    exit 1
    ;;
esac

sed "s/@PORT@/$PORT/g" /etc/nginx/nginx.conf.template > /tmp/nginx.conf || exit 1

echo "NGINX listens on 127.0.0.1:$PORT (network of the host)"

# -e stderr: the startup log of NGINX goes to stderr and not to a file which a non-root user cannot open
exec nginx -c /tmp/nginx.conf -e stderr -g "daemon off;"
