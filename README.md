# domino-log-forwarder - HCL Domino logs and events for OpenTelemetry

Forwards the logs and events of an [HCL Domino](https://www.hcl-software.com/domino) server to any [OpenTelemetry](https://opentelemetry.io/) compatible endpoint (OTLP/HTTP with JSON encoding), for example the [Grafana Loki OTLP endpoint](https://grafana.com/docs/loki/latest/send-data/otel/) or an OpenTelemetry Collector.

The repository has two programs which work together, and some tools to test them:

| Component                                             | What it is                                                                                                                                               |
| :---------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------- |
| [`otelfwd`](#otelfwd---the-otel-forwarder)            | The forwarder. Reads the **Domino console log** from STDIN (`server \| otelfwd`) and structured records from local sockets, pushes OTLP                  |
| [`domfwd`](domfwd/README.md)                          | Domino server add-in. Reads the **Domino events** (Event Monitoring queue) and sends one structured record per event to `otelfwd`                        |
| [`nginx/`](nginx/README.md)                           | Test NGINX container which logs via syslog into `otelfwd`, and the end to end load test                                                                  |
| [`tools/victorialogs/`](tools/victorialogs/README.md) | VictoriaLogs container: a real log database to try `otelfwd` with. It needs `OTLP_PUSH_ENCODING=protobuf`                                                |
| [`wal/`](wal/README.md)                               | The write ahead log of `otelfwd` as a module of its own: usable by other programs, with a sample program and its tests                                   |
| [`filereader/`](filereader/README.md)                 | Follows a growing text file line by line and remembers its position. A module of its own, used by the file input of `otelfwd`                            |
| [`mail-log/`](mail-log/README.md)                     | Builds one OTel log record out of a mail message's sender, recipients, subject and meta data, with a sample generator. Not used by otelfwd or domfwd yet |
| [`tools/otel-sink/`](tools/otel-sink/README.md)       | Test OTLP receiver container                                                                                                                             |

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
| In        | STDIN, UNIX socket, syslog datagram socket, a file  | Local only, no network                                                                                                                                                                                                          |
| In        | TCP on loopback (`OTELFWD_TCP_LISTEN`)              | Loopback only. Off unless configured. No default port, the examples use `127.0.0.1:4390`                                                                                                                                        |
| Out       | HTTP or HTTPS to the receiver (`OTLP_PUSH_API_URL`) | The only network connection. No default: the URL sets host and port (OTLP/HTTP standard: 4318, Loki: 3100). Use `https://` with `OTLP_CA_FILE` for a private CA and `OTLP_PUSH_TOKEN` for a bearer token. Needs outbound access |

Inputs:

* **STDIN:** the console log of a Domino server (`server | otelfwd`), annotated with the server task from `pid.nbf`. See [Domino console log (STDOUT)](#domino-console-log-stdout).
* **UNIX socket** and **TCP on 127.0.0.1:** structured records in the [flat record format](#records-received-via-socket-inputs) from other local programs, above all the Domino event add-in [`domfwd`](domfwd/README.md). TCP is loopback only. See [Domino events (domfwd)](#domino-events-domfwd).
* **Syslog** (UNIX datagram socket, only if enabled): syslog messages, for example the access and error log of NGINX. See [Syslog input](#syslog-input).
* **File** (only if enabled): the lines of a text file which grows, with its position saved across restarts, rotation and truncation. See [File input](#file-input).

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
| Standalone              | `otelfwd -nostdin` (and `OTLP_PUSH_API_URL`)                                                                                                     | sockets and file  | `SIGTERM` or `SIGINT`              |

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

* `OTLP_PUSH_API_URL` has to be set, and at least one input has to work: a socket input, or the [file input](#file-input). Without a configured input the default UNIX socket is used. Otherwise the forwarder logs an error and ends with exit code 1.
* `OTELFWD_DATA_DIR` is the directory for the WAL and for the metrics file. It has to be writable. With `-nostdin` the WAL has to open: if it cannot, for example because another instance uses the same directory, the forwarder exits with the code 1. See [Additional configuration](#additional-configuration).
* `OTELFWD_MIRROR_STDOUT` and `OTELFWD_OUTPUT_LOG` only apply to lines read from STDIN. They are ignored with `-nostdin` and a warning is logged.
* The WAL is replayed while the forwarder is running. Records received before a shutdown are pushed first, and records which could not be pushed stay in the WAL for the next start.
* Only records received via the socket inputs and the file input are pushed. They carry their own attributes. The annotation with the Domino server task name via `pid.nbf` only applies to lines read from STDIN.

## Running more than one instance

**One instance is one logical OTLP destination, optionally with a backup for high availability.** The primary and the backup URL
are two endpoints of the same destination, not two targets: they share the token, the CA file and the encoding, and `otelfwd`
fails over to the backup and back again.

To send different kinds of records to different backends, for example the records of the email subsystem to Loki and the Domino
events to VictoriaLogs, run **one instance per destination** and let each producer write to the socket or port of the instance it
belongs to. `otelfwd` does not route records to different targets on purpose: an instance is a process of its own with its own
queue, its own WAL and its own failures, so a backend which is down or slow fills its own WAL and delays no other instance. The
producer chooses the destination by choosing the socket. Sending one record to several backends, or filtering, transforming or
routing records by their content, is a job for an OpenTelemetry Collector downstream of `otelfwd`, not for `otelfwd`.

Plan the instances first, one row each. Everything in a column must differ between the instances:

| Instance | Data directory            | UNIX socket                        | TCP (loopback)   | Target                 |
| :------- | :------------------------ | :--------------------------------- | :--------------- | :--------------------- |
| mail     | `/var/lib/otelfwd-mail`   | `/run/otelfwd-mail/otelfwd.sock`   | `127.0.0.1:4391` | Loki, JSON             |
| domino   | `/var/lib/otelfwd-domino` | `/run/otelfwd-domino/otelfwd.sock` | `127.0.0.1:4392` | VictoriaLogs, protobuf |

### What every instance needs of its own

| What                    | Setting                                                                                                  | If two instances use the same                                                                                                                                                                                                                                                                                                                                                                                                                                     |
| :---------------------- | :------------------------------------------------------------------------------------------------------- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Data directory          | `OTELFWD_DATA_DIR`                                                                                       | It holds the WAL, which is locked. The second instance cannot open the WAL of the first. With `-nostdin` it exits with the code 1 and says why. In pipe mode it keeps running, because an exit would close the pipe of the server: it has no WAL then, its start summary says `cannot be opened, failed pushes cannot be kept`, its health is an error, and a push which fails is lost. The default UNIX socket and the metrics file are below this directory too |
| UNIX socket             | `OTELFWD_UNIX_SOCKET`                                                                                    | The second instance logs `Unix socket is in use by another process` and this input does not start. Give every instance its own path and its own directory for it                                                                                                                                                                                                                                                                                                  |
| TCP port                | `OTELFWD_TCP_LISTEN`                                                                                     | The second instance logs `Cannot listen on TCP address` and this input does not start. Loopback only, and there is no default port: use a different port for each instance                                                                                                                                                                                                                                                                                        |
| Syslog socket           | `OTELFWD_SYSLOG_SOCKET`                                                                                  | The same as for the UNIX socket. Its file mode is `OTELFWD_SYSLOG_SOCKET_MODE`, and the sender (NGINX) must be able to write to it                                                                                                                                                                                                                                                                                                                                |
| File input              | `OTELFWD_FILE_INPUT`                                                                                     | A file is followed by one instance. Its state file is next to it by default, or in `OTELFWD_FILE_STATE_DIR`                                                                                                                                                                                                                                                                                                                                                       |
| STDIN                   | `otelfwd` without `-nostdin`                                                                             | There is one STDIN: the pipe of the Domino console log. Only one instance reads it, all the others run with `-nostdin`                                                                                                                                                                                                                                                                                                                                            |
| Target and its format   | `OTLP_PUSH_API_URL`, `OTLP_PUSH_API_URL_BACKUP`, `OTLP_PUSH_ENCODING`, `OTLP_PUSH_TOKEN`, `OTLP_CA_FILE` | This is what the instances are for: each has its own. Each backend documents the URL path and the encoding it expects                                                                                                                                                                                                                                                                                                                                             |
| Instance name           | `OTELFWD_INSTANCE`                                                                                       | The name of the instance, for example `mail` or `domino`: the label `otelfwd_instance` of every metric. Set it on every instance if the metrics files of several instances are collected together, otherwise their lines cannot be told apart. Not set: no label, and the metrics are as they always were                                                                                                                                                         |
| Identity of the records | `OTLP_SERVICE_NAME`, `OTLP_SERVICE_NAMESPACE`, `OTLP_SERVICE_INSTANCE_ID`, `OTELFWD_HOSTNAME`            | The default resource of the records which carry none. Set them if the records of the instances should be told apart by it. The label of the metrics is not one of them: it is `OTELFWD_INSTANCE`, see the row above                                                                                                                                                                                                                                               |
| Metrics file            | `OTELFWD_PROM_FILE`                                                                                      | The default is below the data directory, so it differs already. The metrics of all instances have the same names, and the label `otelfwd_instance` (the value of `OTELFWD_INSTANCE`) tells the instances apart when it is set, see [Metrics](#metrics)                                                                                                                                                                                                            |

The default UNIX socket (`<data>/domino/otelfwd.sock`) is only opened when `OTLP_PUSH_API_URL` is set and none of the three socket
settings is configured. An instance with an explicit socket, port or syslog socket does not open it. A socket is created with the
mode `OTELFWD_UNIX_SOCKET_MODE` (default `0600`, only the user who started it), so run the instances as the user of the
producers which write to them.

### Example: two standalone instances

The instances of the plan above. The URLs and the encodings are examples: see the documentation of each backend.

```bash
OTELFWD_INSTANCE=mail \
OTELFWD_DATA_DIR=/var/lib/otelfwd-mail \
OTELFWD_UNIX_SOCKET=/run/otelfwd-mail/otelfwd.sock \
OTELFWD_TCP_LISTEN=127.0.0.1:4391 \
OTLP_PUSH_API_URL=http://loki:3100/otlp/v1/logs \
OTLP_PUSH_ENCODING=json \
otelfwd -nostdin
```

```bash
OTELFWD_INSTANCE=domino \
OTELFWD_DATA_DIR=/var/lib/otelfwd-domino \
OTELFWD_UNIX_SOCKET=/run/otelfwd-domino/otelfwd.sock \
OTELFWD_TCP_LISTEN=127.0.0.1:4392 \
OTLP_PUSH_API_URL=https://victorialogs:9428/insert/opentelemetry/v1/logs \
OTLP_PUSH_ENCODING=protobuf \
otelfwd -nostdin
```

To see what an instance will really use before you rely on it, put `-nostdin` before `-cfg` and start it with its environment:
`-cfg` prints the data directory, the sockets and the target, and exits.

### Pointing the producers at an instance

The producer decides which instance a record goes to, by where it writes:

* **A program which builds its own records** (for example one which uses the [mail-log](mail-log/README.md) class) sends each
  line to the UNIX socket or the TCP port of its instance, for example `/run/otelfwd-mail/otelfwd.sock`.
* **The Domino event add-in** (`domfwd`) has its own `DOMFWD_Socket`, `unix:/run/otelfwd-domino/otelfwd.sock` or
  `tcp:127.0.0.1:4392`, see [domfwd](domfwd/README.md).
* **NGINX** logs to the syslog socket of the instance which owns it (`OTELFWD_SYSLOG_SOCKET`).
* **The Domino console log** is the STDIN of the one instance which is not started with `-nostdin`.

A single input cannot be split by content: everything which arrives on one socket goes to the instance which owns it. If records
for different backends would arrive on one input, for example NGINX access and error logs on one syslog socket, give each kind
its own socket at the producer.

### Why an instance has only one destination

This was discussed at length, and the result is a deliberate boundary: **`otelfwd` forwards to one logical destination and does not
route.** The reasons, so the question does not have to be worked out again:

**A target is more than a URL.** Every target has its own state: the URL and its backup, the encoding, the token and the CA file,
a queue, a WAL, the retry and the failover, the metrics and the health. Several targets inside one process would need all of that
once for each target, plus a routing layer in front. That is several forwarders in one process.

**The targets would not be independent.** If one backend becomes slow, its requests and its WAL must not hold up another. To
guarantee that inside one process, every target needs its own queue and its own execution path, and then the CPU, the memory, the
locks, the queue limits and the shutdown are shared concerns between them. Three couplings show up first:

* **The WAL is replayed in order.** A failed entry stops the replay, so entries for two targets in one WAL block each other. Every
  target needs its own WAL.
* **One push thread shares its delays.** A backend which is down already costs a connect timeout for every batch. With one thread,
  that delay goes to every other target too, unless each target has a thread or a cooldown of its own.
* **One queue shares its losses.** A slow target which fills a shared queue makes the socket inputs drop records which were meant
  for a healthy target.

Choosing the target late, when the batch is pushed, and splitting the batch by target so that no request mixes targets works
well, but it is the easy part. The three couplings and everything around them (the settings of each target, its secrets, its
metrics and health, the tests for all of it) remain.

**Separate instances give the independence for free.** Each instance is its own process, so it is its own failure domain and its
own scaling domain, with its own queue and WAL. The operating system does the isolation.

```
one process, several targets (not built)

                    ┌── Loki
Producer ── otelfwd ┼── VictoriaLogs
                    └── Collector
```

```
one instance for each destination (how it is done)

Producer A ──► otelfwd A ──► Loki
                    │
                    └── WAL A

Producer B ──► otelfwd B ──► VictoriaLogs
                    │
                    └── WAL B
```

**The producer picks the destination by picking the socket.** `otelfwd` does not become a routing product, and it has no rules to
configure, test and explain.

**A backup endpoint is not a second target.** The primary and the backup URL are the same logical destination, so they share the
configuration and `otelfwd` fails over between them and back. That is high availability. Independent destinations are separate
instances.

**Fan-out, filtering and transformation belong downstream.** Sending one record to several backends, dropping or changing records
by their content, or routing by an attribute value is what an OpenTelemetry Collector does. If that is needed, put a Collector
behind an instance.

**What instances cannot do:** they split by input, not by content. One input which carries records for two backends goes to the one
instance which owns it, and the fix is at the producer, a socket of its own for each kind. If a real input ever has to be split by
content and cannot be split at the producer, a smaller design than the first one is the place to start: route after the batch is
taken, split it by target, with a WAL per target and a cooldown after a failure. It is not built, because a separate process costs
no code at all.

## Command line

| Parameter  | Description                                                                     |
| :--------- | :------------------------------------------------------------------------------ |
| `-nostdin` | Do not read STDIN. Only the socket inputs are used, until `SIGTERM` or `SIGINT` |
| `-cfg`     | Print the configuration and exit                                                |
| `-env`     | Print the configuration as environment variables and exit                       |
| `-help`    | Print the help and exit                                                         |
| `-version` | Print the version and exit                                                      |

`-cfg` shows the configuration as parsed so far. Put `-nostdin` before `-cfg` to see it in the output.

## Log at start

At every start `otelfwd` logs the configuration which is in use, to stderr, one setting for each line as `Name: value`. The names are the ones of `-cfg`. This is where to look first when logs do not arrive where they are expected:

```text
2026-09-19T21:34:01Z  otelfwd: STDIN input: yes
2026-09-19T21:34:01Z  otelfwd: Data Dir: /local/notesdata
2026-09-19T21:34:01Z  otelfwd: Metrics File: /local/notesdata/domino/stats/otelfwd.prom
2026-09-19T21:34:01Z  otelfwd: OTLP Push API URL: https://otel.example.com:4318/v1/logs
2026-09-19T21:34:01Z  otelfwd: OTLP Push Encoding: json (Content-Type application/json)
2026-09-19T21:34:01Z  otelfwd: OTLP Push Token: set
2026-09-19T21:34:01Z  otelfwd: OTLP CA File: /local/notesdata/trusted_root.pem
2026-09-19T21:34:01Z  otelfwd: OTLP Push API URL Backup: https://otel-b.example.com:4318/v1/logs
2026-09-19T21:34:01Z  otelfwd: OTLP Push Failback sec: 60
2026-09-19T21:34:01Z  otelfwd: WAL File: /local/notesdata/otelfwd.wal
2026-09-19T21:34:01Z  otelfwd: WAL Pending: 610 bytes of an earlier run, will be replayed
2026-09-19T21:34:01Z  otelfwd: OTLP Push WAL Retry sec: 60
2026-09-19T21:34:01Z  otelfwd: OTLP Service Name: domino
2026-09-19T21:34:01Z  otelfwd: OTLP Service Namespace: domino
2026-09-19T21:34:01Z  otelfwd: OTLP Service Instance: domino1
2026-09-19T21:34:01Z  otelfwd: Unix Socket: /local/notesdata/domino/otelfwd.sock (default)
2026-09-19T21:34:01Z  otelfwd: Output log: /local/notesdata/notes.log
2026-09-19T21:34:01Z  otelfwd: Mirror to stdout: yes
```

Every line of the console output of `otelfwd` starts with the time in UTC (ISO 8601), followed by two blanks. Errors and warnings have their level in brackets, for example `[Error]`. This is also true for the messages of the WAL:

```text
2026-09-19T21:35:12Z  otelfwd: [Error] Curl operation failed: Failed to connect to localhost port 9428 after 0 ms: Could not connect to server
2026-09-19T21:35:12Z  otelfwd: [Error] Cannot send the WAL to the receiver. Trying again in 60 seconds
```

* **The values which are really used** are shown, after the checks of the configuration. If a setting was invalid and was replaced or ignored, an error line before these lines says so, and the summary shows the result, for example no backup endpoint.
* **No secrets.** The token is only shown as `set` or `not set`. The URLs are shown without user name, password, query and fragment, because a URL can carry a secret there (`https://user:password@host/path?token=...`). `-cfg` and `-env` do the same.
* Without `OTLP_PUSH_API_URL` the summary says that OTLP push is off and leaves out the settings of the push. The settings of the backup endpoint, `WAL Pending` and the output log are only there when they are used. If the WAL cannot be opened, the `WAL File` line says so.
* **The sockets** which are used are listed: `Unix Socket`, `TCP Listen` and `Syslog Socket` if they are set, and the default Unix socket, marked `(default)`, when none is set (see [Socket inputs](#socket-inputs)). The lines of the [file input](#file-input) are there when it is set.
* With `-nostdin` the lines have no `otelfwd:` after the time. It is there in pipe mode, where the lines share the output with the mirrored lines of the server.
* The mirrored lines of the server on stdout, the output log file, and the output of `-cfg` and `-help` have no time stamp.
* `-cfg` shows everything, also the settings which are not set. See [Command line](#command-line).

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

| Variable Name              | Description                                                                                                                                          | Example / Comments                               |
| :------------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------- | :----------------------------------------------- |
| `OTELFWD_DATA_DIR`         | Data directory. Has to be writable. Used for the WAL and the default metrics file                                                                    | default: `/local/notesdata`                      |
| `OTELFWD_LOGLEVEL`         | Log level for stdout logging                                                                                                                         | `1`                                              |
| `OTELFWD_HOSTNAME`         | Hostname to use                                                                                                                                      | default: hostname read from OS                   |
| `OTELFWD_INSTANCE`         | Name of this instance, for example `mail`. Only needed when more than one instance runs: it is added to every metric as the label `otelfwd_instance` | default: not set, no label                       |
| `OTELFWD_PROM_FILE`        | Prom File for Metrics output                                                                                                                         | default: `<notesdata>/domino/stats/otelfwd.prom` |
| `OTELFWD_SHUTDOWN_MAX_SEC` | Shutdown max wait seconds                                                                                                                            | default: `30`                                    |

### OTLP push configuration

OTLP push is optional and only active when `OTLP_PUSH_API_URL` is set.
The URL is the complete logs endpoint of the receiver. For the OpenTelemetry Collector this is `/v1/logs`, for Grafana Loki it is `/otlp/v1/logs`.

| Variable Name              | Description                                                     | Example / Comments                        |
| :------------------------- | :-------------------------------------------------------------- | :---------------------------------------- |
| `OTLP_PUSH_API_URL`        | OTLP/HTTP logs endpoint                                         | `https://otel.example.com:4318/v1/logs`   |
| `OTLP_PUSH_API_URL_BACKUP` | Optional backup endpoint, see below                             | `https://otel-b.example.com:4318/v1/logs` |
| `OTLP_PUSH_FAILBACK_SEC`   | Seconds between tries of the primary while the backup is in use | default: `60`                             |
| `OTLP_PUSH_WAL_RETRY_SEC`  | Seconds to wait before the WAL is sent again after a failure    | default: `60`, see below                  |
| `OTLP_PUSH_WAL_MAX_MB`     | Largest size of the WAL in MB, 0 is no limit                    | default: `128`, see below                 |
| `OTLP_PUSH_TOKEN`          | Bearer token for the endpoint                                   | `my-secure-token`                         |
| `OTLP_CA_FILE`             | Trusted Root CA File                                            | `/local/notesdata/trusted_root.pem`       |
| `OTLP_SERVICE_NAME`        | `service.name` resource attribute                               | default: `domino`                         |
| `OTLP_SERVICE_NAMESPACE`   | `service.namespace`                                             | default: `domino`                         |
| `OTLP_SERVICE_INSTANCE_ID` | `service.instance.id`                                           | default: hostname                         |

#### Backup endpoint

`OTLP_PUSH_API_URL_BACKUP` is an optional second endpoint for the case that the first one fails. It uses the same token and CA file. Both endpoints must accept the same data, for example the same Loki tenant. Only one backup is possible, and the primary is the endpoint which `otelfwd` prefers.

* **Failover:** requests go to the primary. If a request is not delivered, the same request goes to the backup. If that delivers it, the backup stays in use: the next requests go to it directly, and a failing primary does not delay them.
* **Failback:** while the backup is in use, the primary is tried again every `OTLP_PUSH_FAILBACK_SEC` seconds (default 60, from 1 to 86400), with a real request. If it delivers, the primary is used again. If the backup fails while it is in use, the primary is tried at once.
* **Both fail:** the request is kept in the WAL as before, and the WAL replay uses the same rules.
* **A refused request** (HTTP 400, see [Durable Log Delivery](#durable-log-delivery)) is not sent to the other endpoint: it would refuse the same data.
* Every change of the endpoint in use is logged once. The metrics show the endpoint and the switches, see [Metrics](#metrics).
* **Duplicates are possible.** If an endpoint received a request but the answer was lost, the other endpoint gets the same request. Delivery is at least once.
* **Timeouts:** a connection which is not established within 3 seconds counts as failed. A whole request has 15 seconds, the try of the primary while the backup is in use 5 seconds.
* **Checks at start:** a backup without `OTLP_PUSH_API_URL`, one which does not start with `http://` or `https://`, or the same as the primary is reported and not used. An invalid interval is reported and 60 is used. The program keeps running.

A name with several IP addresses is another way to spread over two servers: libcurl tries the next address when a connection cannot be made. It does not help when a server accepts the connection but answers with an error, which the backup endpoint covers.

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

* **Default socket:** if `OTLP_PUSH_API_URL` is set and none of `OTELFWD_UNIX_SOCKET`, `OTELFWD_TCP_LISTEN` and `OTELFWD_SYSLOG_SOCKET` is configured, the forwarder listens on `<OTELFWD_DATA_DIR>/domino/otelfwd.sock` and creates the directory if needed. This is where the Domino add-in [`domfwd`](domfwd/README.md) sends by default, so both sides need no setting. `-cfg` and the [log at start](#log-at-start) show the socket which is used, marked `(default)`. With `-cfg` this follows the parameters in the order they are given: `-nostdin` counts only if it comes before `-cfg`. `-env` only shows what can be set. Setting any socket input turns the default off. The file input does not: it is one more input.
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

### File input

`otelfwd` can follow **a text file** like `tail -F` and push every line as a log record. It is meant for logs of programs which write to a file and have no other way to send their logs. It is only enabled if requested. It runs in its own thread and needs `OTLP_PUSH_API_URL`. It works in pipe mode and in standalone mode (`-nostdin`), and one file is followed for now.

| Variable Name            | Description                                                                                                                                                                    | Example / Comments                                        |
| :----------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :-------------------------------------------------------- |
| `OTELFWD_FILE_INPUT`     | The file to follow. The file input is enabled if it is set. The file does not have to exist yet                                                                                | `/var/log/myapp/app.log`                                  |
| `OTELFWD_FILE_START`     | Where to start when there is no state file (the first start): `begin` reads what is in the file, `end` only lines which come later                                             | default: `begin`                                          |
| `OTELFWD_FILE_STATE_DIR` | A directory for the state files, if they should not be next to the files                                                                                                       | default: next to the file: `<file>.otelfwd-state`         |
| `OTELFWD_FILE_SEVERITY`  | The severity of every line: `trace`, `debug`, `info`, `warn`, `error`, `fatal`, or `off` for no severity. A text file has no level of its own, so it is one for the whole file | default: `info`                                           |
| `OTELFWD_FILE_SERVICE`   | `service.name` and `service.namespace` of the lines: the name of the program which writes the file                                                                             | default: `OTLP_SERVICE_NAME` and `OTLP_SERVICE_NAMESPACE` |

**The record:** the line is the body, without the new line (and without a carriage return before it). The time is the time the line was read. The severity is the same for every line (`OTELFWD_FILE_SEVERITY`, `info` by default): a text file has no level, and `off` sends no severity at all. The attribute `log.file.path` is the path of the file. The resource is the default resource of the forwarder (`host.name`, ...), with `service.name` and `service.namespace` from `OTELFWD_FILE_SERVICE` if it is set. The scope is the default scope. Empty lines are not sent. A line which is longer than 1 MB is cut, the rest of it is dropped, and `otelfwd_file_lines_truncated_total` counts it.

**The position is saved when the lines are delivered**, not when they are read. The file is read by the [`FileReader`](filereader/README.md), which keeps the position in a **state file**: next to the file by default (`/var/log/myapp/app.log` has `/var/log/myapp/app.log.otelfwd-state`), or in `OTELFWD_FILE_STATE_DIR` if that is set (`app.log.<hash of the path>.otelfwd-state`: the hash keeps two files with the same name apart, and one directory can hold the state files of many files). The directory has to be writable for `otelfwd`. If it is not (a read only mount, or a directory of another user), `otelfwd` says so at start, the position cannot be saved, and every restart reads the file again: set `OTELFWD_FILE_STATE_DIR` then. `otelfwd` commits it when a batch was accepted by the receiver, or written to the WAL, or refused as bad data. After a restart or a crash `otelfwd` continues at that position. Lines which were read but not delivered are read again, so the delivery is **at least once**, like the [WAL](#durable-log-delivery). The state file is only for the owner (0600), and it belongs to the path: do not delete it unless the file should be read again.

* **Only complete lines.** A line which the program has not finished writing waits until its new line arrives.
* **Rotation** (the file is renamed and a new one is created, like `logrotate` does): the old file is read to its end, then the new file is read from the start. **Truncation** (`copytruncate`, or an emptied file): the file is read again from the start.
* **Backpressure.** The file input only reads while the queue has room (`OTELFWD_SOCKET_QUEUE_MAX`). When the receiver is slow, the queue fills up and the file simply waits: nothing is dropped, the file is the buffer. If the file is rotated away meanwhile, the lines which were not read yet are in the old file, and they are read first.
* **Where it starts:** without a state file the file is read from the beginning (`OTELFWD_FILE_START=begin`), which sends a large old file completely. With `end` only lines which come later are sent, and the position is saved at once. A state file which is damaged, or which belongs to another file (the file was replaced while `otelfwd` was stopped), is reported, and the file is read from the start.
* **Other inputs:** the file input is one more input. It does not stop the others: the default Unix socket (for `domfwd`) is still opened when no socket input is configured, in pipe mode and with `-nostdin`. With `-nostdin` the file input also counts as a working input, so a file-only run does not end with an error.
* **What it does not do:** several files or patterns (one file for now), the joining of lines which belong together (a stack trace), and parsing of a time or a severity out of the line (the severity is the one of the setting for every line).

```bash
export OTLP_PUSH_API_URL=https://otel.example.com:4318/v1/logs
export OTELFWD_FILE_INPUT=/var/log/myapp/app.log
export OTELFWD_FILE_START=end

otelfwd -nostdin
```

The module has a test of its own, see [filereader/README.md](filereader/README.md#test). `make test` runs it. The names of the state files and the hand over of the position from the push thread to the file thread are tested in [`otelfwd_unit_test.cpp`](#unit-test-of-otelfwd).

## Durable Log Delivery

`otelfwd` uses a local Write Ahead Log (WAL) to ensure logs are not lost during temporary network or server outages.

Failed log pushes are written to the WAL and replayed automatically once connectivity is restored.
One WAL record is one push request, which can contain multiple log lines.
The WAL file is `otelfwd.wal` in the data directory (`OTELFWD_DATA_DIR`).

**When the WAL is sent.** The WAL is checked every second. If the receiver does not accept a record, the replay stops there. `otelfwd` logs `Cannot send the WAL to the receiver. Trying again in 60 seconds` and waits `OTLP_PUSH_WAL_RETRY_SEC` seconds (default 60, from 1 to 86400, up to a second more in practice) before the next try. An invalid value is reported at start and 60 is used. The value in use is in the log at start and in `-cfg`.

**How large the WAL can get.** `OTLP_PUSH_WAL_MAX_MB` (default 128, from 0 to 1048576) is the largest size of the WAL file. A record which does not fit is not stored: that push is dropped, and there is one warning in the log until a record was stored again. The file only gets smaller when the WAL is emptied, and a WAL of an earlier run which is larger than the limit is still sent, but takes no new records until it is empty. `0` is no limit: the WAL grows until the disk is full. An invalid value is reported at start and 128 is used. The value in use is in the log at start and in `-cfg`.

* Records are sent in the order they were written. The position of the last accepted record is saved when a replay stops, so the next try continues there. When every record was sent, the WAL is emptied. Delivery is at least once: after a crash a record can be sent twice.
* New log lines do not wait for the WAL. They are pushed at once, so after an outage they can arrive before the older lines, which keep their original time.
* A replay which worked has no message of its own at the default log level. `otelfwd_push_retry_total{result="success"}` counts the records which were replayed.

**What counts as delivered.** The HTTP status of the answer of the receiver decides what happens to a push request:

| Answer of the receiver                                                                                                          | What happens                                                                                                                                                                                                                                                                                 |
| :------------------------------------------------------------------------------------------------------------------------------ | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 200 - 299                                                                                                                       | Delivered                                                                                                                                                                                                                                                                                    |
| 400                                                                                                                             | The receiver refuses this data for good. It is **dropped**, not kept in the WAL and not sent to the backup endpoint. It is counted (`otelfwd_push_rejected_total`) and logged with the answer of the receiver. In the WAL replay only that record is dropped, the records behind it continue |
| Everything else: no connection, timeout, 3xx (redirects are not followed), 401, 403, 404, 408, 413, 429, 500, 502, 503, 504 ... | Not delivered. The backup endpoint is tried if there is one, then the request is kept in the WAL and tried again later. The log names the status and the start of the answer                                                                                                                 |

The OTLP specification retries only 429, 502, 503 and 504, and says that all other 4xx and 5xx codes must not be retried. `otelfwd` is more careful with the data. A wrong token (401), a wrong URL (404) or a size limit of the receiver (413) is a problem of the configuration which is fixed later, and dropping the logs meanwhile would lose them. Only a 400, the receiver saying that the data itself is bad, fails completely.

#### The WAL as a module

The WAL is a module of its own in [`wal/`](wal/README.md): the class `SimpleWAL` with a sample program, its tests and its README. It can be used by other programs. The interface, the rules, the files and the tests are described there. `otelfwd` sets a function which writes the messages of the WAL as lines of its console output, with the time (see [Log at start](#log-at-start)).

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
    "name": "domino.event",
    "version": "0.9.0"
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

| Test                       | How to run                                                                                       | What it needs                                 | What it checks                                                                                                                                                                                                                                                        |
| :------------------------- | :----------------------------------------------------------------------------------------------- | :-------------------------------------------- | :-------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| WAL unit test              | `make test`                                                                                      | a C++ compiler and `make`                     | The WAL module on its own: behaviour, failures, crashes, use from several threads, one record at a time (Peek and Ack), the size limit, its messages, speed ([details](wal/README.md#the-tests))                                                                      |
| File reader unit test      | `make test`                                                                                      | a C++ compiler and `make`                     | The file reader module on its own: complete and unfinished lines, commit and restart, rotation, truncation, damaged state files, long lines ([details](filereader/README.md#test))                                                                                    |
| MailLog unit test          | `make test`                                                                                      | a C++17 compiler (g++ 11 or newer) and `make` | The MailLog module on its own: every field, the order of the attributes, header name normalization, escaping ([details](mail-log/README.md#test))                                                                                                                     |
| Unit test of otelfwd       | `make test`                                                                                      | a C++ compiler and `make`                     | The failover between two OTLP endpoints (backup, failback timing, refused data, threads), the converter from JSON to protobuf, and the format of the log lines ([details](#unit-test-of-otelfwd))                                                                     |
| Durable sender test        | `make test`                                                                                      | a C++ compiler and `make`                     | The socket sender of `domfwd` together with the WAL: lines wait on disk while the receiver is down, arrive in order when it is back, survive a restart and the end of the program ([details](#unit-test-of-the-durable-sender-of-domfwd))                             |
| Load test                  | `./nginx/run_loadtest.sh`                                                                        | Docker                                        | Every event of a large NGINX load arrives at a test receiver exactly once                                                                                                                                                                                             |
| Load test with an outage   | `./nginx/run_loadtest.sh --yes --threads 4 --requests 2000 --fail-for 10 --wait 420 --stall 200` | Docker                                        | The same while the receiver fails for 10 seconds: the events must be kept in the WAL and arrive after the replay ([details](#load-test-with-an-outage-the-wal-end-to-end))                                                                                            |
| Shared data directory test | `./tests/test_shared_data_dir.sh`                                                                | the built `otelfwd`, `bash`                   | Two instances on one data directory: with `-nostdin` the second exits with the code 1 and says why, in pipe mode it keeps running and says that it has no WAL, and an instance with a data directory of its own is not affected. UNIX sockets only, nothing is pushed |
| Metrics test               | `./tests/test_metrics.sh`                                                                        | the built `otelfwd`, `bash`                   | The metrics file: every line well formed, no label `otelfwd_instance` without `OTELFWD_INSTANCE`, the label on every line with it (and escaped), and two instances read together write no series which the other one writes too. UNIX sockets only, nothing is pushed |

The two bash scripts are in `tests/` and share `tests/common.sh`. `make test_scripts` builds `otelfwd` and runs both; they are not part of `make test`, which needs no shell and no built forwarder.

`--yes` in the commands of `run_loadtest.sh` means: do not ask for the size of the test. Without it, the script asks in a terminal for the number of threads and requests, and Enter takes the default (8 threads with 10,000 requests each). `./nginx/run_loadtest.sh --help` lists all options of the script and of the load test program.

### Unit test of otelfwd

`otelfwd_unit_test.cpp` tests the pieces of `otelfwd` which need no network and no other program: the failover between two endpoints, the converter from JSON to protobuf, the format of the log lines, and the label of the metrics. `make test` builds and runs it, or directly:

```bash
./otelfwd_unit_test
```

**The failover** (`push_failover.hpp`): which endpoint gets a push request. There is no network. The failover object does not send anything: the test gives it a function which answers what the test says (delivered, not delivered, or refused) and writes down which endpoint was called (`P` primary, `p` primary tried again while the backup is in use, `B` backup). Time is a parameter, so the 60 seconds of the failback need no waiting.

It checks: no backup, a working primary, the failover and that the backup stays in use, both endpoints failing, refused data (not sent to the other endpoint), the failback at exactly the configured time (not a second before), the immediate try of the primary when the backup fails, the statistics, and threads: when the probe of the primary is due and 8 threads send at the same second, exactly one of them makes it. The output has sections and `[PASS]` / `[FAIL]` lines like the WAL test, see [Reading the output](wal/README.md#reading-the-output).

**The converter** from JSON to protobuf (`otlp_protobuf.hpp`, used with `OTLP_PUSH_ENCODING=protobuf`). The expected bytes were worked out by hand from the OTLP protobuf definition and are written in hex in the test, with the structure in the spaces. It checks:

* A record with all fields and all four types of values, the largest and the smallest int64, times as a string and as a number (also above the largest int64), a bool which is false, and members of the JSON in another order.
* Text: non-ASCII characters, an empty string, a zero byte inside a string, and lengths above 127, which are written as two byte varints on every level.
* The order of records and of `resourceLogs`, empty input, and that only the given length of the input is read.
* What must be refused: broken JSON, more than one JSON value, a member or a type of value which the converter does not know, a value with two types, wrong types, and numbers which are out of range. An error returns no bytes, and the error text names the member.

**The log lines** (`log_line.hpp`): the console output of `otelfwd`. The time is UTC in ISO 8601, then two blanks, the process name (in pipe mode), the level, the message and a text. The expected times are what the `date` command says for the same number of seconds, also when the host time zone is 12 hours ahead. The lines are written to stderr in one piece, and without a process name with `-nostdin`.

**The metrics label** (`prom_label.hpp`): the label `otelfwd_instance` which is added to the name of every metric when `OTELFWD_INSTANCE` is set, in front of the labels it has (`otelfwd_push_total{result="success"}` becomes `otelfwd_push_total{otelfwd_instance="mail",result="success"}`), and the removal of it, which the load test uses to find a metric by its name with or without the label. It checks a metric without labels, with one and with more, an empty value, the escaping of a quote, a backslash and a new line, and that adding and removing it again gives the name back.

### Unit test of the durable sender of domfwd

`domfwd/domfwd_durable_test.cpp` tests `domfwd/domfwd_durable.hpp`: the socket sender of `domfwd` together with the [WAL](wal/README.md). It is also the test of the socket sender. The test starts a small server of its own on a UNIX socket. The server can be stopped and started again, which is what happens to `otelfwd`, and it can stop reading, which fills the socket. There is no Domino and no `otelfwd`. `make test` builds and runs it, or directly:

```bash
./domfwd_durable_test
```

It checks: a receiver which is there (the fast path, the WAL is not used), a receiver which is not there (the lines wait in the WAL and not in memory, and arrive in order when it is back), that a new line never overtakes lines which wait in the WAL, that one `Pump` moves only a bounded number of lines, a queue which is smaller than the low water mark, the end of the program (what is in memory goes to the WAL, and is sent at the next start), a full WAL (refused lines are counted), lines which are not valid, a record with a new line inside in a WAL which somebody else wrote, no WAL at all, and a WAL which cannot be opened. It also checks that `HasRoom()` of the sender always says what `Enqueue()` does, and the functions which take the unsent lines out of the queue.

### Test receiver and load test

`tools/otel-sink` is a test container with an OTLP/HTTP receiver: NGINX in front (HTTP, HTTPS with a generated certificate, a bearer token check, a port which always fails) and a small program which writes every POST to its own JSON file.
It is built from Alpine with Docker Compose. See [tools/otel-sink/README.md](tools/otel-sink/README.md).

```bash
./tools/otel-sink/run.sh
```

The whole path can be load tested: `nginx/run_loadtest.sh` starts the sink, `otelfwd` and a test NGINX, sends a large number of requests and checks that every event arrives, how fast, and where events are lost if not. See [nginx/README.md](nginx/README.md#load-test).

```bash
./nginx/run_loadtest.sh
```

### Load test with a backup endpoint and with refused data

Two options of the load test check the two endpoints of `otelfwd` with the same sink. `--backup` makes the primary endpoint fail and requires that every event arrives through the backup. `--reject` makes the primary answer `400` and requires that nothing arrives: the data is dropped, not kept in the WAL and not sent to the backup. See [Failover and refused data](nginx/README.md#failover-and-refused-data).

```bash
./nginx/run_loadtest.sh --backup --yes --threads 4 --requests 2000
```

```bash
./nginx/run_loadtest.sh --reject --yes --threads 4 --requests 2000
```

### Load test with an outage: the WAL end to end

`--fail-for SEC` makes the test sink answer `503` for that long, and then recover. Every push fails at first, so the events go to the WAL, and `otelfwd` replays them afterwards. Every event must still arrive exactly once. The test has to wait for the replay, so it needs long limits:

```bash
./nginx/run_loadtest.sh --yes --threads 4 --requests 2000 --fail-for 10 --wait 420 --stall 200
```

* **`--wait` and `--stall` are both needed.** `otelfwd` waits `OTLP_PUSH_WAL_RETRY_SEC` seconds (default 60) before it retries the WAL, and nothing arrives in that time. The test also stops when nothing new arrived for `--stall` seconds (default 15). With the default it gives up after 15 seconds and reports every event as missing, although they are still in the WAL.
* **What to expect:** `RESULT: PASS`. The events arrive about a minute after they were sent (the wait of the WAL retry). In the block about `otelfwd`, `lines pushed ok / failed` shows all lines as failed, because the first push of every one failed, and `WAL requests replayed ok` is above 0. Duplicates are allowed (at least once).
* The run takes a few minutes.

## Metrics

Metrics are written to the Prometheus file (`OTELFWD_PROM_FILE`) every 10 seconds and at shutdown.

With more than one instance, `OTELFWD_INSTANCE` names each instance, for example `mail` or `domino`, and every metric line then
has the label `otelfwd_instance` with that name, in front of the labels a metric has: `otelfwd_push_total{result="success"}` is
written as `otelfwd_push_total{otelfwd_instance="mail",result="success"}`. Two instances write the same metric names, and with
the label a collector which reads the files of both can tell them apart, and a query can select one instance or add them up. The
names in the table are without it. **Without `OTELFWD_INSTANCE` there is no label at all** and the file is exactly what it was
before: one instance does not need it. The label is not called `instance`, because Prometheus adds its own `instance` label to
every target it scrapes and renames a clashing one to `exported_instance`. See
[Running more than one instance](#running-more-than-one-instance).

| Metric                                                  | Description                                                                                                                                               |
| :------------------------------------------------------ | :-------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `otelfwd_lines_received_total`                          | Log lines received (STDIN and sockets)                                                                                                                    |
| `otelfwd_push_total{result="success\|error"}`           | Log lines pushed, by result                                                                                                                               |
| `otelfwd_push_retry_total{result="success\|error"}`     | WAL records (one push request each) replayed, by result                                                                                                   |
| `otelfwd_push_rejected_total`                           | Push requests which the receiver refused as bad data (HTTP 400) and which were dropped                                                                    |
| `otelfwd_wal_bytes`                                     | Gauge. Size of the WAL: push requests which wait on disk for the receiver. Only with a push target                                                        |
| `otelfwd_wal_refused_total`                             | Push requests which the WAL did not take (it was full, or a failure). They are lost. Only with a push target                                              |
| `otelfwd_file_lines_total`                              | Lines read from the file input and queued. Empty lines are skipped. Only with a file input                                                                |
| `otelfwd_file_lines_truncated_total`                    | Lines of the file input which were longer than 1 MB and were cut                                                                                          |
| `otelfwd_file_committed_offset_bytes`                   | Gauge. The position in the file up to which the lines were delivered. It starts at 0 again when the file is rotated or truncated                          |
| `otelfwd_file_open`                                     | Gauge. 1 if the file is open, 0 if it does not exist (yet)                                                                                                |
| `otelfwd_health`                                        | Gauge. Health for alerting: 0 is OK, 1 is a warning, 2 is an error, see [Health](#health)                                                                 |
| `otelfwd_push_endpoint_active`                          | Gauge. The endpoint in use: 0 is the primary, 1 is the backup                                                                                             |
| `otelfwd_push_endpoint_requests_total{endpoint,result}` | Push requests to an endpoint (`primary`, `backup`). `result`: `accepted`, `retry`, `rejected`. A try of the primary while the backup is in use counts too |
| `otelfwd_push_failovers_total`                          | Times the backup was taken into use because the primary failed                                                                                            |
| `otelfwd_push_failbacks_total`                          | Times the primary was taken into use again                                                                                                                |
| `otelfwd_socket_lines_total{source,result}`             | Lines received on a socket input. `result`: `accepted`, `dropped`, `invalid`                                                                              |
| `otelfwd_socket_connections_total{source,result}`       | Connections on a socket input. `result`: `accepted`, `rejected`                                                                                           |

The `otelfwd_push_endpoint_*`, `failovers` and `failbacks` metrics are only written if a backup endpoint is configured. `source` is `unix`, `tcp` or `syslog`. The socket metrics are only written for enabled inputs. The syslog input is a datagram socket and has no connection metrics.

## Health

`otelfwd_health` is one number for alerting: **0** is OK (green), **1** is a warning (yellow), **2** is an error (red). `domfwd` has the same metric (`domfwd_health`) with the same rules. It is calculated in the program, and updated when the metrics are written. A change of the state is logged once with the reason, for example `Health: WARNING (the WAL is more than a quarter full)`.

The worst of these rules wins. Otherwise the state is OK, also in the first minutes of an outage: a restart of the receiver does not raise an alert.

| Rule                                                                             | Warning (1) | Error (2)  |
| :------------------------------------------------------------------------------- | :---------- | :--------- |
| The WAL is full to a share of its size limit                                     | 25%         | 50%        |
| The receiver is not reachable for longer than                                    | 15 minutes  | 30 minutes |
| Data was dropped in the last 10 minutes (the WAL was full or refused it)         |             | yes        |
| Data was rejected for good in the last 10 minutes (HTTP 400, or an invalid line) | yes         |            |
| A WAL is needed and could not be opened                                          |             | yes        |

For `otelfwd` "not reachable" means that no push was accepted since the last failed one: the counter of failed pushes grew, and none succeeded. Nothing to push means no change. Without a size limit (`OTLP_PUSH_WAL_MAX_MB=0`) there is no fill to measure, and only the other rules apply. The limits are in `health.hpp`.

Alert rules, for example:

```
otelfwd_health > 0        # warning
otelfwd_health == 2       # page
```

A program which has stopped writes no metrics at all, so also alert on a missing metric or on a metrics file which is not updated (`otelfwd_lastupdate_timestamp_seconds`).

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
