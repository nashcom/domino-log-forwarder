# NGINX logging configuration for otelfwd

Helper configuration to send NGINX access and error logs to `otelfwd` via syslog, with a test container and a load test.

**Status:** the forwarder side is implemented (`OTELFWD_SYSLOG_SOCKET`, see the [Syslog input](../README.md#syslog-input) of `otelfwd`). Tested with NGINX 1.28 against a real `otelfwd`.

## Quick start

You need Linux (or WSL) with Docker. Run this from the repository root:

```bash
make                                 # builds otelfwd
./tools/otel-sink/run.sh --detach    # the OTLP test receiver, in the background
./nginx/run.sh                       # otelfwd and the NGINX test container
```

In a second terminal, send some requests:

```bash
curl -s http://127.0.0.1:18080/ ; curl -s http://127.0.0.1:18080/missing.html
```

The access log and the error log of NGINX arrive at the test receiver. They are the files in `tools/otel-sink/otel-data/received`.
Ctrl-C in the first terminal stops the container and `otelfwd`. `./tools/otel-sink/run.sh --stop` stops the receiver.

### Load test in one command

```bash
./nginx/run_loadtest.sh
```

It builds what it needs, starts everything, sends 80,000 requests, checks that every one of them arrived, prints the result and stops everything again. It asks for the size of the test first. Enter takes the default. See [Load test](#load-test).

## How it works

```text
NGINX
  |-- access_log --> JSON (otelfwd_json) --> syslog, tag nginx_access --+
  |                                                                    +--> UNIX datagram socket --> otelfwd
  |-- error_log  --> text               --> syslog, tag nginx_error  --+
```

* NGINX can only send syslog over UDP or a UNIX datagram socket. TCP is not supported.
* Access and error log use one socket. The syslog tag selects the decoder.
* The access log is JSON with OpenTelemetry style attribute names, so almost no mapping is needed.
* The error log stays plain text. The severity comes from the syslog priority, which NGINX sets itself.
* NGINX 1.29.8+ has a native JSON error log (`error_log <file> json`), but it is part of the commercial NGINX subscription and not available in open source NGINX.

| File                                                       | Description                                                    |
| :--------------------------------------------------------- | :------------------------------------------------------------- |
| `otelfwd-logging.conf`                                     | `log_format` plus `access_log` / `error_log` syslog directives |
| `run.sh`, `Dockerfile`, `docker-compose.yml`, `index.html` | [Test container](#test-container)                              |
| `run_loadtest.sh`, `loadtest.cpp`                          | [Load test](#load-test)                                        |

## Use it with your own NGINX

Include the file in the `http { }` context:

```nginx
http {
    include /etc/nginx/otelfwd-logging.conf;
}
```

Requirements:

* The socket path in the file must match `OTELFWD_SYSLOG_SOCKET` of the forwarder. The syslog input is only enabled if that variable is set.
* The NGINX worker user needs write permission on the socket file and search permission on its directory. The forwarder creates the socket with `OTELFWD_SYSLOG_SOCKET_MODE` (default `0600`, which excludes NGINX workers of another user).
* The file also writes `access_test.log` and `error_test.log` next to the syslog output. Remove those two lines for production.

## Test container

A small NGINX container to try the integration: a plain HTTP server on port 18080 of the host (no TLS) which logs to `otelfwd` through its syslog socket. It uses the same [`otelfwd-logging.conf`](otelfwd-logging.conf) as a real NGINX.

```text
curl ──► NGINX container (host port 18080) ──► /run/otelfwd/syslog.sock ══ mounted directory ══ /tmp/otelfwd-syslog on the host ◄── otelfwd ──► OTLP receiver
```

### Start it

`run.sh` starts everything. It does the following steps in this order:

1. Creates the socket directory `/tmp/otelfwd-syslog` for your user.
2. Starts `otelfwd -nostdin` with the syslog socket `/tmp/otelfwd-syslog/syslog.sock`.
3. Waits until `otelfwd` is running and the socket exists. It stops with a message if `otelfwd` cannot start, for example because another one uses the socket.
4. Starts the container with `docker compose up --build`, in the foreground.

```bash
./run.sh
```

`otelfwd` pushes to `http://127.0.0.1:4318/v1/logs` by default, which is the OTLP test container in [`tools/otel-sink`](../tools/otel-sink/README.md). Start that first (`./tools/otel-sink/run.sh --detach`), or set `OTLP_PUSH_API_URL` to another receiver.
Ctrl-C stops the container and removes it (`docker compose down`), then stops the `otelfwd` which `run.sh` started. Nothing else is touched.

### Send requests

```bash
curl -s http://127.0.0.1:18080/ ; curl -s http://127.0.0.1:18080/missing.html ; curl -s http://127.0.0.1:18080/status/500
```

| URL                                | Result                                                         |
| :--------------------------------- | :------------------------------------------------------------- |
| `/`                                | The start page (200)                                           |
| `/missing.html`                    | 404 in the access log, and an `[error]` entry in the error log |
| `/status/301` `/404` `/500` `/503` | The status code, to see different codes in the access log      |
| `/?token=secret`                   | The query string must not show up in any record                |
| `/health`                          | Health check of the container. It is not logged                |

What arrives at the OTLP receiver: one record per request with the tag `nginx_access` as scope, the JSON line as body and the fields as attributes, and the error log lines with the tag `nginx_error` and severity ERROR.
The container also writes the two plain files of `otelfwd-logging.conf`, to compare with what arrives:

```bash
docker exec otelfwd-test-nginx tail -f /var/log/nginx/access_test.log
```

`docker exec -it otelfwd-test-nginx bash` opens a shell in the container.

### Options of run.sh

| Command              | What it does                                                                                                        |
| :------------------- | :------------------------------------------------------------------------------------------------------------------ |
| `./run.sh`           | Starts everything and stays in the foreground                                                                       |
| `./run.sh --detach`  | Starts everything in the background and returns. `otelfwd` keeps running and writes `otelfwd-data/otelfwd.log`      |
| `./run.sh --stop`    | Stops what `--detach` started: `otelfwd`, then `docker compose down`. Only the `otelfwd` of its pid file is stopped |
| `./run.sh --rebuild` | First removes the image (`docker compose down --rmi local`) and builds it again without the build cache             |
| `./run.sh --clean`   | First deletes the WAL and the metrics file of an earlier run, for a clean start of a load test                      |

`--detach`, `--rebuild` and `--clean` can be combined. Without `--rebuild` the image is still built again when its files changed (`up --build`), using the build cache.

| Setting (environment variable)                       | Default                                                                                                                                                                   |
| :--------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `OTLP_PUSH_API_URL`                                  | `http://127.0.0.1:4318/v1/logs` (the OTLP test container in `tools/otel-sink`)                                                                                            |
| `OTLP_PUSH_API_URL_BACKUP`, `OTLP_PUSH_FAILBACK_SEC` | not set, 60. Passed on to `otelfwd`, see the [backup endpoint](../README.md#backup-endpoint). `run.sh` prints the backup at start, `run_loadtest.sh --backup` sets it     |
| `OTLP_PUSH_TOKEN`, `OTLP_CA_FILE`                    | not set                                                                                                                                                                   |
| `OTELFWD_NGINX_PORT`                                 | `18080` on `127.0.0.1` of the host. The container uses the network of the host, so this is the real port of NGINX. It has to be free on the host                          |
| `OTELFWD_BIN`                                        | `../otelfwd` (built with `make`), otherwise `otelfwd` from the `PATH`                                                                                                     |
| `OTELFWD_SOCKET_DIR`                                 | `/tmp/otelfwd-syslog`. `/mnt/...` is refused                                                                                                                              |
| `OTELFWD_DATA_DIR`                                   | `otelfwd-data` next to `run.sh` (WAL and metrics, git-ignored). It is separate from the data directory of any other `otelfwd`, because two instances must not share a WAL |

### How the socket gets into the container

`otelfwd` runs on the Linux host and creates its syslog socket `/tmp/otelfwd-syslog/syslog.sock`. Compose mounts that **directory** at `/run/otelfwd` in the container, so the socket appears at `/run/otelfwd/syslog.sock`, the path in `otelfwd-logging.conf`. Nothing is mounted below `/tmp` in the container. The normal NGINX logs (`access_test.log`, `error_test.log`) and the temporary files stay inside the container.

* **The container uses the network of the host** (`network_mode: host`). Without it every request of a load test passes Docker's NAT and proxy layer, which costs a lot of throughput. With it NGINX listens directly on `127.0.0.1:18080` (`OTELFWD_NGINX_PORT`) of the host: no `ports`, and the port has to be free on the host. `entrypoint.sh` writes the configuration from `nginx.conf.template` with that port. On Docker Desktop, host networking has to be enabled once (Settings, Resources, Network, "Enable host networking"; Docker Desktop 4.34 or later). With the Docker Engine inside WSL it works without a setting.
* The directory is mounted and not the socket file, because `otelfwd` creates the file again every time it starts. NGINX finds the new file on its next log write, so restarting `otelfwd` does not break a running container. A mounted file would keep pointing to the old socket. It also avoids the case that Docker creates a directory where the socket file is missing.
* The directory has to be on a Linux file system. Sockets cannot be created on `/mnt/c` or `/mnt/d` in WSL. Set another directory with `OTELFWD_SOCKET_DIR`.
* The directory has to belong to your user. `run.sh` creates it (mode `0700`) and refuses a directory of someone else. If Docker creates it for the mount, it belongs to root and `otelfwd` cannot create its socket there.
* The container runs with the user id of the host user (`OTELFWD_UID`, `OTELFWD_GID`). The socket belongs to that user and keeps its default mode `0600`, so nobody else can write to it.
* **Not as root:** NGINX started as root runs its worker processes as its own `nginx` user, and the workers write to the socket. They cannot open a root-owned socket (`connect() failed (13: Permission denied) while logging to syslog`). If you start `run.sh` as root, it runs `otelfwd` (with `setpriv`) and the container as the owner of the repository directory instead, gives that user the socket directory, and says so.

### Start it by hand

The same steps without `run.sh`. Create the directory as your user and start `otelfwd`:

```bash
mkdir -p /tmp/otelfwd-syslog
```

```bash
OTELFWD_SYSLOG_SOCKET=/tmp/otelfwd-syslog/syslog.sock OTLP_PUSH_API_URL=http://127.0.0.1:4318/v1/logs otelfwd -nostdin
```

Start the container:

```bash
BUILDKIT_PROGRESS=plain OTELFWD_UID=$(id -u) OTELFWD_GID=$(id -g) docker compose up --build
```

### Files of the container

| File                                   | Description                                                                          |
| :------------------------------------- | :----------------------------------------------------------------------------------- |
| `Dockerfile`                           | Alpine with NGINX from the Alpine repository, runs as a non-root user                |
| `run.sh`                               | Starts `otelfwd` and then the container                                              |
| `docker-compose.yml`                   | Builds and starts the container, mounts the socket directory                         |
| `nginx.conf.template`, `entrypoint.sh` | The NGINX configuration of the container, and the entrypoint which fills in the port |
| `index.html`                           | The start page with links to the test URLs                                           |

**Status:** the configuration files were tested with a real NGINX 1.28 (same user as `otelfwd`, mode `0600` socket) and a real `otelfwd`. The image and the compose file were run with Docker under WSL (default bridge network, published port). The switch to `network_mode: host` and the `entrypoint.sh` were tested without Docker only: the entrypoint with a stub `nginx`, the configuration with a real NGINX. If `curl http://127.0.0.1:18080/health` does not answer after the start, check host networking of Docker (see above).

## Load test

`loadtest` sends a large number of requests to the test NGINX and checks whether every one of them arrives at the OTLP receiver, and how fast:

```text
loadtest ──► NGINX ──► access log JSON ──► syslog socket ──► otelfwd ──► WAL ──► OTLP ──► otel-sink
   │                                                                                          │
   └──────────── POST /test/reset, GET /test/stats ───────────────────────────────────────────┘
```

NGINX creates events much faster and more predictably than a Domino server. The sink (`tools/otel-sink`) keeps a ledger of every event in memory. Test records are not written to files there.

```bash
./run_loadtest.sh                                   # everything: build, start, test, stop
./run_loadtest.sh --threads 32 --requests 100000    # 3.2 million events
```

`run_loadtest.sh` does these steps: it builds `otelfwd` and `loadtest` (`make`), starts the sink container (`tools/otel-sink/run.sh --detach`), starts `otelfwd` and the NGINX container with a clean WAL (`run.sh --detach --clean`), runs `loadtest`, and stops what it started (`docker compose down`).
The build needs the rapidjson headers.

**Size of the test:** the default is 8 threads with 10,000 requests each, 80,000 events. Started in a terminal without `--threads` and `--requests`, `run_loadtest.sh` proposes these values and asks for others before it starts anything. Enter takes the value in brackets. Give the options, or `--yes`, or run it without a terminal (a script, CI) to skip the questions. The exit code is the one of `loadtest`.

### Options of run_loadtest.sh

| Option of `run_loadtest.sh` | Meaning                                                                                 |
| :-------------------------- | :-------------------------------------------------------------------------------------- |
| `--keep`                    | Leave everything running afterwards                                                     |
| `--rebuild`                 | Remove the images and build them again without the build cache                          |
| `--help`, `-h`              | List the options of this script and of `loadtest`, and end. Nothing is built or started |
| `--yes`                     | Do not ask for the size of the test                                                     |
| `--backup`                  | Failover test, see [Failover and refused data](#failover-and-refused-data)              |
| `--reject`                  | Test of refused data, see [Failover and refused data](#failover-and-refused-data)       |
| all others                  | Go to `loadtest`, see [Options of `loadtest`](#options-of-loadtest)                     |

**A sink which is already running** (port 4318, for example one started from another directory) is looked at first: if it answers `/test/stats` like the current sink, it is used as it is and left running. An older sink without the load test ledger is refused with a message, because it cannot count the events. Stop it and run the script again, it then builds the current image. `OTEL_SINK_PORT` selects another port for the sink. The push URL of `otelfwd` follows it unless `OTLP_PUSH_API_URL` is set.

To build and run `loadtest` alone (needs the rapidjson headers):

```bash
make loadtest
./loadtest --help
```

### What is sent

Every request is `GET /otelfwd-test/<thread>/<count>/<sent_ns>`. NGINX answers it with `404` without looking for a file (`location /otelfwd-test/` in `nginx.conf.template`), so there is no error log line per request. It logs it, the JSON line has the path as `url.path`, and the sink reads the three numbers back from it:

| Part      | Meaning                                                          |
| :-------- | :--------------------------------------------------------------- |
| `thread`  | Number of the sender thread, from 1                              |
| `count`   | Sequence number inside that thread, from 1                       |
| `sent_ns` | Unix time in nanoseconds, taken right before the request is sent |

`{thread, count}` identifies an event. Each thread has its own keep-alive connection and one request in flight. A request is **never sent twice**: one without an answer counts as failed, and its outcome is unknown. Otherwise a repeated request would look like a duplicate made by the forwarder.

### Options of `loadtest`

| Option                                 | Default                  | Meaning                                                                                                                                                                    |
| :------------------------------------- | :----------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `--threads N`                          | 8                        | Sender threads                                                                                                                                                             |
| `--requests N`                         | 10000                    | Requests per thread                                                                                                                                                        |
| `--rate N`                             | 0 (as fast as possible)  | Requests per second and thread. Bursts and a steady load fail differently                                                                                                  |
| `--wait SEC`                           | 60                       | How long to wait for the last events after the last request                                                                                                                |
| `--stall SEC`                          | 15                       | Stop waiting when nothing new arrived for this long                                                                                                                        |
| `--timeout SEC`                        | 10                       | Timeout of one HTTP request                                                                                                                                                |
| `--fail-for SEC`                       | 0                        | WAL test: the sink answers `503` for this long, then recovers                                                                                                              |
| `--otelfwd-prom FILE`                  | set by `run_loadtest.sh` | Metrics file of `otelfwd`. Shows where events were lost. `otelfwd` writes it every 10 seconds                                                                              |
| `--nginx URL` `--sink URL` `--token T` | ports of the containers  | Where the test NGINX and the sink are                                                                                                                                      |
| `--expect-failover`                    |                          | The primary endpoint of `otelfwd` fails and there is a backup. Passes only if every event arrived through the backup and `otelfwd` made a failover. Needs `--otelfwd-prom` |
| `--expect-reject`                      |                          | The primary endpoint refuses the data with 400 and there is a backup. Passes only if nothing arrived. Needs `--otelfwd-prom`                                               |
| `--verbose`                            |                          | Lists every thread                                                                                                                                                         |

### Failover and refused data

`otelfwd` can have a backup endpoint (`OTLP_PUSH_API_URL_BACKUP`, see the [main README](../README.md#backup-endpoint)). The test sink has a port which always fails (4320), so two options of `run_loadtest.sh` test the two endpoints without any other tool. They start `otelfwd` with the failing port as the primary endpoint and the normal port (4318) as the backup.

| Option     | The primary answers                 | What has to happen                                                                                    |
| :--------- | :---------------------------------- | :---------------------------------------------------------------------------------------------------- |
| `--backup` | `503` (the default of the sink)     | `otelfwd` makes a failover. **Every** event arrives at the sink through the backup, exactly once      |
| `--reject` | `400` (`OTEL_SINK_FAIL_STATUS=400`) | **Nothing** arrives. The data is dropped: not kept in the WAL for a retry, and not sent to the backup |

```bash
./run_loadtest.sh --backup --yes --threads 4 --requests 2000
```

```bash
./run_loadtest.sh --reject --yes --threads 4 --requests 2000
```

* **What the report shows:** the block `otelfwd endpoints` lists the requests to the primary and to the backup, the failovers and the endpoint in use at the end. For `--backup` the result is `PASS` with the note that the events came through the backup. For `--reject` it is `PASS` with the note that the data was refused and dropped.
* **What makes it fail:** for `--backup`, missing events, no failover, or no request accepted by the backup. For `--reject`, events which arrived at the sink, data which was kept in the WAL for a retry, data which was sent to the backup, or no refusal counted at all (`otelfwd_push_rejected_total`).
* **The status of the failing port** is set when the sink starts (`OTEL_SINK_FAIL_STATUS`, 503 by default, and `--reject` sets 400). A sink which is already running keeps its setting, so the script asks the port before it starts anything. If the status does not fit, it stops with a message and tells to stop the sink. Nothing else is touched.
* **Failback** (going back to the primary) is not part of these tests, because the failing port never recovers. It is covered by the [unit test of otelfwd](../README.md#unit-test-of-otelfwd). To see it in a load test the sink would need a switch which makes one port work again.
* Both options work with `--fail-for`. The failing time then applies to both ports, so both endpoints fail for that time, and the events go to the WAL.
* These two options were tested with a fake environment for the verdict, not with the containers. Please run each once and tell if the containers behave differently.

### The result

The report has three parts: what the generator sent, what the sink received, and what `otelfwd` saw (the change of its counters during the test).

| Value                    | Meaning                                                                                                                                                                                                                     |
| :----------------------- | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `missing`                | Expected events which never arrived                                                                                                                                                                                         |
| `duplicates`             | Deliveries beyond the first one, for example from a WAL replay. Allowed (at least once)                                                                                                                                     |
| `unexpected`             | Events outside the expected range. Records of an earlier run in the WAL show up here or as conflicts                                                                                                                        |
| `malformed`              | Test paths which cannot be read                                                                                                                                                                                             |
| `conflicts`              | The same `{thread, count}` with a different send time                                                                                                                                                                       |
| end to end latency       | Arrival in the sink minus `sent_ns`: min, average, p50, p95, p99 (from a histogram, about 10 % exact), max                                                                                                                  |
| the two stages           | Generator to the NGINX log time, and NGINX log time to the sink. Exact to about half a millisecond, because NGINX logs whole milliseconds                                                                                   |
| forwarding rate          | Events per second from the first to the last arrival                                                                                                                                                                        |
| other records            | Log records in the sink which are not test events, with samples. NGINX writes its own problems to the same socket, for example `send() to syslog failed while logging request`. Such a line takes the place of a lost event |
| named as failed by NGINX | Events which such a line names, and how many of the missing events are among them. If both numbers are equal the report says PROVEN: all loss happened between NGINX and `otelfwd`                                          |

**PASS** needs: nothing missing, nothing unexpected or malformed, no conflicting send times, and every request sent and answered with `404`. Duplicates are reported but allowed. With `--fail-for` the test passes when everything arrived after the sink recovered, even if some events arrive twice.

When events are missing, the `otelfwd` part says where:

| What `otelfwd` reports                                                        | Meaning                                                                                                                                                    |
| :---------------------------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------- |
| (accepted minus other records) + dropped + invalid is less than what was sent | Lost before `otelfwd`: NGINX could not write to the syslog socket, or its buffer was full. Syslog over a datagram socket has no backpressure. Try `--rate` |
| `dropped` above 0                                                             | `otelfwd`'s queue was full (`OTELFWD_SOCKET_QUEUE_MAX`): pushing was slower than receiving                                                                 |
| everything accepted, still missing                                            | Lost after `otelfwd`: push errors, its WAL, or the sink                                                                                                    |

Both of the first two can apply in one test. The report then lists each cause with its own number. Lines of NGINX itself which are not test events (the "other records") are subtracted from "accepted", because they take the place of lost events.

### Limits

* **Two places lose events under a high rate**, and the report tells which:
  * **Between NGINX and `otelfwd`:** syslog over a UNIX datagram socket has no backpressure. At most `net.unix.max_dgram_qlen` (512) datagrams wait, and the send buffer of NGINX (about 200 KB) holds only roughly 170 to 400 log lines. When it is full NGINX drops the line and writes `[warn] send() to syslog failed while logging request, ... request: "GET /otelfwd-test/23/14559/..."` to its error log. That line arrives as an "other record" in the sink, in place of the lost event, and is shown in the report. Because it names the request, the sink can match it to the missing event: the report says PROVEN when every missing event is named by such a line. At about 20,000 events/s a stall of `otelfwd` for 25 milliseconds is enough. A receive buffer setting of `otelfwd` does not help (measured).
  * **Inside `otelfwd`:** its input queue (`OTELFWD_SOCKET_QUEUE_MAX`, 100,000 records) between the input threads and the single push thread. The push thread sends at most 100 records per request, one request at a time, so it can push about `100 / round trip time` records per second (about 13,000 with a Docker sink). A higher input rate fills the queue, and `otelfwd` drops (counted as `dropped`).
* The latency numbers need the same clock for the generator and the sink. That is true on one host, also with Docker Desktop on WSL.
* The WAL retry of `otelfwd` waits 60 seconds after a failed push (`OTLP_PUSH_WAL_RETRY_SEC`), and nothing arrives at the sink in that time. A `--fail-for` test needs a long `--wait` **and** a long `--stall`, because the test also stops when nothing new arrived for `--stall` seconds (default 15): `--fail-for 10 --wait 420 --stall 200`. Without `--stall` it gives up after 15 seconds and reports all events as missing, although they are still in the WAL.
* The ledger takes about 40 bytes per event. The sink accepts up to 20 million events per test and 4096 threads.
* Start each test clean: `run_loadtest.sh` deletes the WAL. Records of an earlier run in the WAL would count as `unexpected` or `conflicts`.

## Access log fields

| JSON key                       | NGINX variable            | Comment                                                                                     |
| :----------------------------- | :------------------------ | :------------------------------------------------------------------------------------------ |
| `time`                         | `$msec`                   | quoted `seconds.millis`, exact event time                                                   |
| `server.address`               | `$host`                   |                                                                                             |
| `client.address`               | `$remote_addr`            |                                                                                             |
| `http.request.method`          | `$request_method`         |                                                                                             |
| `url.scheme`                   | `$scheme`                 | `http` or `https`                                                                           |
| `url.path`                     | `$uri`                    | path only, no query string (may contain tokens)                                             |
| `http.response.status_code`    | `$status`                 | number                                                                                      |
| `http.request.size`            | `$request_length`         | number, request line, headers and body                                                      |
| `http.response.body.size`      | `$body_bytes_sent`        | number                                                                                      |
| `user_agent.original`          | `$http_user_agent`        |                                                                                             |
| `nginx.request_time`           | `$request_time`           | number, seconds                                                                             |
| `nginx.upstream.response_time` | `$upstream_response_time` | quoted. Empty without an upstream (the forwarder skips empty strings), can be `-` or a list |
| `nginx.request_id`             | `$request_id`             | for correlation                                                                             |
