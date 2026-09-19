# OTLP test container: NGINX and otel-sink in one Alpine based image.
#
#   docker build -t otel-test-sink tools/otel-sink
#   docker run --rm --name otel-test-sink --hostname otel-test-sink -p 127.0.0.1:4318:4318 -p 127.0.0.1:4319:4319 -p 127.0.0.1:4320:4320 \
#          --user "$(id -u):$(id -g)" -v "$PWD/otel-data:/data" otel-test-sink
#
#   docker exec -it otel-test-sink bash
#
# The image is built from Alpine and installs NGINX from the Alpine repository. otel-sink is compiled in the first stage.
# Everything runs as a non-root user.

ARG ALPINE_VERSION=latest

FROM alpine:${ALPINE_VERSION} AS build

RUN apk add --no-cache g++ musl-dev rapidjson-dev zlib-dev zlib-static

COPY otel-sink.cpp test_ledger.hpp /src/

RUN g++ -std=c++17 -O2 -Wall -Wextra -static -pthread -o /otel-sink /src/otel-sink.cpp -lz


FROM alpine:${ALPINE_VERSION}

# bash is only there for docker exec -it otel-test-sink bash. The entrypoint is a plain POSIX sh script.
RUN apk add --no-cache nginx openssl tini bash \
 && addgroup -g 1000 otel \
 && adduser -D -u 1000 -G otel otel \
 && mkdir -p /data /etc/otel-sink \
 && chown otel:otel /data

COPY --from=build /otel-sink /usr/local/bin/otel-sink
COPY nginx.conf.template /etc/otel-sink/nginx.conf.template
COPY entrypoint.sh /usr/local/bin/entrypoint.sh

# The script may have been checked out with Windows line endings
RUN sed -i 's/\r$//' /usr/local/bin/entrypoint.sh /etc/otel-sink/nginx.conf.template \
 && chmod 0755 /usr/local/bin/entrypoint.sh

USER otel

VOLUME /data

EXPOSE 4318 4319 4320

HEALTHCHECK --interval=10s --timeout=3s --start-period=5s CMD wget -q -O /dev/null http://127.0.0.1:4318/healthz || exit 1

ENTRYPOINT ["/sbin/tini", "--", "/usr/local/bin/entrypoint.sh"]
