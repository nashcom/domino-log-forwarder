# OTLP test container

A small receiver for testing `otelfwd` (or any other OTLP/HTTP log exporter). It stores the body of every request in its own JSON file. For load tests it can also count the events of a test in memory instead, and report what is missing, duplicated or slow: see [Load test ledger](#load-test-ledger).

```text
client  --->  NGINX  ---- UNIX socket ---->  otel-sink  --->  one JSON file per POST
              4318 http                       (no HTTP library,
              4319 https                       no TLS, no auth)
              4320 always fails
```

NGINX handles what a test receiver should not re-implement: HTTP, TLS, the bearer token check, size limits and the simulated errors.
It forwards every accepted request over a UNIX socket to `otel-sink`, which writes the files.

The image is built from Alpine. NGINX is installed from the Alpine repository, `otel-sink` is compiled in the build. Everything runs as a non-root user.

## Build and run

With Docker Compose. The image is built from the Dockerfile when it does not exist yet:

```bash
cd tools/otel-sink

BUILDKIT_PROGRESS=plain OTEL_SINK_UID=$(id -u) OTEL_SINK_GID=$(id -g) docker compose up
```

`BUILDKIT_PROGRESS=plain` prints the build log as plain text instead of the animated view. It is optional.
Add `--build` to build the image again after changing the sources. The files are written to `tools/otel-sink/otel-data`.
`OTEL_SINK_UID` and `OTEL_SINK_GID` make them belong to you. Without them the container runs with uid and gid 1000.
The settings below can be passed the same way, or written into a file named `.env` next to `docker-compose.yml`:

```bash
OTEL_SINK_TOKEN=secret OTEL_SINK_UID=$(id -u) OTEL_SINK_GID=$(id -g) docker compose up
```

Without Compose:

```bash
BUILDKIT_PROGRESS=plain docker build -t otel-test-sink tools/otel-sink

mkdir -p otel-data

docker run --rm --name otel-test-sink --hostname otel-test-sink --user "$(id -u):$(id -g)" \
  -p 127.0.0.1:4318:4318 -p 127.0.0.1:4319:4319 -p 127.0.0.1:4320:4320 \
  -v "$PWD/otel-data:/data" otel-test-sink
```

Without `--user` the container runs as user `otel` (uid 1000), which needs a volume it can write to.

The container is named `otel-test-sink` and its host name is `otel-test-sink` as well (Docker would otherwise use the random container id). Other containers on the same Docker network reach it under that name, for example `https://otel-test-sink:4319/v1/logs`.

## Start with run.sh

`run.sh` does the steps above for you: it sets the user of the container, creates `otel-data`, builds the image if needed and starts the container.

```bash
bash run.sh              # foreground. Ctrl-C stops the container and removes it (docker compose down)
bash run.sh --detach     # in the background, returns when port 4318 answers
bash run.sh --stop       # docker compose down
bash run.sh --rebuild    # first remove the image (docker compose down --rmi local) and build it again without the build cache
```

`--detach` and `--rebuild` can be combined. The received files stay in `otel-data/received` after `down`. As root, the container runs as the owner of the directory, so the files do not belong to root. The settings below are passed on to `docker-compose.yml` as environment variables.

## Shell in the container

The image contains `bash`:

```bash
docker exec -it otel-test-sink bash
```

Useful places inside the container: `/data/received` (the files), `/data/nginx-access.log`, `/tmp/otel-sink/nginx.conf` (the NGINX configuration which is generated at start) and `/tmp/otel-sink/locations.conf`.

## Ports

| Port   | Protocol | Behavior                                                                                 |
| :----- | :------- | :--------------------------------------------------------------------------------------- |
| `4318` | HTTP     | OTLP/HTTP endpoint. Every request is stored. Answers `200` and `{}`                      |
| `4319` | HTTPS    | The same with a self-signed certificate                                                  |
| `4320` | HTTP     | Always answers with an error (default `503`) and stores nothing. For retry and WAL tests |

`GET /healthz` answers `200` on all ports and needs no token.

## Settings

Environment variables of the container (`docker run -e NAME=value`, or the same names in the environment of `docker compose`):

| Variable                | Description                                                                         | Default                                                                              |
| :---------------------- | :---------------------------------------------------------------------------------- | :----------------------------------------------------------------------------------- |
| `OTEL_SINK_TOKEN`       | Require `Authorization: Bearer <token>` on 4318 and 4319, otherwise answer `401`    | no token required                                                                    |
| `OTEL_SINK_FAIL_STATUS` | Status code of port 4320 (400 to 599)                                               | `503`                                                                                |
| `OTEL_SINK_SAN`         | Names in the certificate, needed when the client uses another name than `localhost` | `DNS:localhost,IP:127.0.0.1` (Compose adds `DNS:otel-test-sink`, the container name) |
| `OTEL_SINK_RAW`         | `1`: write the bodies as received, do not pretty print JSON                         | pretty print                                                                         |

The token may contain letters, digits and `. _ ~ + / = -`. Invalid values stop the container with a message.

## Files

The volume `/data` contains:

| Path               | Content                                                                |
| :----------------- | :--------------------------------------------------------------------- |
| `received/`        | One file per POST                                                      |
| `certs/cert.pem`   | The generated certificate. Use it as `OTLP_CA_FILE` for the HTTPS port |
| `certs/key.pem`    | Its private key                                                        |
| `nginx-access.log` | NGINX access log, including refused and failing requests               |

File names look like `000001_20260918T201530.123Z_v1-logs.json`: a sequence number, the UTC time, and the request path.
The numbering continues after a restart.

* A JSON body is pretty printed (`OTEL_SINK_RAW=1` keeps it as received).
* A body which is not JSON, for example protobuf, is written as `.bin`.
* A gzip compressed body is decompressed first. Other encodings are stored as `.bin`.
* Each request is also logged by the container: sequence number, path, number of log records, size and file name. Requests with load test events are neither stored nor logged one by one, see [Load test ledger](#load-test-ledger).

The certificate is kept in the volume. It is generated again when `OTEL_SINK_SAN` changes or when the files are deleted.

## Quick test with curl

Run these in `tools/otel-sink` (where `otel-data` is). Without a token, leave out the `Authorization` header.

```bash
curl -s http://127.0.0.1:4318/healthz
```

```bash
curl -s -w '\nHTTP %{http_code}\n' -X POST http://127.0.0.1:4318/v1/logs -H 'Content-Type: application/json' -d '{"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"curl-test"}}]},"scopeLogs":[{"scope":{"name":"curl"},"logRecords":[{"timeUnixNano":"1785760554890000000","severityNumber":9,"severityText":"INFO","body":{"stringValue":"hello from curl"}}]}]}]}'
```

The answer is `{}` with `HTTP 200`. The request is now a file:

```bash
ls otel-data/received
```

```bash
cat otel-data/received/$(ls otel-data/received | tail -1)
```

HTTPS, trusting the generated certificate:

```bash
curl -s -w '\nHTTP %{http_code}\n' --cacert otel-data/certs/cert.pem -X POST https://localhost:4319/v1/logs -H 'Content-Type: application/json' -d '{"resourceLogs":[{"scopeLogs":[{"scope":{"name":"curl"},"logRecords":[{"body":{"stringValue":"hello over https"}}]}]}]}'
```

With a token (container started with `OTEL_SINK_TOKEN=secret`). Without the header the answer is `401`:

```bash
curl -s -w '\nHTTP %{http_code}\n' -X POST http://127.0.0.1:4318/v1/logs -H 'Content-Type: application/json' -H 'Authorization: Bearer secret' -d '{"resourceLogs":[{"scopeLogs":[{"scope":{"name":"curl"},"logRecords":[{"body":{"stringValue":"with token"}}]}]}]}'
```

The failing port answers `503` and stores nothing:

```bash
curl -s -w '\nHTTP %{http_code}\n' -X POST http://127.0.0.1:4320/v1/logs -H 'Content-Type: application/json' -d '{}'
```

Use `localhost` (not `127.0.0.1`) for HTTPS: the certificate contains the names `localhost` and `otel-test-sink`.
Without `--cacert`, curl refuses the self-signed certificate with exit code 60.

## Load test ledger

For the load test of `nginx/loadtest` (see `nginx/README.md`) the sink keeps a ledger of test events in memory. The requests are `GET /otelfwd-test/<thread>/<count>/<sent_ns>`. The `url.path` attribute of an access log record carries the three numbers. OTLP requests which contain such records are answered with 200 and are **not** written to a file or printed for every request (a summary line appears every 5 seconds). Other OTLP requests are stored as before.

| Request            | What it does                                                                                                                                         |
| :----------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------- |
| `POST /test/reset` | Body `{"threads": 32, "events_per_thread": 100000}`. Clears the ledger and sets the expected range (from 1). Limits: 4096 threads, 20 million events |
| `GET /test/stats`  | The numbers as JSON, see below                                                                                                                       |
| `POST /test/fail`  | Body `{"status": 503}`: every OTLP request is answered with this status and not stored. `{"status": 0}` ends it. For WAL tests                       |

The requests go through the NGINX of the container like all others, so `OTEL_SINK_TOKEN` applies to them too.

```bash
curl -s -X POST http://127.0.0.1:4318/test/reset -d '{"threads": 4, "events_per_thread": 1000}'
curl -s http://127.0.0.1:4318/test/stats
```

`/test/stats` returns, among others:

| Field                                     | Meaning                                                                                                                                                                                                                                                                                                                                  |
| :---------------------------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `expected`                                | `threads * events_per_thread`                                                                                                                                                                                                                                                                                                            |
| `unique`                                  | Different `{thread, count}` received                                                                                                                                                                                                                                                                                                     |
| `deliveries`, `duplicates`                | Valid test records received, and the deliveries beyond the first one (`deliveries - unique`)                                                                                                                                                                                                                                             |
| `missing`                                 | `expected - unique`. `missing_sample` lists the first ten                                                                                                                                                                                                                                                                                |
| `unexpected`                              | Test records with a thread or count outside the range                                                                                                                                                                                                                                                                                    |
| `malformed`                               | Test paths which cannot be read                                                                                                                                                                                                                                                                                                          |
| `mixed_batches`                           | OTLP requests with test and other records. They are not written to a file either                                                                                                                                                                                                                                                         |
| `other_records`, `other_sample`           | Records which are not test events, received while a test is active, and the first few as text. NGINX writes its own problems to the same syslog socket, for example `[warn] send() to syslog failed while logging request ... request: "GET /otelfwd-test/..."`: such a line takes the place of an event which was lost before `otelfwd` |
| `nginx_reported_lost`, `missing_reported` | Events named in such a `send() to syslog failed` line, and how many of the missing events are among them. If `missing_reported` equals `missing`, all loss is between NGINX and `otelfwd`                                                                                                                                                |
| `timestamp_conflicts`                     | The same key with a different send time                                                                                                                                                                                                                                                                                                  |
| `latency`, `stages`                       | End to end latency and its two parts: min, average, p50, p95, p99, max in nanoseconds                                                                                                                                                                                                                                                    |
| `first_arrival_ns`, `last_arrival_ns`     | Unix time of the first and the last delivery                                                                                                                                                                                                                                                                                             |
| `failed_requests`, `fail_status`          | Requests answered with the status of `/test/fail`, and the status which is set                                                                                                                                                                                                                                                           |
| `threads`                                 | Per thread: `expected`, `unique`, `deliveries`, `duplicates`, `missing`                                                                                                                                                                                                                                                                  |

The sink handles every connection in its own thread. The ledger has one mutex, which is held for the time of one request. It uses about 40 bytes per expected event.

## Example scripts

The folder `examples` contains test scripts which use curl (and only standard tools, no `jq`). They need bash, so on Windows run them in WSL or Git Bash.

```bash
cd tools/otel-sink

bash examples/run-all.sh
```

| Script              | What it does                                                                                        |
| :------------------ | :-------------------------------------------------------------------------------------------------- |
| `send-log.sh`       | Sends one log record. The smallest example: `bash examples/send-log.sh "message" 17`                |
| `test-basic.sh`     | Health check, one POST, the file name, pretty printed JSON, another path                            |
| `test-https.sh`     | HTTPS with the generated certificate, and the refusal without `--cacert` (curl exit code 60)        |
| `test-token.sh`     | Bearer token: 401 without, with a wrong token, and 200 with the right one. Skipped without a token  |
| `test-fail-port.sh` | The port which always fails: status code, nothing stored                                            |
| `test-gzip.sh`      | Compressed request body is stored decompressed. A body which is not gzip gets 400                   |
| `test-errors.sh`    | Binary body and broken JSON (stored as `.bin`), a PUT (405), a path with `..`                       |
| `test-parallel.sh`  | 20 clients at the same time (`PARALLEL=50` for more): every request in its own file, numbers unique |
| `test-large.sh`     | 3000 log records in one POST (`LARGE_RECORDS=10000`), and a 33 MB request which is refused with 413 |
| `run-all.sh`        | Runs all tests and prints a summary                                                                 |

Each script prints `PASS`, `FAIL` or `SKIP` per check and returns 0 when nothing failed. `run-all.sh` returns 2 when the sink is not reachable.

The scripts are configured with environment variables. The defaults match the container as started by `docker compose up`:

| Variable           | Description                                                                 | Default                     |
| :----------------- | :-------------------------------------------------------------------------- | :-------------------------- |
| `SINK_HOST`        | Address of the container                                                    | `127.0.0.1`                 |
| `SINK_HTTP_PORT`   | OTLP/HTTP port                                                              | `4318`                      |
| `SINK_HTTPS_PORT`  | OTLP/HTTPS port                                                             | `4319`                      |
| `SINK_FAIL_PORT`   | The port which always fails                                                 | `4320`                      |
| `SINK_TOKEN`       | Bearer token, when the container was started with `OTEL_SINK_TOKEN`         | none                        |
| `SINK_FAIL_STATUS` | Status code of the failing port                                             | `503`                       |
| `SINK_DATA_DIR`    | The `/data` volume on the host                                              | `tools/otel-sink/otel-data` |
| `SINK_CONTAINER`   | Container name, used with `docker exec` when `SINK_DATA_DIR` does not exist | `otel-test-sink`            |

The scripts check the received files in `SINK_DATA_DIR`, or with `docker exec` if that directory does not exist. Without access to the files, the checks on the files are skipped.
Every run marks its records with its own id, so old files do not disturb a run.

```bash
# the container was started with OTEL_SINK_TOKEN=secret
SINK_TOKEN=secret bash examples/run-all.sh

# a container on another host
SINK_HOST=192.168.1.20 SINK_DATA_DIR=/nonexistent bash examples/run-all.sh
```

The scripts only add files to the sink. They never delete anything. Delete the files in `otel-data/received` yourself when you want to start clean.

## Use with otelfwd

```bash
# HTTP
OTLP_PUSH_API_URL=http://127.0.0.1:4318/v1/logs otelfwd ...

# HTTP with a token (container started with -e OTEL_SINK_TOKEN=secret)
OTLP_PUSH_API_URL=http://127.0.0.1:4318/v1/logs OTLP_PUSH_TOKEN=secret otelfwd ...

# HTTPS: trust the generated certificate
OTLP_PUSH_API_URL=https://127.0.0.1:4319/v1/logs OTLP_CA_FILE=otel-data/certs/cert.pem otelfwd ...

# Retry and WAL: this port always fails, the lines stay in the WAL until the endpoint is changed
OTLP_PUSH_API_URL=http://127.0.0.1:4320/v1/logs otelfwd ...
```

## Without Docker

`otel-sink` can be built and run on its own (needs the rapidjson and zlib headers):

```bash
make otel-sink
./otel-sink --socket /tmp/otel-sink/sink.sock --dir otel-received
```

To run the whole container setup on a host that has NGINX and OpenSSL installed, run the entrypoint with its paths pointed to your directories:

```bash
OTEL_SINK_DATA=$PWD/otel-data OTEL_SINK_TMP=/tmp/otel-sink OTEL_SINK_BIN=$PWD/otel-sink \
OTEL_SINK_TEMPLATE=tools/otel-sink/nginx.conf.template sh tools/otel-sink/entrypoint.sh
```

## Notes

* This is a test tool. Do not expose the ports beyond localhost. The certificate is self-signed, and without a token anyone who can reach the ports can send data.
* Requests are limited to 32 MB by NGINX.
* `otel-sink` has no authentication. Only NGINX can reach its socket, which is inside the container and only accessible for the container user.
* NGINX buffers every request completely before forwarding it, so chunked requests arrive at `otel-sink` with a `Content-Length`.
