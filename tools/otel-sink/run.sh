#!/usr/bin/env bash
# Starts the OTLP test container (otel-test-sink): NGINX and otel-sink in one image, see README.md.
#
#   bash run.sh             build the image if needed and start the container in the foreground.
#                           Ctrl-C stops the container and removes it (docker compose down)
#   bash run.sh --detach    start it in the background and return once the port answers
#   bash run.sh --stop      docker compose down
#   bash run.sh --rebuild   first remove the image (docker compose down --rmi local) and build it again without the build cache
#
# --detach and --rebuild can be combined. The received files are in otel-data/received. They stay after "down".
#
# Settings (environment variables), passed on to docker-compose.yml:
#
#   OTEL_SINK_TOKEN        require "Authorization: Bearer <token>" on ports 4318 and 4319
#   OTEL_SINK_FAIL_STATUS  status of port 4320, which always fails (default 503)
#   OTEL_SINK_SAN          names of the generated certificate
#   OTEL_SINK_RAW          1: write the bodies as received
#   OTEL_SINK_UID, OTEL_SINK_GID   user of the container. Default: your user id. As root: the owner of this directory,
#                          so the files in otel-data do not belong to root.

if [ -z "${BASH_VERSION:-}" ]; then
  echo "this script needs bash" >&2
  exit 2
fi

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"

DETACH=0
STOP=0
REBUILD=0
COMPOSE_UP=0
KEEP=0

for Arg in "$@"; do
  case "$Arg" in
    --detach)  DETACH=1 ;;
    --stop)    STOP=1 ;;
    --rebuild) REBUILD=1 ;;
    *)
      echo "usage: bash run.sh [--detach] [--rebuild] | --stop" >&2
      exit 2
      ;;
  esac
done

if [ "$STOP" = "1" ] && [ "$((DETACH + REBUILD))" != "0" ]; then
  echo "--stop cannot be combined with other options" >&2
  exit 2
fi

if ! command -v docker >/dev/null 2>&1; then
  echo "docker was not found" >&2
  exit 2
fi

export BUILDKIT_PROGRESS="${BUILDKIT_PROGRESS:-plain}"

cd "$HERE" || exit 1

if [ "$STOP" = "1" ]; then
  docker compose down
  exit 0
fi

# The container runs as this user, so the files in otel-data belong to it. As root: the owner of the repository directory
RUN_UID="${OTEL_SINK_UID:-$(id -u)}"
RUN_GID="${OTEL_SINK_GID:-$(id -g)}"

if [ "$RUN_UID" = "0" ] && [ -z "${OTEL_SINK_UID:-}" ]; then
  RUN_UID="$(stat -c %u "$HERE")"
  RUN_GID="$(stat -c %g "$HERE")"

  if [ "$RUN_UID" = "0" ]; then
    RUN_UID=1000
    RUN_GID=1000
  fi

  echo "Running as root: the container runs as uid $RUN_UID, gid $RUN_GID (owner of $HERE)."
fi

export OTEL_SINK_UID="$RUN_UID"
export OTEL_SINK_GID="$RUN_GID"

# The directory has to belong to that user. If Docker creates it for the mount, it belongs to root and the sink cannot write there
mkdir -p "$HERE/otel-data" || exit 1

if [ "$(id -u)" = "0" ]; then
  chown "$RUN_UID:$RUN_GID" "$HERE/otel-data" || exit 1
fi


# Removes the container when the script ends. A detached start keeps it.
cleanup()
{
  if [ "$COMPOSE_UP" = "1" ] && [ "$KEEP" != "1" ]; then
    (cd "$HERE" && docker compose down)
  fi
}

trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM


if [ "$REBUILD" = "1" ]; then
  echo "Removing the image and building it again without the build cache"
  docker compose down --rmi local || exit 1
  docker compose build --no-cache || exit 1
fi

if [ "$DETACH" = "1" ]; then
  COMPOSE_UP=1
  docker compose up -d --build || exit 1

  for _ in $(seq 1 300); do
    if (exec 3<>/dev/tcp/127.0.0.1/4318) 2>/dev/null; then
      KEEP=1
      echo "Running. OTLP: http://127.0.0.1:4318/v1/logs . Files: $HERE/otel-data/received . Stop it with: bash run.sh --stop"
      exit 0
    fi

    sleep 0.2
  done

  echo "The sink did not answer on port 4318 within 60 seconds" >&2
  exit 1
fi

COMPOSE_UP=1
docker compose up --build
