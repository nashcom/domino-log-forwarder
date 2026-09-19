# VictoriaLogs

A [VictoriaLogs](https://docs.victoriametrics.com/victorialogs/) container to try `otelfwd` with a real log database.
VictoriaLogs is an open source log database from the [VictoriaMetrics](https://victoriametrics.com/) project ([source on GitHub](https://github.com/VictoriaMetrics/VictoriaLogs)).
It has an OTLP/HTTP endpoint for logs, a web UI and a query language ([LogsQL](https://docs.victoriametrics.com/victorialogs/logsql/)).

VictoriaLogs stores the log lines and lets you search them.
It is also the receiver which needs the protobuf format, see [Format: protobuf, not JSON](#format-protobuf-not-json).

Sending the logs:

```text
Domino server  --->  otelfwd  ---- OTLP/HTTP, protobuf ---->  VictoriaLogs
STDOUT, events       JSON in       :9428                       stores the
                     the WAL       /insert/opentelemetry/      log lines
                                   v1/logs
```

Searching the logs:

```text
VictoriaLogs  --->  web UI        http://localhost:9428/select/vmui/
              --->  query API     http://localhost:9428/select/logsql/query   (LogsQL)
```

## Quick start

You need Docker. From the repository root:

```bash
docker compose -f victorialogs/compose.yml up -d
```

Send the sample Domino console lines with `otelfwd`. **`OTLP_PUSH_ENCODING=protobuf` is required**:

```bash
cat examples/sample-console.log | OTLP_PUSH_API_URL=http://localhost:9428/insert/opentelemetry/v1/logs OTLP_PUSH_ENCODING=protobuf ./otelfwd
```

`otelfwd` logs the format it uses at start. Look for this line:

```text
2026-09-19T21:34:01Z  otelfwd: OTLP Push Encoding: protobuf (Content-Type application/x-protobuf)
```

Open the web UI and search for the lines:

```text
http://localhost:9428/select/vmui/
```

Or ask from the command line:

```bash
curl -s http://localhost:9428/select/logsql/query -d 'query=_time:10m | limit 5'
```

Stop VictoriaLogs. The logs stay in the Docker volume:

```bash
docker compose -f victorialogs/compose.yml down
```

## Standalone mode

The same settings work for `otelfwd` in standalone mode, which reads from its sockets instead of STDIN (for example the syslog socket of the NGINX test container, or `domfwd`):

```bash
OTLP_PUSH_API_URL=http://localhost:9428/insert/opentelemetry/v1/logs OTLP_PUSH_ENCODING=protobuf ./otelfwd -nostdin
```

In pipe mode (`server | otelfwd`) put the two variables in front of `otelfwd` in the same way.

## Format: protobuf, not JSON

`otelfwd` works with OTLP/HTTP JSON internally, and the WAL holds JSON. Some receivers only accept OTLP as protobuf. VictoriaLogs is one of them.
With the default setting `OTLP_PUSH_ENCODING=json` it refuses every request:

```text
2026-09-19T21:34:07Z  otelfwd: [Error] The receiver refused the push request as bad data. The data is dropped: HTTP status 400: json encoding isn't supported for opentelemetry format. Use protobuf encoding
```

With `OTLP_PUSH_ENCODING=protobuf`, `otelfwd` converts each request from JSON to protobuf just before it is sent, and sends it with `Content-Type: application/x-protobuf`.
Nothing else changes: batching, the WAL, retry and the backup endpoint work as before.

| Setting                       | What is sent                                                     |
| :---------------------------- | :--------------------------------------------------------------- |
| `OTLP_PUSH_ENCODING=json`     | `application/json`. The default. For receivers which accept JSON |
| `OTLP_PUSH_ENCODING=protobuf` | `application/x-protobuf`. Needed for VictoriaLogs                |

Good to know:

* **A wrong format loses data.** HTTP status `400` means "the data is bad": `otelfwd` does not retry it, does not keep it in the WAL, and does not send it to the backup endpoint. It is logged (at most every 10 seconds) and counted in `otelfwd_push_rejected_total`. If that counter grows while nothing arrives, check the log for `OTLP Push Encoding` and the answer of the receiver.
* **Both endpoints use the same format.** `OTLP_PUSH_API_URL_BACKUP` gets protobuf too when `OTLP_PUSH_ENCODING=protobuf` is set. The test receiver `tools/otel-sink` stores protobuf as `.bin` files.
* **A record which cannot be converted is dropped.** It is logged and counted in `otelfwd_push_convert_errors_total` (only present with protobuf).

## What VictoriaLogs gets

Every log line is one OTLP log record. `otelfwd` sends the resource attributes (`service.name`, `service.namespace`, `service.instance.id`, `host.name`, `os.type`), the scope (name and version), and per record the time, the severity if there is one, the line as the body, and attributes such as `domino.task` and `process.pid`.
The complete list is in the [main README](../README.md).

VictoriaLogs treats the resource attributes as the fields which identify a log stream ("stream fields", see the [OpenTelemetry page](https://docs.victoriametrics.com/victorialogs/data-ingestion/opentelemetry/) of the documentation).
The other names it creates are best seen on your own data: the query below shows the records as JSON, and the web UI has a JSON view too.

```bash
curl -s http://localhost:9428/select/logsql/query -d 'query=_time:10m | limit 3'
```

A few queries (see the [LogsQL reference](https://docs.victoriametrics.com/victorialogs/logsql/) for everything else):

```bash
curl -s http://localhost:9428/select/logsql/query -d 'query=_time:1h error'
```

```bash
curl -s http://localhost:9428/select/logsql/query -d 'query=_time:5m | stats count() lines'
```

## The stack

`compose.yml` starts one container:

| Setting            | Value                                  | Meaning                                                               |
| :----------------- | :------------------------------------- | :-------------------------------------------------------------------- |
| Image              | `victoriametrics/victoria-logs:latest` | The official image                                                    |
| Container name     | `victorialogs`                         |                                                                       |
| Port               | `9428`                                 | OTLP endpoint, web UI and query API                                   |
| `-storageDataPath` | `/victoria-logs-data`                  | Inside the container. The volume `victoria-logs-data` is mounted here |
| `-retentionPeriod` | `30d`                                  | Older logs are deleted (the VictoriaLogs default is 7 days)           |
| `restart`          | `always`                               | Starts again with Docker. Use `down` to stop it for good              |

Endpoints on port `9428`:

| Path                            | Purpose                                               |
| :------------------------------ | :---------------------------------------------------- |
| `/insert/opentelemetry/v1/logs` | OTLP/HTTP logs, protobuf. This is `OTLP_PUSH_API_URL` |
| `/select/vmui/`                 | Web UI                                                |
| `/select/logsql/query`          | Query API: `query=`, `limit=`, `start=`, `end=`       |

## Delete the logs

`down` keeps the volume. To start with an empty database, remove the volume as well:

```bash
docker compose -f victorialogs/compose.yml down -v
```

## Notes

* This is a test setup. There is no authentication and no TLS. The compose file publishes port `9428` on all interfaces of the host. Use `"127.0.0.1:9428:9428"` in `ports:` if others must not reach it.
* The image tag is `latest`. For a repeatable test, use a version tag, for example the one of the [quick start](https://docs.victoriametrics.com/victorialogs/quickstart/).
* `otelfwd` cannot send extra HTTP headers. The VictoriaLogs headers `VL-Stream-Fields`, `VL-Msg-Field` and `VL-Ignore-Fields` are therefore not used, and the defaults apply.
* `OTLP_PUSH_TOKEN` and `OTLP_CA_FILE` are not needed for this setup.

## More

* [VictoriaLogs documentation](https://docs.victoriametrics.com/victorialogs/)
* [OpenTelemetry setup for VictoriaLogs](https://docs.victoriametrics.com/victorialogs/data-ingestion/opentelemetry/)
* [Querying VictoriaLogs](https://docs.victoriametrics.com/victorialogs/querying/) and [LogsQL](https://docs.victoriametrics.com/victorialogs/logsql/)
* [otelfwd README](../README.md)

