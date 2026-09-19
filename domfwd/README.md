# domfwd - Domino event add-in

`domfwd` is a Domino server add-in (server task) which reads the events of the Domino event queue, the same source Domino Event Monitoring uses.
For every event it builds one record in the [flat record format](../README.md#records-received-via-socket-inputs) (the OpenTelemetry log data model) and sends it to [`otelfwd`](../README.md), which pushes it to any OTLP receiver.

```text
Domino server
    │  events (Event Monitoring queue)
    ▼
  domfwd  ── one flat JSON record per event ──┬── UNIX socket / TCP 127.0.0.1 ──► otelfwd ──► OTLP receiver
 (add-in)                                     ├── domino-events.json (trace file, DOMFWD_TraceFile=1)
                                              └── direct OTLP push via libcurl (optional)
```

The name `domfwd` belongs to this add-in. The general forwarder is `otelfwd`.

## Quick start (Linux)

1. Start `otelfwd` as the Domino user. It listens on `<data>/domino/otelfwd.sock` by default and needs an OTLP endpoint:

   ```bash
   OTLP_PUSH_API_URL=http://127.0.0.1:4318/v1/logs otelfwd -nostdin
   ```

2. Load the add-in on the Domino server: `load domfwd`. It sends to the same socket by default, without any setting.
3. Look for `domfwd: Socket: Connected to unix:...` in the Domino log.

The event handling of the server has to post events to the event queue `domfwd` (`events4.nsf`). If this is not configured, the queue stays empty and nothing is forwarded. The metric `domfwd_events_received_total` and the `-vv` trace line `alive: ... events received=` show whether events arrive.

## What a record looks like

One event, as sent to `otelfwd`:

```json
{
  "time_unix_nano": "1785760554890000000",
  "observed_time_unix_nano": "1785760556110000000",
  "severity_number": 17,
  "severity_text": "ERROR",
  "body": "Database compactor error: File does not exist",
  "scope": { "name": "domino.event" },
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

The record is sent as one line. `otelfwd` passes all of it on unchanged.

## Configuration (notes.ini)

| Setting                | Description                                                                                                                                                                                                                        |
| :--------------------- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `DOMFWD_Socket`        | Optional. Where to send the records: `unix:/path/to/socket` or `tcp:127.0.0.1:4390` (loopback only), `off` to disable. Default on Linux: `unix:<data>/domino/otelfwd.sock`, the default socket of `otelfwd`. No default on Windows |
| `DOMFWD_PromFile`      | `off` disables the metrics file `<data>/domino/stats/domfwd.prom` (see Metrics). Default: on                                                                                                                                       |
| `DOMFWD_TraceFile`     | `1` writes every record also to `<data>/domino/logs/domino-events.json` (troubleshooting). Default: off                                                                                                                            |
| `DOMFWD_OtelPushURL`   | Optional direct OTLP push via libcurl. Only used in builds with libcurl support                                                                                                                                                    |
| `DOMFWD_OtelPushToken` | Bearer token for the direct push                                                                                                                                                                                                   |
| `DOMFWD_OtelCaFile`    | CA file for the direct push. Default: `<data>/cacert.pem` if it exists                                                                                                                                                             |

Start the add-in with `load domfwd`. Parameters: `-v` (verbose), `-vv` (also traces the queue and every event with printf to the stdout of the add-in process, not to the Domino console), `-e` (list the known event types and end).
Every start logs the version and the build time, for example `domfwd: Starting (Domino Event Forwarder) version 0.9.0, built Sep 18 2026 23:16:39`.

Keep the add-in loaded: add `domfwd` to `ServerTasks` in `notes.ini`. While it is not loaded the event queue does not exist, and the server logs `Error posting event to event queue 'domfwd': No such queue` for every event it wants to post.
On `tell domfwd quit` the add-in reads the queue empty before it frees it, but events of other processes can still arrive during the shutdown. The event queue `domfwd` is not the task queue `MQ$DOMFWD` of the add-in itself.

The receiving side is `otelfwd` with a matching input (by default its UNIX socket) and `OTLP_PUSH_API_URL` set. See [Socket inputs](../README.md#socket-inputs).
The trace file (`DOMFWD_TraceFile=1`) is appended to and not rotated.

The socket transport (`domfwd_socket.hpp`) never blocks the add-in:

* Records wait in a queue of at most 2000 lines / 4 MB. When it is full, the newest record is dropped and counted.
* A lost connection is detected and re-established every 5 seconds. A record which was only partly sent is sent again in full.
* Delivery has no acknowledgement. Records sent just before `otelfwd` stops can be lost.

## Metrics

The add-in writes `<data>/domino/stats/domfwd.prom` every 30 seconds and at shutdown, in the Prometheus text format for the node_exporter textfile collector. This is the same directory `otelfwd` uses for `otelfwd.prom`. The file is written to a temporary file first and then renamed, so a reader never sees a half written file. `DOMFWD_PromFile=off` in `notes.ini` disables it.

| Metric                                          | Type    | Description                                                                           |
| :---------------------------------------------- | :------ | :------------------------------------------------------------------------------------ |
| `domfwd_build_number`                           | gauge   | Version as a number: major * 10000 + minor * 100 + patch (0.9.0 is 900)               |
| `domfwd_started_timestamp_seconds`              | gauge   | Unix time when the add-in was started                                                 |
| `domfwd_uptime_seconds`                         | gauge   | Uptime                                                                                |
| `domfwd_lastupdate_timestamp_seconds`           | gauge   | Unix time of the last write of this file                                              |
| `domfwd_events_received_total`                  | counter | Events read from the Domino event queue                                               |
| `domfwd_events_skipped_total{reason="own"}`     | counter | Events generated by `domfwd` itself (its own console output), not forwarded           |
| `domfwd_events_dropped_total{reason="version"}` | counter | Events with an event version the add-in does not know                                 |
| `domfwd_last_event_timestamp_seconds`           | gauge   | Unix time of the last event received (its own events excluded), 0 if none since start |
| `domfwd_socket_sent_total`                      | counter | Records sent to the forwarder                                                         |
| `domfwd_socket_dropped_total`                   | counter | Records dropped because the send queue was full                                       |
| `domfwd_socket_rejected_total`                  | counter | Records refused by the sender (empty or containing a line break)                      |
| `domfwd_socket_connects_total`                  | counter | Connections established. A fast growing value means a flapping connection             |
| `domfwd_socket_queued`                          | gauge   | Records waiting to be sent                                                            |
| `domfwd_socket_connected`                       | gauge   | 1 if connected to the forwarder                                                       |

The `domfwd_socket_*` metrics are only written if a socket is configured.

The counters show where records get lost. If `domfwd_events_received_total` grows but `domfwd_socket_sent_total` does not, the problem is inside the add-in or at the socket. If `domfwd_socket_sent_total` matches the lines `otelfwd` accepted on its socket (`otelfwd_socket_lines_total{result="accepted"}`) but fewer are pushed, look at `otelfwd` and the receiver.

A quiet server can legitimately have no events for a long time, so `domfwd_last_event_timestamp_seconds` alone is not a good alert. Use it together with the counters and `domfwd_socket_connected`.

## Build

The add-in needs the Domino C API toolkit. It cannot be built in CI.

Linux (Domino build environment, `LOTUS` and `Notes_ExecDirectory` set as usual):

```bash
make                        # socket transport only, no libcurl dependency (default)
make USE_CURL=1             # adds the direct OTLP push via the libcurl of Domino
make CURL_LIBS=-lcurl       # if the link does not find the curl_* functions through the Domino libraries
```

Windows (Visual Studio developer prompt, Domino toolkit paths set as usual):

```bat
nmake /f mswin64.mak                # with the direct OTLP push via libcurl (default)
nmake /f mswin64.mak USE_CURL=0     # socket transport only
```

`mk.cmd` builds and copies the result to `d:\lotus\...`. Adjust the paths in it for your machine.

The libcurl of Domino itself is used for the direct push. On Windows the `curl_*` exports of `nnotes.dll` are imported through `libcurl-x64.def`, so no separate DLL is needed.
The add-in also uses extended event functions of the Domino libraries which are not part of the public toolkit headers. They are imported through `domfwd_ext.def` and declared in `domfwd.cpp`.

## Status

* Linux: built with the Domino build environment and run on a live Domino server (WSL). Events are read from the event queue, sent over the UNIX socket to `otelfwd` (default socket) and pushed to an OTLP receiver. The metrics file is written.
* The socket sender is also tested on its own against `otelfwd` (reconnect, partial sends, full queue, TCP and Unix socket).
* Windows: compiled with Visual Studio 2022 (both `USE_CURL` variants). **Not yet verified:** a live run on Windows, and the TCP transport on a live server. Windows has no default socket target.

## Files

| File                | Description                                                    |
| :------------------ | :------------------------------------------------------------- |
| `domfwd.cpp`        | The add-in                                                     |
| `domfwd_socket.hpp` | Non-blocking socket sender (no Domino API, no libcurl, no STL) |
| `makefile`          | Linux build                                                    |
| `mswin64.mak`       | Windows build                                                  |
| `mk.cmd`            | Windows build and copy                                         |
| `domfwd_ext.def`    | Import definition of the extended event functions              |
| `libcurl-x64.def`   | Import definition of the `curl_*` functions in `nnotes.dll`    |
