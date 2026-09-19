# NGINX test container for the syslog input of otelfwd: a plain HTTP server which logs to otelfwd through its socket.
#
#   docker build -t otelfwd-test-nginx nginx
#
# See docker-compose.yml for how to start it. The image is built from Alpine and installs NGINX from the Alpine repository.
# It runs as a non-root user (the compose file sets the user id, so it can write to the socket of otelfwd).
# The container uses the network of the host: no NAT and no Docker proxy between the load test and NGINX.

ARG ALPINE_VERSION=latest

FROM alpine:${ALPINE_VERSION}

# bash and curl are only there for docker exec -it otelfwd-test-nginx bash
RUN apk add --no-cache nginx bash curl \
 && mkdir -p /www /var/log/nginx \
 && chmod 1777 /var/log/nginx

COPY nginx.conf.template /etc/nginx/nginx.conf.template
COPY otelfwd-logging.conf /etc/nginx/otelfwd-logging.conf
COPY index.html /www/index.html
COPY entrypoint.sh /usr/local/bin/entrypoint.sh

# The files may have been checked out with Windows line endings
RUN sed -i 's/\r$//' /etc/nginx/nginx.conf.template /etc/nginx/otelfwd-logging.conf /usr/local/bin/entrypoint.sh \
 && chmod 0755 /usr/local/bin/entrypoint.sh

# Any user id works: NGINX needs no user of its own. Its temporary files are in /tmp, the plain test logs in /var/log/nginx.
USER 1000:1000

# The port is OTELFWD_NGINX_PORT (default 18080) on 127.0.0.1 of the host
HEALTHCHECK --interval=10s --timeout=3s --start-period=5s CMD curl -fsS -o /dev/null "http://127.0.0.1:${OTELFWD_NGINX_PORT:-18080}/health" || exit 1

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
