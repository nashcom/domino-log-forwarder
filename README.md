# domino-log-forwarder - HCL Domino logs and events for OpenTelemetry

Forwards the logs and events of an [HCL Domino](https://www.hcl-software.com/domino) server to any [OpenTelemetry](https://opentelemetry.io/) compatible endpoint (OTLP/HTTP with JSON encoding), for example the [Grafana Loki OTLP endpoint](https://grafana.com/docs/loki/latest/send-data/otel/) or an OpenTelemetry Collector.

The repository has two programs which work together, and some tools to test them:

| Component                                       | What it is                                                                                                                              |
| :---------------------------------------------- | :-------------------------------------------------------------------------------------------------------------------------------------- |
| [`otelfwd`](#otelfwd---the-otel-forwarder)      | The forwarder. Reads the **Domino console log** from STDIN (`server \| otelfwd`) and structured records from local sockets, pushes OTLP |
| [`domfwd`](domfwd/README.md)                    | Domino server add-in. Reads the **Domino events** (Event Monitoring queue) and sends one structured record per event to `otelfwd`       |
| [`nginx/`](nginx/README.md)                     | Test NGINX container which logs via syslog into `otelfwd`, and the end to end load test                                                 |
| [`tools/otel-sink/`](tools/otel-sink/README.md) | Test OTLP receiver container                                                                                                            |

`otelfwd` is general purpose. Everything which can write a line of text or a JSON record to a local socket can use it, for example NGINX. The Domino specific parts are the console log annotation and `domfwd`.

> **Version 2.0 only supports OTLP.**
> The Loki push API and the Alloy output format of version 1.x have been removed.
> Version 1.0.3 is the last version with the Loki push API. See [Migration from version 1.x](#migration-from-version-1x).

## Overview

```text
Domino console (STDOUT) ─── STDIN ───────────┐
                                             │
domfwd (Domino events) ──── UNIX Socket ─────┤
                                             │
Other local producer ────── TCP 127.0.0.1 ───┤
                                             │
NGINX (syslog) ──────────── UNIX Datagram ───┤
                                             │
                                             ▼
                                          otelfwd
                                             │
                                             ├── Batching
                                             ├── WAL / retry
                                             ├── Resource/scope grouping
                                             └── OTLP/HTTP JSON
                                                     │
                                                     │  HTTP(S) over TCP, outbound
                                                     │  configurable URL, OTLP default port 4318
                                                     │
                                                     ▼
                                           Any OTLP Receiver
                                        (tested with Grafana Loki)
```

Network interfaces of `otelfwd`:

| Direction | Interface                                           | Notes                                                                                                                                                                                                                           |
| :-------- | :-------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| In        | STDIN, UNIX socket, syslog datagram socket          | Local only, no network                                                                                                                                                                                                          |
| In        | TCP on loopback (`OTELFWD_TCP_LISTEN`)              | Loopback only. Off unless configured. No default port, the examples use `127.0.0.1:4390`                                                                                                                                        |
| Out       | HTTP or HTTPS to the receiver (`OTLP_PUSH_API_URL`) | The only network connection. No default: the URL sets host and port (OTLP/HTTP standard: 4318, Loki: 3100). Use `https://` with `OTLP_CA_FILE` for a private CA and `OTLP_PUSH_TOKEN` for a bearer token. Needs outbound access |

Inputs:

* **STDIN:** the console log of a Domino server (`server | otelfwd`), annotated with the server task from `pid.nbf`. See [Domino console log (STDOUT)](#domino-console-log-stdout).
* **UNIX socket** and **TCP on 127.0.0.1:** structured records in the [flat record format](#records-received-via-socket-inputs) from other local programs, above all the Domino event add-in [`domfwd`](domfwd/README.md). TCP is loopback only. See [Domino events (domfwd)](#domino-events-domfwd).
* **Syslog** (UNIX datagram socket, only if enabled): syslog messages, for example the access and error log of NGINX. See [Syslog input](#syslog-input).

What `otelfwd` does with them:

* **Batching:** lines which are queued at the same time are pushed in one request (up to 100 lines / 512 KB). It never waits to fill a batch.
* **WAL / retry:** a request which fails is written to a write ahead log and replayed when the receiver is back.
* **Resource/scope grouping:** records with the same resource and scope share one `resourceLogs` entry.
* **OTLP/HTTP JSON:** the requests go to the OTLP logs endpoint of any receiver, for example an OpenTelemetry Collector or Grafana Loki.
* **Metrics:** writes a metrics file for Prometheus.

## Domino console log (STDOUT)

The console output of the Domino server is the first use case. The server writes its console log to STDOUT. Piping it into `otelfwd` sends every line to the OTLP endpoint, and the log stays where it was:

```bash
export OTLP_PUSH_API_URL=https://otel.example.com:4318/v1/logs
export OTELFWD_OUTPUT_LOG=/local/notesdata/notes.log     # optional, see below

/opt/hcl/domino/bin/server | otelfwd
```

* **One line, one record.** The line is the body, unchanged. Console lines carry no severity and no source time stamp, so the severity is unspecified and the time the line was read is used.
* **Domino task name.** The process id in the line prefix (`[86261:000002-...]`) is looked up in `pid.nbf` and sent as attribute `domino.task` (for example `http`, `amgr`, `router`). The PID is sent as `process.pid`. Grafana Loki and similar backends can then filter by task. The complete list of attributes is in [Lines read from STDIN](#lines-read-from-stdin).
* **Output log.** `OTELFWD_OUTPUT_LOG` writes the unmodified input to a log file, so there is no shell redirection and the log handling stays in the forwarder. `OTELFWD_MIRROR_STDOUT=1` writes it to STDOUT as well, for example for `docker logs`. Without `OTELFWD_OUTPUT_LOG` no log file is written. See [Output related configuration](#output-related-configuration).
* **Nothing is lost when the receiver is down.** A failed push goes to the [WAL](#durable-log-delivery) and is replayed later. Lines read from STDIN are never dropped.
* **The forwarder ends with the server.** It ends when STDIN is closed. Records which could not be pushed stay in the WAL for the next start. See [Pipe mode](#pipe-mode).

Domino events (not the console text) are a different source, see the next section.

## Domino events (domfwd)

[`domfwd`](domfwd/README.md) is a Domino server add-in (server task) in this repository. It reads the events of the Domino event queue, the same source Domino Event Monitoring uses, and turns every event into one structured record: severity, event type, error code and text, add-in name, target database and more, with the process of the server as resource. This is information which is not in the console text.

```text
Domino server ── events ──► domfwd ── UNIX socket ──► otelfwd ──► OTLP receiver
```

* `domfwd` sends to the default UNIX socket of `otelfwd` (`<data>/domino/otelfwd.sock`) on Linux without any setting, so `load domfwd` next to a running `otelfwd` is enough.
* The receiving `otelfwd` can run as a pipe (`server | otelfwd`, which also forwards the console log) or standalone (`otelfwd -nostdin`, see [Operating modes](#operating-modes)).
* The event handling of the server has to post events to the event queue `domfwd`. Configuration, notes.ini settings, metrics, build instructions (needs the Domino C API toolkit) and status are in [domfwd/README.md](domfwd/README.md).
* The record format `domfwd` sends is the [flat record format](#records-received-via-socket-inputs).

## otelfwd - the OTel forwarder

`otelfwd` takes log data from STDIN and from optional local socket inputs and pushes it as OpenTelemetry logs (OTLP/HTTP with JSON encoding). The rest of this document is its reference.

Features:

* Pushes logs via OTLP/HTTP (JSON) to any OpenTelemetry compatible endpoint
* Annotates Domino console lines using `pid.nbf` with the Domino server task name
* Supports durable WAL based retry for push operations
* Sends lines that are queued at the same time in one push request (up to 100 lines / 512 KB, never waits to fill a batch)
* Optional socket inputs (UNIX socket, loopback TCP and a syslog datagram socket) to receive structured log records, for example from the Domino event add-in or NGINX
* Can run standalone without reading STDIN (`-nostdin`), serving only its socket inputs
* Writes the unmodified input to STDOUT or a defined log file
* Writes a metrics file for Prometheus

## Operating modes

`otelfwd` is designed as a stream processor and log forwarder.
It is not a process supervisor or init replacement.

| Mode                    | Started with                                                                                                                                     | Input             | Ends when                          |
| :---------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------- | :---------------- | :--------------------------------- |
| Pipe (default)          | `server \| otelfwd`                                                                                                                              | STDIN             | STDIN is closed (the server stops) |
| Pipe with socket inputs | `server \| otelfwd` and `OTELFWD_UNIX_SOCKET` and/or `OTELFWD_TCP_LISTEN` (default: [UNIX socket](#socket-inputs) if `OTLP_PUSH_API_URL` is set) | STDIN and sockets | STDIN is closed                    |
| Standalone              | `otelfwd -nostdin` (and `OTLP_PUSH_API_URL`)                                                                                                     | sockets only      | `SIGTERM` or `SIGINT`              |

### Pipe mode

```bash
/opt/hcl/domino/bin/server | otelfwd
```

The log lines of the server are read from STDIN. The forwarder ends when STDIN is closed.
If `OTLP_PUSH_API_URL` is set, a UNIX socket input is also opened by default (see [Socket inputs](#socket-inputs)). Socket inputs are served as long as the forwarder runs.

### Standalone mode

With `-nostdin` the forwarder does not read STDIN at all. It only serves its [socket inputs](#socket-inputs) and runs until it receives `SIGTERM` or `SIGINT`.
This makes it usable as a separate service, for example to receive records from other programs.

```bash
export OTELFWD_DATA_DIR=/var/lib/otelfwd
export OTLP_PUSH_API_URL=https://otel.example.com:4318/v1/logs
export OTELFWD_UNIX_SOCKET=/run/otelfwd/otelfwd.sock    # optional, default: <data>/domino/otelfwd.sock

otelfwd -nostdin
```

* `OTLP_PUSH_API_URL` has to be set, and at least one socket input has to work. Without a configured socket input the default UNIX socket is used. Otherwise the forwarder logs an error and ends with exit code 1.
* `OTELFWD_DATA_DIR` is the directory for the WAL and for the metrics file. It has to be writable. See [Additional configuration](#additional-configuration).
* `OTELFWD_MIRROR_STDOUT` and `OTELFWD_OUTPUT_LOG` only apply to lines read from STDIN. They are ignored with `-nostdin` and a warning is logged.
* The WAL is replayed while the forwarder is running. Records received before a shutdown are pushed first, and records which could not be pushed stay in the WAL for the next start.
* Only records received via the socket inputs are pushed. They carry their own attributes. The annotation with the Domino server task name via `pid.nbf` only applies to lines read from STDIN.

## Command line

| Parameter  | Description                                                                     |
| :--------- | :------------------------------------------------------------------------------ |
| `-nostdin` | Do not read STDIN. Only the socket inputs are used, until `SIGTERM` or `SIGINT` |
| `-cfg`     | Print the configuration and exit                                                |
| `-env`     | Print the configuration as environment variables and exit                       |
| `-help`    | Print the help and exit                                                         |
| `-version` | Print the version and exit                                                      |

`-cfg` shows the configuration as parsed so far. Put `-nostdin` before `-cfg` to see it in the output.

## Environment Variables

By default the input is written to the output log file specified via `OTELFWD_OUTPUT_LOG`.
In addition to the standard log file, the input can be mirrored to STDOUT.
Letting `otelfwd` write the output log avoids shell based output redirection and keeps log handling centralized in the forwarder.

If no output log file is specified, no output log is written.

### Output related configuration

| Variable Name           | Description            | Example / Comments           |
| :---------------------- | :--------------------- | :--------------------------- |
| `OTELFWD_OUTPUT_LOG`    | Output log file name   | `/local/notesdata/notes.log` |
| `OTELFWD_MIRROR_STDOUT` | Mirror stdin to stdout | `1`                          |

### Additional configuration

Most of the following parameters are optional.

| Variable Name              | Description                                                                       | Example / Comments                               |
| :------------------------- | :-------------------------------------------------------------------------------- | :----------------------------------------------- |
| `OTELFWD_DATA_DIR`         | Data directory. Has to be writable. Used for the WAL and the default metrics file | default: `/local/notesdata`                      |
| `OTELFWD_LOGLEVEL`         | Log level for stdout logging                                                      | `1`                                              |
| `OTELFWD_HOSTNAME`         | Hostname to use                                                                   | default: hostname read from OS                   |
| `OTELFWD_PROM_FILE`        | Prom File for Metrics output                                                      | default: `<notesdata>/domino/stats/otelfwd.prom` |
| `OTELFWD_SHUTDOWN_MAX_SEC` | Shutdown max wait seconds                                                         | default: `30`                                    |

### OTLP push configuration

OTLP push is optional and only active when `OTLP_PUSH_API_URL` is set.
The URL is the complete logs endpoint of the receiver. For the OpenTelemetry Collector this is `/v1/logs`, for Grafana Loki it is `/otlp/v1/logs`.

| Variable Name              | Description                       | Example / Comments                      |
| :------------------------- | :-------------------------------- | :-------------------------------------- |
| `OTLP_PUSH_API_URL`        | OTLP/HTTP logs endpoint           | `https://otel.example.com:4318/v1/logs` |
| `OTLP_PUSH_TOKEN`          | Bearer token for the endpoint     | `my-secure-token`                       |
| `OTLP_CA_FILE`             | Trusted Root CA File              | `/local/notesdata/trusted_root.pem`     |
| `OTLP_SERVICE_NAME`        | `service.name` resource attribute | default: `domino`                       |
| `OTLP_SERVICE_NAMESPACE`   | `service.namespace`               | default: `domino`                       |
| `OTLP_SERVICE_INSTANCE_ID` | `service.instance.id`             | default: hostname                       |

### Socket inputs

Socket inputs receive log records from other programs, for example the Domino event forwarder.
Each input runs in its own thread and needs `OTLP_PUSH_API_URL`, because the received records are pushed via OTLP.
An input that cannot be started is logged and skipped. Reading from STDIN continues.

| Variable Name              | Description                                                                                                                                 | Example / Comments               |
| :------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------ | :------------------------------- |
| `OTELFWD_UNIX_SOCKET`      | UNIX socket to receive records. Default if `OTLP_PUSH_API_URL` is set and no other socket input is configured: `<data>/domino/otelfwd.sock` | `/run/otelfwd/otelfwd.sock`      |
| `OTELFWD_UNIX_SOCKET_MODE` | File mode of the UNIX socket (octal)                                                                                                        | default: `0600` (only the owner) |
| `OTELFWD_TCP_LISTEN`       | Loopback TCP address to receive records                                                                                                     | `127.0.0.1:4390` or `[::1]:4390` |
| `OTELFWD_SOCKET_QUEUE_MAX` | Max queued records before records are dropped                                                                                               | default: `100000`                |

* **Default socket:** if `OTLP_PUSH_API_URL` is set and none of `OTELFWD_UNIX_SOCKET`, `OTELFWD_TCP_LISTEN` and `OTELFWD_SYSLOG_SOCKET` is configured, the forwarder listens on `<OTELFWD_DATA_DIR>/domino/otelfwd.sock` and creates the directory if needed. This is where the Domino add-in [`domfwd`](domfwd/README.md) sends by default, so both sides need no setting. `-cfg` only shows explicitly configured sockets. Setting any socket input turns the default off.
* The socket is created with the mode `OTELFWD_UNIX_SOCKET_MODE` (default `0600`). Only the user who started the forwarder can connect. Start `otelfwd` as the same user as the Domino server.
* The TCP input has no authentication and no encryption. It only accepts loopback addresses (`127.0.0.0/8` or `::1`) and refuses to start on any other address.
* A stale UNIX socket file of an earlier run is replaced. A socket which is in use by another process, or a file which is not a socket, is never removed.
* At most 32 connections per input are served at the same time. More connections are closed immediately.
* One record is one line. Lines above 1 MB are discarded.
* When the queue is full, records received via sockets are dropped and counted. Lines read from STDIN are never dropped.

Example using `socat`:

```bash
echo '{"body":"Hello from a socket","severity_number":9,"severity_text":"INFO"}' | socat - UNIX-CONNECT:/run/otelfwd/otelfwd.sock
```

### Syslog input

A UNIX **datagram** socket which receives syslog messages, for example from NGINX (`access_log syslog:server=unix:...`). It is only enabled if requested. It runs in its own thread and needs `OTLP_PUSH_API_URL`.

| Variable Name                | Description                                                           | Example / Comments               |
| :--------------------------- | :-------------------------------------------------------------------- | :------------------------------- |
| `OTELFWD_SYSLOG_SOCKET`      | Path of the datagram socket. The syslog input is enabled if it is set | `/run/otelfwd/syslog.sock`       |
| `OTELFWD_SYSLOG_SOCKET_MODE` | File mode of the socket (octal)                                       | default: `0600` (only the owner) |

One datagram is one message, in the format NGINX sends: `<PRI>Mmm dd hh:mm:ss [hostname ]tag: message`.

| Part of the message                             | Becomes                                                                                                                                                                                                                                                                                                                                                                                                                                           |
| :---------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ |
| `PRI` (severity)                                | Severity of the record: emerg, alert, crit = FATAL, err = ERROR, warning = WARN, notice, info = INFO, debug = DEBUG. The facility is ignored                                                                                                                                                                                                                                                                                                      |
| `tag`                                           | Scope name of the record. `tag[pid]:` is accepted, the process id is dropped                                                                                                                                                                                                                                                                                                                                                                      |
| `message`                                       | Body of the record, **unchanged**                                                                                                                                                                                                                                                                                                                                                                                                                 |
| timestamp, hostname                             | Not used. The timestamp has no year, no time zone and only seconds. The event time is the arrival time                                                                                                                                                                                                                                                                                                                                            |
| resource `service.name` and `service.namespace` | The part of the tag before the first underscore, for both: `nginx_access` and `nginx_error` are `nginx`, `apache_access` is `apache`. A tag without an underscore is used as it is. The complete tag stays the scope name. Without a tag the default resource is used. `service.instance.id`, `host.name` and `os.type` are the defaults of the forwarder. The producer chooses its identity through the tag, `otelfwd` knows nothing about NGINX |

If the message is a **JSON object**, its members become attributes of the record with their JSON types (string, integer, floating point, boolean). Members with an empty string as value are skipped. The member `time` is the event time, in seconds with a fraction as string or number, for example `"1789769289.831"` (the `$msec` variable of NGINX). It is not an attribute. The body is still the original message. If the message is not valid JSON, or `time` cannot be used, the record is delivered as it is with the arrival time.

The receiver does not know NGINX. Everything is decided by the tag and the message. Messages without a valid `<PRI>`, empty messages and datagrams above 64 KB are counted as `invalid` and not delivered. A datagram is refused to the sender when too many are already waiting, and NGINX then drops the line and logs `send() to syslog failed while logging request` (a `[warn]` line, with the request) in its error log. `otelfwd` cannot count these. For UNIX datagram sockets the number of waiting datagrams is limited by `net.unix.max_dgram_qlen` (512 by default) and by the send buffer of the sender, not by a receive buffer of `otelfwd`. The [load test](nginx/README.md#load-test) shows where events get lost.

**Permissions:** to send a datagram, the sender needs write permission on the socket file and search permission on its directory. With the default mode `0600` only the user of `otelfwd` can send, so the NGINX worker processes cannot. Set `OTELFWD_SYSLOG_SOCKET_MODE` (for example `0620` and a group both users share). Everybody who can write to the socket can inject log records.
The directory of the socket has to exist. It is not created.

**NGINX** with the JSON log format and the error log, see [nginx/README.md](nginx/README.md), which also has a test container:

```nginx
access_log syslog:server=unix:/run/otelfwd/syslog.sock,tag=nginx_access otelfwd_json;
error_log  syslog:server=unix:/run/otelfwd/syslog.sock,tag=nginx_error;
```

The access log should use a JSON `log_format` with `escape=json` and OpenTelemetry attribute names as keys. The `combined` format also works, but it arrives as plain text in the body without attributes.

## Durable Log Delivery

`otelfwd` uses a local Write Ahead Log (WAL) to ensure logs are not lost during temporary network or server outages.

Failed log pushes are written to the WAL and replayed automatically once connectivity is restored.
One WAL record is one push request, which can contain multiple log lines.
The WAL file is `otelfwd.wal` in the data directory (`OTELFWD_DATA_DIR`).

## Output in OTLP/HTTP JSON format

One request contains one `resourceLogs` entry per resource and scope, and one log record per log line in arrival order.

### Lines read from STDIN

Log lines carry no severity and no source timestamp, so the severity is left unspecified and the time the line was read is used as time stamp.

| Level      | Attribute             | Value                                                            |
| :--------- | :-------------------- | :--------------------------------------------------------------- |
| Resource   | `service.name`        | `OTLP_SERVICE_NAME` (default: `domino`)                          |
| Resource   | `service.namespace`   | `OTLP_SERVICE_NAMESPACE` (default: `domino`)                     |
| Resource   | `service.instance.id` | `OTLP_SERVICE_INSTANCE_ID` (default: hostname)                   |
| Resource   | `host.name`           | hostname                                                         |
| Resource   | `os.type`             | `linux`                                                          |
| Scope      | name / version        | `otelfwd` / version of the forwarder                             |
| Log record | `process.pid`         | PID from the log line prefix (not set if the line has no prefix) |
| Log record | `domino.task`         | Domino server task name from `pid.nbf` (`unknown` if not found)  |

Some backends, like Grafana Loki, turn a default set of resource attributes (for example `service.name`, `service.namespace`, `service.instance.id` and `host.name`) into index labels and store all other attributes as structured metadata.
The PID is therefore not an index label.
The set of attributes is preliminary and might change.

```json
{"resourceLogs":[{"resource":{"attributes":[{"key":"service.name","value":{"stringValue":"domino"}}]},"scopeLogs":[{"scope":{"name":"otelfwd","version":"2.0.0"},"logRecords":[{"timeUnixNano":"1770749014360459684","observedTimeUnixNano":"1770749014360459684","body":{"stringValue":"[86261:000002-00007E61FEDAE2C0] 02/01/2026 00:48:03   HTTP Server: Shutdown"},"attributes":[{"key":"process.pid","value":{"intValue":"86261"}},{"key":"domino.task","value":{"stringValue":"http"}}]}]}]}]}
```

The example shows only one of the resource attributes to keep it short.

Invalid UTF-8 sequences in a log line are replaced by the Unicode replacement character before the line is sent.

### Records received via socket inputs

Socket inputs receive one JSON object per line in a flat record format, which follows the OpenTelemetry log data model.
Everything in a record is passed on as it is: time stamps, severity, resource, scope, attributes and body.
All fields are optional.

| Field                     | Description                                                                                     |
| :------------------------ | :---------------------------------------------------------------------------------------------- |
| `time_unix_nano`          | Event time in nanoseconds since the epoch, as decimal string or integer. Default: observed time |
| `observed_time_unix_nano` | Time the record was observed. Default: time the forwarder received the record                   |
| `severity_number`         | 1 to 24. Values outside of the range are ignored                                                |
| `severity_text`           | Severity text                                                                                   |
| `body`                    | The log message. An object or array is converted to its JSON text                               |
| `scope`                   | Object with `name` and optionally `version`. Default: the forwarder's scope                     |
| `resource`                | Flat map of resource attributes. Default: the forwarder's resource (see above)                  |
| `attributes`              | Flat map of log record attributes                                                               |

Attribute values: a string is sent as `stringValue`, an integer as `intValue`, a floating point number as `doubleValue` and a boolean as `boolValue`.
An object or array is converted to its JSON text and sent as `stringValue`. `null` values are skipped.
Unknown top level fields are ignored.
Records with the same resource and scope are combined in one `resourceLogs` entry.

Example: a Domino event.

```json
{
  "time_unix_nano": "1785760554890000000",
  "observed_time_unix_nano": "1785760556110000000",
  "severity_number": 17,
  "severity_text": "ERROR",
  "body": "Database compactor error: File does not exist",
  "scope": {
    "name": "domino.event"
  },
  "resource": {
    "service.name": "domino",
    "service.instance.id": "CN=makemake/O=NotesLab",
    "host.name": "nsh-t14.nashcom.loc",
    "os.type": "windows",
    "service.version": "14.5.1FP1",
    "process.executable.name": "ncompact",
    "process.pid": 33708,
    "domino.process.pidtid": "83AC:0002-796C"
  },
  "attributes": {
    "domino.event.type_code": 5,
    "domino.event.type": "Resource",
    "domino.event.time": "2026-08-03T12:35:54.89Z",
    "domino.error.code": 259,
    "domino.error.text": "Database compactor error: File does not exist",
    "domino.error.additional_code": 259,
    "domino.addin.name": "Database Compactor",
    "domino.target.server": "CN=makemake/O=NotesLab",
    "domino.target.database": "xyz.nsf",
    "domino.target.user": ""
  }
}
```

The record must be sent as a single line.

## Examples

The folder `examples` contains sample data and two scripts which push it with `otelfwd`. They need bash and a built `otelfwd` (`make`).
By default they push to the [test container](#testing) at `http://127.0.0.1:4318/v1/logs`.

```bash
./examples/push-stdin.sh
```

Pipe mode: pushes `examples/sample-console.log` (Domino console lines) from STDIN. The same without the script:

```bash
cat examples/sample-console.log | OTLP_PUSH_API_URL=http://127.0.0.1:4318/v1/logs ./otelfwd
```

`./otelfwd -cfg` only prints the configuration and ends. It does not read STDIN, so it is useful to check what a set of environment variables results in, not to push:

```bash
OTLP_PUSH_API_URL=http://127.0.0.1:4318/v1/logs ./otelfwd -cfg
```

```bash
./examples/push-socket.sh
```

Standalone mode: starts `otelfwd -nostdin` with a TCP input on `127.0.0.1:4390`, sends `examples/sample-records.jsonl` (the Domino event, an NGINX style record and two plain records) and stops it. `./examples/push-socket.sh unix` uses a Unix socket (needs `socat` or `nc`).
The same by hand, in two terminals:

```bash
OTLP_PUSH_API_URL=http://127.0.0.1:4318/v1/logs OTELFWD_TCP_LISTEN=127.0.0.1:4390 ./otelfwd -nostdin
```

```bash
cat examples/sample-records.jsonl > /dev/tcp/127.0.0.1/4390
```

Both scripts print how many lines were pushed and return an error if the push failed. Settings (environment variables):

| Variable            | Description                                                                    | Default                                   |
| :------------------ | :----------------------------------------------------------------------------- | :---------------------------------------- |
| `OTELFWD_BIN`       | The forwarder                                                                  | `./otelfwd`, otherwise `otelfwd` in PATH  |
| `OTLP_PUSH_API_URL` | Where to push                                                                  | `http://127.0.0.1:4318/v1/logs`           |
| `OTLP_PUSH_TOKEN`   | Bearer token, if the receiver needs one                                        | none                                      |
| `OTLP_CA_FILE`      | CA file for `https://`, for example `tools/otel-sink/otel-data/certs/cert.pem` | none                                      |
| `OTELFWD_DATA_DIR`  | Data directory (WAL, metrics). Set it to keep the WAL of a failed push         | a temporary directory, deleted afterwards |

## Testing

`tools/otel-sink` is a test container with an OTLP/HTTP receiver: NGINX in front (HTTP, HTTPS with a generated certificate, a bearer token check, a port which always fails) and a small program which writes every POST to its own JSON file.
It is built from Alpine with Docker Compose. See [tools/otel-sink/README.md](tools/otel-sink/README.md).

```bash
./tools/otel-sink/run.sh
```

The whole path can be load tested: `nginx/run_loadtest.sh` starts the sink, `otelfwd` and a test NGINX, sends a large number of requests and checks that every event arrives, how fast, and where events are lost if not. See [nginx/README.md](nginx/README.md#load-test).

```bash
./nginx/run_loadtest.sh
```

## Metrics

Metrics are written to the Prometheus file (`OTELFWD_PROM_FILE`) every 10 seconds and at shutdown.

| Metric                                              | Description                                                                  |
| :-------------------------------------------------- | :--------------------------------------------------------------------------- |
| `otelfwd_lines_received_total`                      | Log lines received (STDIN and sockets)                                       |
| `otelfwd_push_total{result="success\|error"}`       | Log lines pushed, by result                                                  |
| `otelfwd_push_retry_total{result="success\|error"}` | WAL records (one push request each) replayed, by result                      |
| `otelfwd_socket_lines_total{source,result}`         | Lines received on a socket input. `result`: `accepted`, `dropped`, `invalid` |
| `otelfwd_socket_connections_total{source,result}`   | Connections on a socket input. `result`: `accepted`, `rejected`              |

`source` is `unix`, `tcp` or `syslog`. The socket metrics are only written for enabled inputs. The syslog input is a datagram socket and has no connection metrics.

## Migration from version 1.x

Version 2.0 removes the Loki push API and the Alloy output format.
The program was called `domfwd` in version 1.x. It has been renamed to `otelfwd`, and all names which carried the old name have changed with it.

| Version 1.x                                                                      | Version 2.0                                          |
| :------------------------------------------------------------------------------- | :--------------------------------------------------- |
| program `domfwd`                                                                 | program `otelfwd`                                    |
| `LOKI_PUSH_API_URL` (`.../loki/api/v1/push`)                                     | `OTLP_PUSH_API_URL` (for example `.../otlp/v1/logs`) |
| `LOKI_PUSH_TOKEN`                                                                | `OTLP_PUSH_TOKEN`                                    |
| `LOKI_CA_FILE`                                                                   | `OTLP_CA_FILE`                                       |
| `LOKI_NAMESPACE`                                                                 | `OTLP_SERVICE_NAMESPACE`                             |
| `LOKI_POD`                                                                       | `OTLP_SERVICE_INSTANCE_ID`                           |
| `LOKI_JOB`                                                                       | removed. Use `OTLP_SERVICE_NAME`                     |
| `DOMFWD_ANNOTATE_STDOUT`, `DOMFWD_ANNOATED_LOG`                                  | removed (Alloy output format)                        |
| `DOMINO_DATA_PATH`                                                               | `OTELFWD_DATA_DIR`                                   |
| `DOMINO_OUTPUT_LOG`                                                              | `OTELFWD_OUTPUT_LOG`                                 |
| `DOMFWD_SHUTDOW_MAX_SEC`                                                         | `OTELFWD_SHUTDOWN_MAX_SEC`                           |
| `DOMFWD_LOGLEVEL`, `DOMFWD_HOSTNAME`, `DOMFWD_MIRROR_STDOUT`, `DOMFWD_PROM_FILE` | the same names with the prefix `OTELFWD_`            |
| metrics `domfwd_*`                                                               | metrics `otelfwd_*`                                  |
| metrics file `domfwd.prom`                                                       | metrics file `otelfwd.prom`                          |

* Grafana Loki needs version 3.0 or newer for the OTLP endpoint.
* Dashboards and alerts which use the metrics of version 1.x have to be updated to the new metric prefix.
* The WAL file is now `otelfwd.wal`. Unsent data in the WAL file `domfwd.wal` of version 1.x is not replayed, because it is in the Loki format. `otelfwd` logs a warning if such a file exists and leaves it untouched.
* `otelfwd` logs an error if `LOKI_PUSH_API_URL` is still set.
