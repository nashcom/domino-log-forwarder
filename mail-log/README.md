# MailLog

A small C++ class which builds one OTel log record out of the sender, the recipients, the subject and the other meta data of a mail
message, and returns it as one line of JSON. It has no other dependency than the C++ standard library: no rapidjson, no libcurl,
nothing else. Copy this folder, or the two files `mail_log.hpp` and `mail_log.cpp`.

This module is not used by `otelfwd` or `domfwd` yet. It is a building block for a caller which has a mail message and wants to
turn it into a log record, and it can be tested on its own.

**The schema is defined in [SCHEMA.md](SCHEMA.md), not here.** That document is the only place which says what each attribute
is, its type, its meaning, when to send it and what "absent" means, independent of any language. This class is one implementation
of it, and this README only describes the C++ API: which method writes which attribute, and how the class behaves.

## Use

```cpp
#include "mail_log.hpp"

MailLog Log;

Log.SetHostName    ("mail01.example.com");   // the mail server, not necessarily the machine otelfwd runs on
Log.SetServiceName ("postfix");
Log.SetFrom        ("sender@example.com");
Log.AddTo          ("recipient@example.com");
Log.SetSubject     ("Hello World");
Log.SetSize        (4096);
Log.SetMessageId   ("<abc@example.com>");
Log.SetBody        ("Delivered to recipient@example.com");
Log.SetSeverity    (9, "INFO");

std::string Record = Log.Build();     // one line of JSON, no new line at the end

Send (Record);                        // to a Unix or TCP socket of otelfwd, to a file, wherever it is needed
```

`Build()` can be called again and again without changing the object. `Clear()` empties it, so the same object can build the next
record without being constructed again. `Build()` does not add a new line at the end: the sender of the socket
(`domfwd_durable.hpp`'s sender, or any of its own) adds one.

This is what the exact code above produces (`SetTime` was not called, so there is no `time_unix_nano`):

```json
{"scope":{"name":"mail-log","version":"0.1.0"},"resource":{"host.name":"mail01.example.com","service.name":"postfix"},"severity_number":9,"severity_text":"INFO","body":"Delivered to recipient@example.com","attributes":{"email.from.address":"sender@example.com","email.to.addresses":["recipient@example.com"],"email.subject":"Hello World","email.message.id":"<abc@example.com>","email.size_bytes":4096}}
```

The structure of the record (`scope`, `resource`, `time_unix_nano`, ..., `attributes`) is in
[SCHEMA.md, Record structure](SCHEMA.md#record-structure). One thing to know when the record goes to `otelfwd`: its socket inputs
replace, and do not merge, a partial `resource` with their own default, so set everything about the mail server together, see
[SCHEMA.md, Resource](SCHEMA.md#resource).

## A complete record

This is record 4 of `./mail_log_sample` (see [Sample](#sample-generating-mail-log-records)): every field of the class in one
record, for a message which the server `mail02` received and delivered. It is shown pretty-printed, `Build()` returns it on one
line. What each attribute means is in [SCHEMA.md](SCHEMA.md); this section only shows how they come out.

```json
{
  "scope": { "name": "mail-log", "version": "0.1.0" },
  "resource": {
    "host.name": "mail02.example.com",
    "service.name": "exim",
    "service.namespace": "accounting",
    "service.instance.id": "mail02-1",
    "cloud.region": "eu-central-1"
  },
  "time_unix_nano": "1790241300000000000",
  "observed_time_unix_nano": "1790241303000000000",
  "severity_number": 9,
  "severity_text": "INFO",
  "body": "Delivered to kunde@example.com",
  "attributes": {
    "email.from.address": "buchhaltung@example.com",
    "email.to.addresses": ["kunde@example.com"],
    "email.cc.addresses": ["buchhaltung-archiv@example.com"],
    "email.bcc.addresses": ["controlling@example.com"],
    "email.envelope.from.address": "buchhaltung@example.com",
    "email.subject": "Rechnung für Bestellung #42",
    "email.message.id": "<3.1758000000@example.com>",
    "email.queue.id": "4X1abc-000004",
    "email.event": "delivery",
    "email.direction": "inbound",
    "email.header.date": "Thu, 24 Sep 2026 09:15:00 +0000",
    "email.header.x_invoice_number": "RE-2026-0042",
    "email.crypto.protocol.name": "pgp",
    "tls.protocol.name": "tls",
    "tls.protocol.version": "1.3",
    "tls.cipher": "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256",
    "tls.server.subject": "CN=mail02.example.com, O=Example",
    "tls.server.issuer": "CN=Example CA",
    "tls.server.hash.sha256": "0687F666A054EF17A08E2F2162EAB4CBC0D265E1D7875BE74BF3C712CA92DAF0",
    "tls.server.not_before": "2026-01-01T00:00:00.000Z",
    "tls.server.not_after": "2027-01-01T00:00:00.000Z",
    "tls.client.subject": "CN=mail-client.example.net",
    "tls.client.issuer": "CN=Example CA",
    "tls.client.hash.sha256": "9E393D93138888D288266C2D915214D1D1CCEB2A9E393D93138888D288266C2D",
    "tls.client.not_before": "2026-03-01T00:00:00.000Z",
    "tls.client.not_after": "2027-03-01T00:00:00.000Z",
    "client.address": "203.0.113.10",
    "server.address": "mail02.example.com",
    "network.local.address": "192.0.2.20",
    "network.peer.address": "198.51.100.7",
    "email.smtp.helo": "mail-client.example.net",
    "email.smtp.response.enhanced_status_code": "2.6.0",
    "email.smtp.response.text": "Message accepted for delivery",
    "email.spf.result": "pass",
    "email.spf.domain": "example.com",
    "email.dkim.result": "pass",
    "email.dkim.domain": "example.com",
    "email.dkim.selector": "selector1",
    "email.dmarc.result": "pass",
    "email.spam.result": "ham",
    "email.spam.engine.name": "Rspamd",
    "email.virus.result": "clean",
    "email.virus.engine.name": "ClamAV",
    "email.action": "deliver",
    "email.reason": "SPF/DKIM/DMARC pass, no policy matched",
    "email.policy.name": "Default-Inbound",
    "email.policy.id": "POL-0001",
    "email.classification": "confidential",
    "email.auto_submitted": "no",
    "email.size_bytes": 53500,
    "email.attachments": [
      {
        "name": "Rechnung_42.pdf",
        "size_bytes": 51302,
        "sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08",
        "content_type": "application/pdf"
      }
    ],
    "email.envelope.recipients": [
      {
        "address": "kunde@example.com",
        "status": "delivered",
        "code": 250,
        "enhanced_status_code": "2.0.0",
        "text": "Message accepted for delivery"
      },
      { "address": "buchhaltung-archiv@example.com", "status": "delivered" },
      { "address": "controlling@example.com" }
    ],
    "client.port": 51234,
    "server.port": 25,
    "network.local.port": 25,
    "network.peer.port": 41022,
    "email.smtp.response.code": 250,
    "email.signed": true,
    "email.encrypted": true,
    "tls.established": true,
    "email.spam.score": 0.1,
    "email.virus.checked": true
  }
}
```

Things to see in it:

* **Where the fields are:** the resource says which mail server produced the record, the time fields, severity and body are the record
  itself, and everything about the message is under `attributes`.
* **The order of the attributes:** the text attributes are in the order the sample sets them. `email.size_bytes`, the attachments,
  the envelope recipients, the ports, the SMTP reply code, the booleans and the spam score always come last, in this fixed order:
  the class writes them from their own members, see [The rules](#the-rules). A reader must not depend on the order.
* **Arrays are always arrays:** `email.to.addresses` has one address and is still an array, and so are the recipients and the
  attachments.
* **Absent is not there:** a recipient with no status is only `{ "address": ... }`, and the record has no `email.virus.threat.name`
  because nothing was found. The three-state booleans (`email.signed`, `email.encrypted`, `tls.established`,
  `email.virus.checked`) are there because the producer knew the answer.
* **Numbers:** `email.spam.score` is `0.1` (a double, and a whole number would be `3.0`), the ports and the sizes are integers,
  and the times are strings.

## Methods and attributes

Which method writes which attribute. What an attribute means is in [SCHEMA.md](SCHEMA.md), under the group named in the first
column. "Set" replaces the value, "Add" adds one more (see [The rules](#the-rules)).

| Schema group                                                                               | Method                                                                                                                                   | Attribute                                                                                                            | Kind                                                                                                        |
| :----------------------------------------------------------------------------------------- | :--------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------------------------------------------------------------------- | :---------------------------------------------------------------------------------------------------------- |
| [Resource (OpenTelemetry attributes)](SCHEMA.md#resource)                                  | `SetHostName (Name)`                                                                                                                     | `host.name`                                                                                                          | Set                                                                                                         |
|                                                                                            | `SetServiceName (Name)`                                                                                                                  | `service.name`                                                                                                       | Set                                                                                                         |
|                                                                                            | `SetServiceNamespace (Namespace)`                                                                                                        | `service.namespace`                                                                                                  | Set                                                                                                         |
|                                                                                            | `SetServiceInstanceId (Id)`                                                                                                              | `service.instance.id`                                                                                                | Set                                                                                                         |
|                                                                                            | `SetResourceAttribute (Key, Value)`                                                                                                      | whatever `Key` is                                                                                                    | Set, used exactly as given: the escape hatch for a resource fact none of the above cover                    |
| [Record structure](SCHEMA.md#record-structure)                                             | `SetBody (Text)`                                                                                                                         | `body`                                                                                                               | Set. See [Body](#the-body)                                                                                  |
|                                                                                            | `SetSeverity (Number, Text)`                                                                                                             | `severity_number`, `severity_text`                                                                                   | Set                                                                                                         |
|                                                                                            | `SetTime (TimeUnixNano)`                                                                                                                 | `time_unix_nano`                                                                                                     | Set                                                                                                         |
|                                                                                            | `SetObservedTime (TimeUnixNano)`                                                                                                         | `observed_time_unix_nano`                                                                                            | Set                                                                                                         |
| [Message](SCHEMA.md#message-attributes)                                                    | `SetFrom (Address)`                                                                                                                      | `email.from.address`                                                                                                 | Set                                                                                                         |
|                                                                                            | `AddTo (Address)`                                                                                                                        | `email.to.addresses`                                                                                                 | Add, always an array                                                                                        |
|                                                                                            | `AddCc (Address)`                                                                                                                        | `email.cc.addresses`                                                                                                 | Add, always an array                                                                                        |
|                                                                                            | `AddBcc (Address)`                                                                                                                       | `email.bcc.addresses`                                                                                                | Add, always an array                                                                                        |
|                                                                                            | `SetSubject (Subject)`                                                                                                                   | `email.subject`                                                                                                      | Set                                                                                                         |
|                                                                                            | `SetMessageId (Id)`                                                                                                                      | `email.message.id`                                                                                                   | Set                                                                                                         |
|                                                                                            | `SetQueueId (Id)`                                                                                                                        | `email.queue.id`                                                                                                     | Set                                                                                                         |
|                                                                                            | `SetEvent (Event)`                                                                                                                       | `email.event`                                                                                                        | Set                                                                                                         |
|                                                                                            | `SetDirection (Direction)`                                                                                                               | `email.direction`                                                                                                    | Set                                                                                                         |
|                                                                                            | `SetSize (Bytes)`                                                                                                                        | `email.size_bytes`                                                                                                   | Set, a number                                                                                               |
|                                                                                            | `AddAttachment (Name, Size, Sha256, ContentType)`                                                                                        | `email.attachments`                                                                                                  | Add, one object per call. Only `Name` is required                                                           |
|                                                                                            | `AddHeader (Name, Value)`                                                                                                                | `email.header.<name>`                                                                                                | Add. The name is lower case, anything which is not a letter, a digit or an underscore becomes an underscore |
| [Envelope](SCHEMA.md#envelope-attributes)                                                  | `SetEnvelopeFrom (Address)`                                                                                                              | `email.envelope.from.address`                                                                                        | Set                                                                                                         |
|                                                                                            | `SetEnvelopeFromEmpty ()`                                                                                                                | `email.envelope.from.address` as `""`                                                                                | Set, on purpose empty: a bounce's `MAIL FROM:<>`                                                            |
|                                                                                            | `AddRecipient (Address, Status, Code, EnhancedStatus, Text)`                                                                             | `email.envelope.recipients`                                                                                          | Add, one object per call. Only `Address` is required, `AddRecipient (Address)` is enough                    |
| [Message protection](SCHEMA.md#message-protection-attributes)                              | `SetSigned (Value)`                                                                                                                      | `email.signed`                                                                                                       | Set, a boolean, always sent once called                                                                     |
|                                                                                            | `SetEncrypted (Value)`                                                                                                                   | `email.encrypted`                                                                                                    | Set, a boolean, always sent once called                                                                     |
|                                                                                            | `SetCryptoProtocol (Protocol)`                                                                                                           | `email.crypto.protocol.name`                                                                                         | Set                                                                                                         |
| [Connection (OpenTelemetry attributes)](SCHEMA.md#connection-attributes)                   | `SetClientAddress (Address)` / `SetClientPort (Port)`                                                                                    | `client.address` / `client.port`                                                                                     | Set                                                                                                         |
|                                                                                            | `SetServerAddress (Address)` / `SetServerPort (Port)`                                                                                    | `server.address` / `server.port`                                                                                     | Set                                                                                                         |
|                                                                                            | `SetNetworkLocalAddress (Address)` / `SetNetworkLocalPort (Port)`                                                                        | `network.local.address` / `network.local.port`                                                                       | Set                                                                                                         |
|                                                                                            | `SetNetworkPeerAddress (Address)` / `SetNetworkPeerPort (Port)`                                                                          | `network.peer.address` / `network.peer.port`                                                                         | Set                                                                                                         |
|                                                                                            | `SetTlsEstablished (Value)`                                                                                                              | `tls.established`                                                                                                    | Set, a boolean, always sent once called                                                                     |
|                                                                                            | `SetTlsProtocolName (Name)`                                                                                                              | `tls.protocol.name`                                                                                                  | Set                                                                                                         |
|                                                                                            | `SetTlsProtocolVersion (Version)`                                                                                                        | `tls.protocol.version`                                                                                               | Set                                                                                                         |
|                                                                                            | `SetTlsCipher (Cipher)`                                                                                                                  | `tls.cipher`                                                                                                         | Set                                                                                                         |
|                                                                                            | `SetTlsClientSubject`, `SetTlsClientIssuer`, `SetTlsClientSha256`, `SetTlsClientNotBefore`, `SetTlsClientNotAfter` (each takes one text) | `tls.client.subject`, `tls.client.issuer`, `tls.client.hash.sha256`, `tls.client.not_before`, `tls.client.not_after` | Set. The certificate the client presented (mutual TLS), dates as ISO 8601 text                              |
|                                                                                            | `SetTlsServerSubject`, `SetTlsServerIssuer`, `SetTlsServerSha256`, `SetTlsServerNotBefore`, `SetTlsServerNotAfter` (each takes one text) | `tls.server.subject`, `tls.server.issuer`, `tls.server.hash.sha256`, `tls.server.not_before`, `tls.server.not_after` | Set. The certificate the server presented                                                                   |
| [SMTP session](SCHEMA.md#smtp-session-attributes)                                          | `SetSmtpHelo (Helo)`                                                                                                                     | `email.smtp.helo`                                                                                                    | Set                                                                                                         |
|                                                                                            | `SetSmtpResponseCode (Code)`                                                                                                             | `email.smtp.response.code`                                                                                           | Set, a number                                                                                               |
|                                                                                            | `SetSmtpEnhancedStatus (Status)`                                                                                                         | `email.smtp.response.enhanced_status_code`                                                                           | Set                                                                                                         |
|                                                                                            | `SetSmtpResponseText (Text)`                                                                                                             | `email.smtp.response.text`                                                                                           | Set                                                                                                         |
| [Spam](SCHEMA.md#spam-attributes)                                                          | `SetSpamScore (Score)`                                                                                                                   | `email.spam.score`                                                                                                   | Set, a `double`, always sent once called (not NaN or infinity)                                              |
|                                                                                            | `SetSpamResult (Result)`                                                                                                                 | `email.spam.result`                                                                                                  | Set                                                                                                         |
|                                                                                            | `SetSpamEngine (Engine)`                                                                                                                 | `email.spam.engine.name`                                                                                             | Set                                                                                                         |
| [Virus](SCHEMA.md#virus-attributes)                                                        | `SetVirusChecked (Value)`                                                                                                                | `email.virus.checked`                                                                                                | Set, a boolean, always sent once called                                                                     |
|                                                                                            | `SetVirusResult (Result)`                                                                                                                | `email.virus.result`                                                                                                 | Set                                                                                                         |
|                                                                                            | `SetVirusName (Name)`                                                                                                                    | `email.virus.threat.name`                                                                                            | Set                                                                                                         |
|                                                                                            | `SetVirusEngine (Engine)`                                                                                                                | `email.virus.engine.name`                                                                                            | Set                                                                                                         |
| [Authentication](SCHEMA.md#authentication-attributes)                                      | `SetSpfResult (Result)` / `SetSpfDomain (Domain)`                                                                                        | `email.spf.result` / `email.spf.domain`                                                                              | Set                                                                                                         |
|                                                                                            | `SetDkimResult (Result)` / `SetDkimDomain (Domain)` / `SetDkimSelector (Selector)`                                                       | `email.dkim.result` / `email.dkim.domain` / `email.dkim.selector`                                                    | Set                                                                                                         |
|                                                                                            | `SetDmarcResult (Result)`                                                                                                                | `email.dmarc.result`                                                                                                 | Set. There is no `SetDmarcDomain`, see the schema                                                           |
| [Classification, action and policy](SCHEMA.md#classification-action-and-policy-attributes) | `SetClassification (Classification)`                                                                                                     | `email.classification`                                                                                               | Set                                                                                                         |
|                                                                                            | `SetAutoSubmitted (Value)`                                                                                                               | `email.auto_submitted`                                                                                               | Set                                                                                                         |
|                                                                                            | `SetAction (Action)`                                                                                                                     | `email.action`                                                                                                       | Set                                                                                                         |
|                                                                                            | `SetReason (Reason)`                                                                                                                     | `email.reason`                                                                                                       | Set                                                                                                         |
|                                                                                            | `SetPolicyName (Name)` / `SetPolicyId (Id)`                                                                                              | `email.policy.name` / `email.policy.id`                                                                              | Set                                                                                                         |

## The rules

These are the behaviours of the class. The rules of the schema itself (absent values, arrays, three-state booleans, UTF-8) are in
[SCHEMA.md](SCHEMA.md#requirement-levels-and-absent-values) and [SCHEMA.md, Rules](SCHEMA.md#rules).

* **Set replaces, Add collects.** A `Set...` method replaces the value it has. An `Add...` method can be called more than once,
  and the values are kept in the order they were added.
* **An empty text, or a number or time of 0 or less, is ignored.** It never replaces a value which was set before. That keeps a caller
  which does not have a field (an empty `Reply-To`, a subject the caller could not decode) from wiping out a value which was
  set earlier by mistake. To clear a value which was already set, build a new object or call `Clear()`. The exceptions are
  `SetEnvelopeFromEmpty()`, which sends an empty string on purpose, and `SetSigned()`, `SetEncrypted()`, `SetTlsEstablished()`,
  `SetVirusChecked()` and `SetSpamScore()`, where `false` or `0` is a real value and is always sent once the method is called at
  all: there is no way to say "not known" other than not calling them. A NaN or infinite score is not sent, and as the call takes
  effect it replaces an earlier score: the last call counts.
* **The caller passes UTF-8.** The class does not check it, and it does not parse an address or a header, or fold or unfold a
  header line: that is the caller's job. It only stores what it is given and writes it as a JSON string, with `"`, `\` and the
  control characters (0x00-0x1F) escaped. The bytes of a UTF-8 character (0x80 and above) are written as they are.
* **Severity is a pair.** `SetSeverity (Number, Text)` takes a number from 1 to 24 (OpenTelemetry's severity numbers); any other
  number is ignored, also with a text. The number and the text are one pair: a new call replaces both, so `SetSeverity (17, "")`
  after `SetSeverity (9, "INFO")` leaves the number 17 with no text.
* **A header which has its own method is not sent as a header.** `AddHeader ("Message-ID", ...)` would put the message id into
  the record twice, as `email.message.id` and as `email.header.message_id`. The class does not stop a caller from doing it: use
  `SetFrom`, `AddTo`, `AddCc`, `AddBcc`, `SetSubject`, `SetMessageId` and `SetAutoSubmitted` for those.
* **Pass `""`, not `0`, for a text which is not given.** A literal `0` is a null pointer to the compiler, and `AddRecipient ("a@x", 0)`
  builds a `std::string` from it, which is undefined behaviour. The boolean setters refuse a string literal at compile time:
  `SetSigned ("false")` would be `true`.
* **Not thread safe.** One object is for one thread, like a `std::string`.
* **The order of the attributes.** The attributes appear in the order their key was first used, whichever method used it, except
  these, which are always added last, in this fixed order: `email.size_bytes`, `email.attachments`,
  `email.envelope.recipients`, `client.port`, `server.port`, `network.local.port`, `network.peer.port`,
  `email.smtp.response.code`, `email.signed`, `email.encrypted`, `tls.established`, `email.spam.score` and
  `email.virus.checked`. The schema does not depend on this order, it is how the class writes them.
* **`email.spam.score` is written with `std::to_chars`**, so it keeps every digit and always has a point as the decimal
  separator. That needs g++ 11 or newer.

### The body

Call `SetBody()` for every record if you can. A log viewer shows the body as the line (VictoriaLogs shows it as `_msg`). Use the
original log line if your program parsed one, else a short sentence of its own. If you set none, `Build()` makes a short summary
of what is there, so the line is not empty. What it contains, and what it never contains (the subject, the recipients' addresses),
is defined in [SCHEMA.md, Body](SCHEMA.md#body). A body you set is used as it is.

## Files

| File                     | Content                                                                                 |
| :----------------------- | :-------------------------------------------------------------------------------------- |
| `SCHEMA.md`              | The schema, independent of this class: the only place where the attributes are defined  |
| `mail_log.hpp`           | The class, with the description of its behaviour                                        |
| `mail_log.cpp`           | The implementation                                                                      |
| `mail_log_unit_test.cpp` | The unit test                                                                           |
| `mail_log_sample.cpp`    | A sample program: builds ten example records and prints each as one line, see below     |
| `makefile`               | `make test` builds and runs the unit test (and the sample, so it cannot go out of date) |

## Test

```bash
make test
```

The test prints one line per check. It covers the empty record and the scope (always the first field), the resource, one line per
text setter from a table (an empty text is ignored, a second value replaces a `Set` and is kept by an `Add`), the numbers, the
booleans and the score (a second value replaces the first, `false` replaces `true`, a number of 0 or less is ignored, the
biggest int64), the severity (1 to 24, the number and the text are one pair), the fixed order of `Build()`, the header and the
envelope addresses kept apart, the recipients with and without a status, attachments, headers (name normalization, two names
which become the same key, the raw `Date` alongside `SetTime`), the message protection and the connection (explicit `false`
against not set, the TLS certificates, `client.*` against `network.*`), the SMTP session, spam and virus (including that
`email.virus.checked` only answers whether a scan happened), the authentication results, the default body, the escaping in every
place a value is written (also a NUL, a DEL and a byte which is not valid UTF-8), the spam score formatting (always a valid JSON
number with a point or an exponent, which reads back as the same double, no locale decimal comma), one record with every setter of
the class in it with the whole string pinned, and `Clear()` and a repeated `Build()`.

## Sample: generating mail log records

```bash
make
./mail_log_sample
./mail_log_sample 20
```

`./mail_log_sample` prints ten records, one per line. `./mail_log_sample COUNT` (above with 20) prints COUNT simple synthetic
records instead, for a quick check of volume. The ten records are:

| Record | Situation                                        | What it shows                                                                                                                                                                                                                                                |
| :----- | :----------------------------------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| 0      | A plain delivery                                 | A resource (the mail server, not the machine of otelfwd), the envelope matching the headers, one recipient with a status inside it (`delivered`), a size and a severity                                                                                      |
| 1      | A bounce                                         | The empty envelope sender `MAIL FROM:<>` (`SetEnvelopeFromEmpty`), the header `X-Failed-Recipients`, a body which is the raw error text, severity ERROR                                                                                                      |
| 2      | A message with several recipients                | Three `To`, one `Cc`, and a `Bcc` recipient which is only in the envelope and in no header, a classification                                                                                                                                                 |
| 3      | A message with headers and an attachment         | `Reply-To` and `X-Mailer` as headers, an attachment with its SHA-256 and content type, a signature (S/MIME) with `email.encrypted` left unset, which is "not known" and not `false`                                                                          |
| 4      | A complete record                                | Every field of the class in one record, except `SetEnvelopeFromEmpty` and `SetVirusName` (cases 1 and 5): a UTF-8 subject, the raw `Date` header, both TLS certificates, a gateway in front of the client, SPF/DKIM/DMARC, spam and virus, action and policy |
| 5      | A virus gateway quarantines an infected message  | The virus result, the threat name and the engine, the action with its reason and policy, and no `SetBody`: the record has the default body                                                                                                                   |
| 6      | An inbound message rejected in the SMTP dialogue | The SMTP reply (550 5.7.1) and the policy which decided it, failed SPF and DMARC, and real `false` values: no TLS, not signed, and `email.virus.checked` `false` because it was rejected before the scan                                                     |
| 7      | One message, the recipient which was delivered   | Recipients with different outcomes are one record each: this one has the same queue id and message id as record 8, a status `delivered`, the action `deliver`                                                                                                |
| 8      | One message, the recipient which bounced         | The other record of the same message: the status `bounced` with 550 5.1.1, the action `reject` and its reason, severity ERROR                                                                                                                                |
| 9      | Mail sent out and deferred                       | The remote server's certificate has expired (`tls.server.*`), the logical server next to the socket endpoint (`network.peer.*`), the action `defer` with the reason and the MTA-STS policy, no SMTP reply                                                    |

Each record is one line on stdout, in the same form `Build()` returns (no new line added by the class, added by this program the
same way a caller sending it over a socket would). That means the output can be piped straight into a receiver which reads the
flat record format, for example `otelfwd`'s socket input, to see a mail record arrive there for real:

```bash
./mail_log_sample | nc -N -U /local/notesdata/domino/otelfwd.sock
```

`otelfwd` needs `OTLP_PUSH_API_URL` set, and a UNIX socket listening at that path (the default one, or `OTELFWD_UNIX_SOCKET`,
see the main README, [Socket inputs](../README.md#socket-inputs)).
