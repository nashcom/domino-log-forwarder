# The mail-log schema

**Version 0.1.0. Status: Development.**

This document defines the attributes of one log record describing one email message, or one outcome for it. It is independent of
any programming language, log pipeline or mail product. The C++ class in this folder ([README.md](README.md)) is one
implementation of it; another implementation should follow this document, not the C++ source.

The format follows the conventions of the OpenTelemetry semantic conventions: attributes are grouped by namespace, each table
lists the attribute, its type, its meaning, examples and a requirement level. Attributes which OpenTelemetry already defines
(`host.*`, `service.*`, `client.*`, `server.*`, `network.*`, `tls.*`) are used with their OpenTelemetry names and meaning and are
collected in [Generic OpenTelemetry attributes](#generic-opentelemetry-attributes). Every attribute in the `email.*` namespace has the stability **Development** until version `1.0.0`: it can still
be renamed or removed.

- [Record structure](#record-structure)
- [Scope](#scope)
- [Body](#body)
- [Requirement levels and absent values](#requirement-levels-and-absent-values)
- [Naming principle](#naming-principle)
- [Generic OpenTelemetry attributes](#generic-opentelemetry-attributes)
  - [Resource](#resource)
  - [Connection attributes](#connection-attributes)
- [Email attributes](#email-attributes)
  - [Message attributes](#message-attributes)
  - [Envelope attributes](#envelope-attributes)
  - [Message protection attributes](#message-protection-attributes)
  - [SMTP session attributes](#smtp-session-attributes)
  - [Spam attributes](#spam-attributes)
  - [Virus attributes](#virus-attributes)
  - [Authentication attributes](#authentication-attributes)
  - [Classification, action and policy attributes](#classification-action-and-policy-attributes)
- [Object types](#object-types)
- [Rules](#rules)
- [One record per outcome](#one-record-per-outcome)
- [Privacy and cardinality](#privacy-and-cardinality)
- [Reserved](#reserved)
- [Versioning](#versioning)

## Record structure

A record is one JSON object on one line, in the flat record format `otelfwd` reads from its socket inputs (see the main
[README](../README.md#records-received-via-socket-inputs)). That format is a transport detail and not part of this schema: the
same attributes can be carried in an OTLP log record. Everything in this document is either a top-level field of the record or
an entry of its `attributes` (or `resource`) object.

A transport does not have to keep the type of every attribute. In particular the arrays (`string[]`) and the arrays of objects
(`map[]`) of this schema reach a backend as they are only if the transport supports them: `otelfwd` currently sends an array or an
object as a JSON text in a string value. The schema defines the record as written by a producer, and a consumer which reads it
through such a transport parses the text.

| Field                     | Type   | Description                                                                                                                           | Requirement Level |
| :------------------------ | :----- | :------------------------------------------------------------------------------------------------------------------------------------ | :---------------- |
| `scope`                   | map    | Which schema produced the record, see [Scope](#scope)                                                                                 | Always present    |
| `resource`                | map    | Which mail server produced the record, see [Resource](#resource)                                                                      | Recommended       |
| `time_unix_nano`          | string | When the event happened, nanoseconds since the epoch, written as a string                                                             | Recommended       |
| `observed_time_unix_nano` | string | When the record was observed (read, received), nanoseconds since the epoch, written as a string                                       | Recommended       |
| `severity_number`         | int    | The severity as OpenTelemetry defines it: 1 trace, 5 debug, 9 info, 13 warn, 17 error, 21 fatal; a number outside 1 to 24 is not sent | Recommended       |
| `severity_text`           | string | The severity as the producer calls it, for example `INFO`                                                                             | Recommended       |
| `body`                    | string | The text of the log line, see [Body](#body)                                                                                           | Recommended       |
| `attributes`              | map    | The attributes defined in the sections below                                                                                          | Recommended       |

## Scope

Every record has a `scope`, whether or not anything else was set. It says which schema produced the record and at what version,
not what the message was. A consumer uses it to tell which rules a record follows.

| Attribute | Type   | Description                                                                                                                                                                              | Examples   | Requirement Level |
| :-------- | :----- | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :--------- | :---------------- |
| `name`    | string | The name of the schema                                                                                                                                                                   | `mail-log` | Always present    |
| `version` | string | The version of this document, and of the implementation which wrote the record: an implementation keeps its own version in step with the version of this document it was written against | `0.1.0`    | Always present    |

```json
"scope": { "name": "mail-log", "version": "0.1.0" }
```

## Body

`body` is the text of the log line. Log viewers show it as the line itself, so a producer should set one for every record: the
original log line if it parsed one, otherwise a short sentence of its own.

If the producer sets none, an implementation of this schema builds a default from what is there, so the line is not empty. The
default is a short, Postfix-like summary, each part only if the record has it, in this order: `email.event`, `email.action`,
`from=<envelope sender>` (`from=<>` for a bounce; the header `From` as it is, without brackets, if there is no envelope
sender), `message-id=<email.message.id>` and `nrcpt=<number of recipients>` (the envelope recipients if there are any, else the
`To`, `Cc` and `Bcc` values together). For example: `scanned quarantine from=<attacker@example.net> message-id=<m@x> nrcpt=1`.
It never contains the subject (it can be long and can hold personal data) or the recipients' addresses. A body which the
producer set is used as it is, and with nothing to summarize there is no body.

## Requirement levels and absent values

Every attribute of this schema is optional: a producer sends what it knows. The requirement level says how a producer should
treat an attribute which it does know.

| Level                   | Meaning                                                                                                                                                                                      |
| :---------------------- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| Always present          | The producer must send it. Only `scope`, `scope.name` and `scope.version`                                                                                                                    |
| Recommended             | The producer should send it when it knows the value                                                                                                                                          |
| Opt-In                  | The producer sends it only when its operator has asked for it, because it can carry personal, confidential or high-cardinality data. See [Privacy and cardinality](#privacy-and-cardinality) |
| Required (in an object) | Required inside the entry of an object type, such as the `address` of a recipient                                                                                                            |

A value which was never set is **absent**, so a consumer uses its own default, or knows that nothing was said about it. Absent
is different from a value in exactly these cases, where the meaning is defined per attribute and repeated in the tables:

* `email.envelope.from.address` has three states: absent (unknown), a non-empty address, and an **empty string**, the
  explicitly known null reverse-path `MAIL FROM:<>` of a bounce.
* `email.signed`, `email.encrypted`, `tls.established` and `email.virus.checked` are booleans with three states: absent (not
  known or not reported), `false` (the producer knows it is false) and `true` (the producer knows it is true). `false` is a real,
  common value and is always sent when known.
* `email.spam.score` of `0` (and a negative score) is a real value and is sent when known.

For every other attribute, an empty string, or a number of 0 or less (a negative size, port, reply code or time is never valid),
means "not set" and is not sent.

## Naming principle

A concept which is not specific to email uses an existing, generic attribute name from the OpenTelemetry semantic conventions,
if one exists for it, instead of an `email.*` one. `email.*` is reserved for what is genuinely about email: an address, a header,
an attachment, a mail-specific protocol result. This way the schema composes with the wider OpenTelemetry vocabulary, and a
query, alert or dashboard built on a generic attribute (for example "any connection using TLS 1.0") keeps working for email
records. A generic attribute is therefore never nested under `email.*`: `tls.established`, not `email.tls.established`.

OpenTelemetry has no `email.*` namespace, so the `email.*` attributes are defined here. They follow the OpenTelemetry naming
rules: a name for a single value is singular (`email.from.address`), a name for an array is plural (`email.to.addresses`), and an
attribute is never at the same time a value and the prefix of another attribute (`email.action` and `email.reason`, not
`email.action.reason`), because some backends cannot map both. The generic attributes below were checked against the OpenTelemetry
registry, not assumed from memory.

SMTP itself is specific to email. An SMTP-only fact (the HELO identity, the SMTP reply) is therefore `email.smtp.*`, in the same
group as `email.spf.*`, `email.dkim.*` and `email.dmarc.*`, whereas the connection which carries it is generic.

## Generic OpenTelemetry attributes

**Every attribute in this section is a standard OpenTelemetry semantic-convention attribute. This schema does not define or
change any of them.** Their name, type and meaning are the ones OpenTelemetry gives them, and the tables only say how they apply
to an email record and how they are marked here (requirement level, examples). They are not in the `email.` namespace because
they are not specific to email (see [Naming principle](#naming-principle)). The authoritative definition is the OpenTelemetry
registry, and where this document and the registry ever differ, the registry wins:

| Attributes               | OpenTelemetry registry                                                                                                                                             |
| :----------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| `host.name`, `service.*` | [host](https://opentelemetry.io/docs/specs/semconv/registry/attributes/host/), [service](https://opentelemetry.io/docs/specs/semconv/registry/attributes/service/) |
| `client.*`               | [client](https://opentelemetry.io/docs/specs/semconv/registry/attributes/client/)                                                                                  |
| `server.*`               | [server](https://opentelemetry.io/docs/specs/semconv/registry/attributes/server/)                                                                                  |
| `network.*`              | [network](https://opentelemetry.io/docs/specs/semconv/registry/attributes/network/)                                                                                |
| `tls.*`                  | [tls](https://opentelemetry.io/docs/specs/semconv/registry/attributes/tls/)                                                                                        |

They are the attributes of the resource which produced the record, and of the connection which carried the message. Of the TLS
attributes only the ones useful for a mail record are listed: the OpenTelemetry registry has more (the raw certificate and its
chain, MD5 and SHA-1 fingerprints, JA3 hashes, the ciphers a client offered), which a producer can add under their own names.

### Resource

`resource` describes which mail server produced the record. It does not describe the message, and it is not necessarily the
machine of whatever transports the record. All of these are the generic OpenTelemetry resource attributes.

| Attribute             | Type   | Description                                              | Examples              | Requirement Level |
| :-------------------- | :----- | :------------------------------------------------------- | :-------------------- | :---------------- |
| `host.name`           | string | The host name of the mail server                         | `mail01.example.com`  | Recommended       |
| `service.name`        | string | The mail system itself                                   | `postfix`, `exchange` | Recommended       |
| `service.namespace`   | string | A grouping of the mail system, if the producer has one   | `accounting`          | Recommended       |
| `service.instance.id` | string | Which instance, for example in a cluster of mail servers | `mail02-1`            | Recommended       |

A producer can add any other generic resource attribute it has under its own existing name (for example `cloud.region`); this
document does not list every one that could apply. The `resource` is present only if the producer knows at least one of them.

**`otelfwd`'s socket inputs do not merge a partial `resource` with their own default resource.** A record which sends any
`resource` field replaces the whole default for that record, not only the field it sent. A producer which sets, say, only
`host.name` therefore also loses `otelfwd`'s own `service.name` for that record. A producer which uses `resource` should set
everything about the mail server that it knows.

### Connection attributes

The SMTP connection which carried the message. These are the generic OpenTelemetry attributes, not `email.*`. `client.*` and
`server.*` describe the logical parties by their role in the connection, and are not related to the direction of the mail: the
client is the side which opened the connection. `network.*` describes the actual TCP socket endpoints, which differ from the
logical parties behind a proxy, a gateway or a load balancer. Do not perform a reverse lookup just to fill `client.address` or
`server.address`: an IP address is a valid value.

| Attribute                | Type    | Description                                                                                                                                                                                                                  | Examples                                                           | Requirement Level |
| :----------------------- | :------ | :--------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :----------------------------------------------------------------- | :---------------- |
| `client.address`         | string  | The address of the SMTP client: a name if the producer has one, otherwise the IP address                                                                                                                                     | `sender.example.com`, `203.0.113.10`                               | Recommended       |
| `client.port`            | int     | The port of the SMTP client. `0` or less is not a port and is not sent                                                                                                                                                       | `51234`                                                            | Recommended       |
| `server.address`         | string  | The address of the SMTP server, the same idea as `client.address`                                                                                                                                                            | `mx.example.net`                                                   | Recommended       |
| `server.port`            | int     | The port of the SMTP server                                                                                                                                                                                                  | `25`                                                               | Recommended       |
| `network.local.address`  | string  | The address of this side of the actual TCP connection, normally an IP address                                                                                                                                                | `192.0.2.20`                                                       | Recommended       |
| `network.local.port`     | int     | The port of this side of the actual TCP connection                                                                                                                                                                           | `25`                                                               | Recommended       |
| `network.peer.address`   | string  | The address of the endpoint directly connected to this one, normally an IP address. Behind a proxy or gateway this is the proxy, not the original client                                                                     | `198.51.100.7`                                                     | Recommended       |
| `network.peer.port`      | int     | The port of the directly connected endpoint                                                                                                                                                                                  | `41022`                                                            | Recommended       |
| `tls.established`        | boolean | Whether the connection was encrypted (STARTTLS). Absent: not known. `false` and `true` are both real values                                                                                                                  | `true`                                                             | Recommended       |
| `tls.protocol.name`      | string  | The TLS protocol. Not a fixed list here; OpenTelemetry gives these as its examples                                                                                                                                           | `tls`, `ssl`                                                       | Recommended       |
| `tls.protocol.version`   | string  | The negotiated protocol version                                                                                                                                                                                              | `1.2`, `1.3`                                                       | Recommended       |
| `tls.cipher`             | string  | The negotiated cipher suite                                                                                                                                                                                                  | `TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256`                            | Recommended       |
| `tls.client.subject`     | string  | The distinguished name of the subject of the certificate the client presented. Only there if the client offered one (mutual TLS). Which side is the client follows the role in the connection, not the direction of the mail | `CN=partner.example.org, O=Example`                                | Opt-In            |
| `tls.client.issuer`      | string  | The distinguished name of the issuer of that certificate                                                                                                                                                                     | `CN=Example CA`                                                    | Opt-In            |
| `tls.client.hash.sha256` | string  | The SHA-256 fingerprint of the DER-encoded client certificate, as hex                                                                                                                                                        | `0687F666A054EF17A08E2F2162EAB4CBC0D265E1D7875BE74BF3C712CA92DAF0` | Recommended       |
| `tls.client.not_before`  | string  | When the client certificate becomes valid, as ISO 8601 text                                                                                                                                                                  | `2026-01-01T00:00:00.000Z`                                         | Recommended       |
| `tls.client.not_after`   | string  | When the client certificate stops being valid, as ISO 8601 text                                                                                                                                                              | `2027-01-01T00:00:00.000Z`                                         | Recommended       |
| `tls.server.subject`     | string  | The distinguished name of the subject of the certificate the server presented. For mail sent out this is the remote mail server's certificate                                                                                | `CN=mx.example.net, O=Example`                                     | Opt-In            |
| `tls.server.issuer`      | string  | The distinguished name of the issuer of that certificate                                                                                                                                                                     | `CN=Example CA`                                                    | Opt-In            |
| `tls.server.hash.sha256` | string  | The SHA-256 fingerprint of the DER-encoded server certificate, as hex                                                                                                                                                        | `0687F666A054EF17A08E2F2162EAB4CBC0D265E1D7875BE74BF3C712CA92DAF0` | Recommended       |
| `tls.server.not_before`  | string  | When the server certificate becomes valid, as ISO 8601 text                                                                                                                                                                  | `2026-01-01T00:00:00.000Z`                                         | Recommended       |
| `tls.server.not_after`   | string  | When the server certificate stops being valid, as ISO 8601 text. An expired certificate is a common reason for a failed or deferred delivery under MTA-STS or DANE                                                           | `2027-01-01T00:00:00.000Z`                                         | Recommended       |

## Email attributes

Everything in the `email.` namespace. These attributes are defined by this schema, in groups.

### Message attributes

The message as it is in its headers. These are header values and are distinct from the SMTP envelope (see
[Envelope attributes](#envelope-attributes)): a mailing list, an alias or a Bcc makes them differ.

| Attribute             | Type                | Description                                                                                                                                                                                                                                                                                                        | Examples                                                                                  | Requirement Level |
| :-------------------- | :------------------ | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :---------------------------------------------------------------------------------------- | :---------------- |
| `email.from.address`  | string              | The header `From` (RFC 5322), as it is in the header: it can include a display name. Not the envelope sender                                                                                                                                                                                                       | `alice@example.com`, `CEO <ceo@example.com>`                                              | Recommended       |
| `email.to.addresses`  | string[]            | The header `To`. Always an array, also with one address                                                                                                                                                                                                                                                            | `["bob@example.com"]`                                                                     | Recommended       |
| `email.cc.addresses`  | string[]            | The header `Cc`. Always an array                                                                                                                                                                                                                                                                                   | `["archive@example.com"]`                                                                 | Recommended       |
| `email.bcc.addresses` | string[]            | The header `Bcc`, if the producer knows it: it is usually not in a delivered copy. Always an array                                                                                                                                                                                                                 | `["controlling@example.com"]`                                                             | Recommended       |
| `email.subject`       | string              | The header `Subject`                                                                                                                                                                                                                                                                                               | `Project update`                                                                          | Recommended       |
| `email.message.id`    | string              | The header `Message-ID`, with its angle brackets, as it is in the header. Distinct from the queue id                                                                                                                                                                                                               | `<1.1758000000@example.com>`                                                              | Recommended       |
| `email.queue.id`      | string              | The mail system's own identifier of the message, the id which ties several records of the same message together over its life. The mail system's own queue id if it has one, otherwise one the producer makes up                                                                                                   | `4X1abc-000001`                                                                           | Recommended       |
| `email.event`         | string              | Which point of the message's life this record is about. Free text, not a fixed list; whatever the producer's own stages are called. Not the same as `email.action`: an event says what occurrence the record describes, an action says what the mail system decided                                                | `received`, `scanned`, `delivery`, `bounce`, `quarantined`                                | Recommended       |
| `email.direction`     | string              | The direction of the message as the producer's mail system sees it. Free text, not a fixed list                                                                                                                                                                                                                    | `inbound`, `outbound`, `relay`                                                            | Recommended       |
| `email.size_bytes`    | int                 | The size of the message in bytes                                                                                                                                                                                                                                                                                   | `4096`                                                                                    | Recommended       |
| `email.attachments`   | map[]               | The attachments, one object per attachment, see [Attachment](#attachment). Always an array, also with one attachment. Metadata only, never the content                                                                                                                                                             | see below                                                                                 | Recommended       |
| `email.header.<name>` | string, or string[] | Any other header. `<name>` is the header name in lower case, with everything which is not a letter, a digit or an underscore replaced by an underscore. A header which occurs more than once is an array. The headers which have an attribute above are not sent this way (the value would be in the record twice) | `email.header.reply_to`: `support@example.com`, `email.header.received`: `["...", "..."]` | Opt-In            |

`Date` is the one header which can be both: it is `time_unix_nano` of the record (a producer which parses the header calls that
the time of the event), and it can also be kept as `email.header.date` with its original text, since the parsed and the raw
form can both be useful.

### Envelope attributes

The SMTP envelope (RFC 5321), as opposed to the headers: the sender is `MAIL FROM`, the recipients are `RCPT TO`.

| Attribute                     | Type   | Description                                                                                                                                                                                                                                                                                                                                           | Examples                                 | Requirement Level |
| :---------------------------- | :----- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :--------------------------------------- | :---------------- |
| `email.envelope.from.address` | string | The SMTP envelope sender, the reverse-path of `MAIL FROM`. Three states: absent (not known), an address, or an **empty string** for the null reverse-path `MAIL FROM:<>` of a bounce, which is not the same as the attribute being absent                                                                                                             | `alice@example.com`, `""` (empty string) | Recommended       |
| `email.envelope.recipients`   | map[]  | The SMTP envelope recipients and, optionally, what became of each one, one object per recipient, see [Recipient](#recipient). Always an array, also with one recipient. Can differ from the header recipients: a list expands to more recipients than the header shows, and a `Bcc` recipient has an envelope recipient with usually no header at all | see below                                | Recommended       |

### Message protection attributes

Whether the message content itself is signed or encrypted. This is completely separate from the transport encryption of the
connection, `tls.*` in [Connection attributes](#connection-attributes): most mail is encrypted in transit and not signed.

| Attribute                    | Type    | Description                                                                                                 | Examples       | Requirement Level |
| :--------------------------- | :------ | :---------------------------------------------------------------------------------------------------------- | :------------- | :---------------- |
| `email.signed`               | boolean | Whether the message is cryptographically signed. Absent: not known. `false` and `true` are both real values | `true`         | Recommended       |
| `email.encrypted`            | boolean | Whether the message content is encrypted. Same three states as `email.signed`                               | `false`        | Recommended       |
| `email.crypto.protocol.name` | string  | The format of the signature or encryption, if the producer knows it. Free text, not a fixed list            | `smime`, `pgp` | Recommended       |

### SMTP session attributes

Facts of the SMTP protocol which have no generic OpenTelemetry attribute. They describe what happened on the wire, and are
distinct from the decision the mail system took about the message (see
[Classification, action and policy attributes](#classification-action-and-policy-attributes)).

| Attribute                                  | Type   | Description                                                                                                                                                                   | Examples                  | Requirement Level |
| :----------------------------------------- | :----- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------ | :---------------- |
| `email.smtp.helo`                          | string | The HELO/EHLO identity the client gave. Not interpreted as a verified host name. It is "the HELO domain" which `email.spf.domain` refers to when the envelope sender is empty | `mail-client.example.net` | Recommended       |
| `email.smtp.response.code`                 | int    | The 3-digit SMTP reply code the server sent for the message. `0` or less is not a reply code and is not sent                                                                  | `250`, `550`              | Recommended       |
| `email.smtp.response.enhanced_status_code` | string | The RFC 3463 enhanced status code of the reply. A string, because `5.7.1` is a structured value and not a number. Not checked against the RFC                                 | `2.0.0`, `5.7.1`          | Recommended       |
| `email.smtp.response.text`                 | string | The text of the SMTP reply                                                                                                                                                    | `Relay access denied`     | Recommended       |

### Spam attributes

| Attribute                | Type   | Description                                                                                                                                                                                                                                                                                                                                                                                                                                              | Examples                 | Requirement Level |
| :----------------------- | :----- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :----------------------- | :---------------- |
| `email.spam.score`       | double | The score the scanning engine gave, as it gave it: scores are not normalized between products, and `email.spam.engine.name` says how to read it. `0` and negative scores are real values. NaN and infinity have no JSON number and are not sent. Written with every digit of the value and always with a point as the decimal separator. A whole number is written with `.0` (`3.0`), so it stays a double for a reader which types a number by its text | `3.2`, `0.0`, `-1.5`     | Recommended       |
| `email.spam.result`      | string | The verdict of the scanner. Free text, not a fixed list                                                                                                                                                                                                                                                                                                                                                                                                  | `spam`, `ham`            | Recommended       |
| `email.spam.engine.name` | string | The scanner or product which produced the score and the result. Useful when two scanners disagree                                                                                                                                                                                                                                                                                                                                                        | `SpamAssassin`, `Rspamd` | Recommended       |

### Virus attributes

| Attribute                 | Type    | Description                                                                                                                                                                                            | Examples                          | Requirement Level |
| :------------------------ | :------ | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :-------------------------------- | :---------------- |
| `email.virus.checked`     | boolean | Whether the message was scanned for malware at all, not what the scan found. Absent: the producer does not say. `false`: the producer knows for certain that no scan happened. `true`: a scan happened | `true`                            | Recommended       |
| `email.virus.result`      | string  | The verdict of the scanner, separate from `email.virus.checked`: `true` and `clean` together are the normal clean message. Free text, not a fixed list                                                 | `clean`, `infected`, `suspicious` | Recommended       |
| `email.virus.threat.name` | string  | The name of what was found, if `email.virus.result` says something was: the signature or threat name as the scanner gives it                                                                           | `Win32/Whatever`                  | Recommended       |
| `email.virus.engine.name` | string  | The scanner or product which performed the scan. Not the same as the threat name                                                                                                                       | `ClamAV`                          | Recommended       |

### Authentication attributes

Results of the sender authentication checks. The results are free text: this schema does not reject a value which the relevant
standard does not define, but recommends the standard's own values where there are some.

| Attribute             | Type   | Description                                                                                                                                                                                     | Examples                                                                | Requirement Level |
| :-------------------- | :----- | :---------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- | :---------------------------------------------------------------------- | :---------------- |
| `email.spf.result`    | string | The result of the SPF check (RFC 7208)                                                                                                                                                          | `pass`, `fail`, `softfail`, `neutral`, `none`, `temperror`, `permerror` | Recommended       |
| `email.spf.domain`    | string | The domain the SPF check actually ran against: the envelope sender's domain, or the HELO domain if the envelope sender is empty. Not necessarily the domain of `email.from.address`             | `example.com`                                                           | Recommended       |
| `email.dkim.result`   | string | The result of the DKIM check (RFC 6376)                                                                                                                                                         | `pass`, `fail`, `none`                                                  | Recommended       |
| `email.dkim.domain`   | string | The `d=` signing domain of the DKIM signature which was checked                                                                                                                                 | `example.com`                                                           | Recommended       |
| `email.dkim.selector` | string | The `s=` selector of that signature. With the domain it identifies the exact signing key, which matters when a domain has more than one                                                         | `selector1`                                                             | Recommended       |
| `email.dmarc.result`  | string | The result of the DMARC evaluation (RFC 7489). There is no DMARC domain: DMARC checks whether the domain of the header `From` aligns with the SPF and DKIM domains and has no domain of its own | `pass`, `fail`, `none`                                                  | Recommended       |

### Classification, action and policy attributes

What the mail system decided, why, and which rule was responsible. These stay separate concepts: the verdict of a scanner
(`email.spam.result`, `email.virus.result`) is not an action, an action is not a reason, and none of them is the SMTP reply.

| Attribute              | Type   | Description                                                                                                                                        | Examples                                                 | Requirement Level |
| :--------------------- | :----- | :------------------------------------------------------------------------------------------------------------------------------------------------- | :------------------------------------------------------- | :---------------- |
| `email.classification` | string | The classification of the message. Free text, not a fixed list; the four-tier scheme is a common choice                                            | `public`, `internal`, `confidential`, `restricted`       | Recommended       |
| `email.auto_submitted` | string | The value of the header `Auto-Submitted` (RFC 3834), which tells automated systems not to reply to the message. Some mail systems use other values | `auto-generated`, `auto-replied`, `no`                   | Recommended       |
| `email.action`         | string | What the mail system did with the message. Not a verdict, not a reason. Free text: there is no standard for it and every gateway has its own words | `deliver`, `quarantine`, `reject`, `discard`, `redirect` | Recommended       |
| `email.reason`         | string | Why the action happened: which rule or check triggered it, in the producer's own words                                                             | `Attachment matched AV signature`                        | Recommended       |
| `email.policy.name`    | string | The policy or rule which decided the action, if the mail system has one                                                                            | `AV-Block-Executables`                                   | Recommended       |
| `email.policy.id`      | string | The identifier of that policy or rule                                                                                                              | `POL-1042`                                               | Recommended       |

## Object types

Two attributes are arrays of objects. Everything in an object except the field marked Required is optional, and a field which was
not given is left out of the object, not sent empty.

### Attachment

An entry of `email.attachments`. An attachment without a name is not an attachment and is not added.

| Field          | Type   | Description                                                                                                                                                                           | Examples              | Requirement Level |
| :------------- | :----- | :------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------ | :-------------------- | :---------------- |
| `name`         | string | The file name of the attachment. Free text, potentially personal and high cardinality                                                                                                 | `invoice.pdf`         | Required          |
| `size_bytes`   | int    | The size of the attachment in bytes. `0` or less is left out                                                                                                                          | `45231`               | Recommended       |
| `sha256`       | string | The SHA-256 hash of the content, as a hex string. Only this algorithm has a field of its own; another goes into an `email.header.<name>` until there is a need for more than one here | `9f86d081884c7d65...` | Recommended       |
| `content_type` | string | The MIME type, if the producer has it. Often not known, and then left out                                                                                                             | `application/pdf`     | Recommended       |

```json
"email.attachments": [
  { "name": "invoice.pdf", "size_bytes": 45231, "sha256": "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08", "content_type": "application/pdf" }
]
```

### Recipient

An entry of `email.envelope.recipients`. It is the only place where a status for one recipient lives: the header recipients are
plain arrays of addresses. A producer which has no status per recipient sends only the addresses. A recipient without an address
is not a recipient and is not added.

| Field                  | Type   | Description                                                       | Examples                                       | Requirement Level |
| :--------------------- | :----- | :---------------------------------------------------------------- | :--------------------------------------------- | :---------------- |
| `address`              | string | The recipient address as it was given in `RCPT TO`                | `bob@example.com`                              | Required          |
| `status`               | string | What became of this recipient. Free text, not a fixed list        | `delivered`, `deferred`, `bounced`, `rejected` | Recommended       |
| `code`                 | int    | The SMTP reply code for this recipient. `0` or less is left out   | `550`                                          | Recommended       |
| `enhanced_status_code` | string | The RFC 3463 enhanced status code for this recipient. Not checked | `5.1.1`                                        | Recommended       |
| `text`                 | string | The text of the SMTP reply for this recipient                     | `user unknown`                                 | Recommended       |

```json
"email.envelope.recipients": [
  { "address": "bob@example.com", "status": "delivered", "code": 250, "enhanced_status_code": "2.0.0", "text": "OK" },
  { "address": "dora@example.com", "status": "bounced", "code": 550, "enhanced_status_code": "5.1.1", "text": "user unknown" },
  { "address": "compliance-bcc@example.com" }
]
```

The message-level `email.smtp.response.*` attributes describe the reply for the message as a whole; these fields describe one
recipient.

## Rules

* **Absent, not empty.** A value which was never set is left out of the record, see
  [Requirement levels and absent values](#requirement-levels-and-absent-values).
* **Arrays.** The address attributes (`email.to.addresses`, `email.cc.addresses`, `email.bcc.addresses`), `email.envelope.recipients`
  and `email.attachments` are always arrays, also with one entry, so a consumer has one type to query whatever the number. The
  values are in the order they were added. A header (`email.header.<name>`) is a string with one value and an array with more.
* **No mail parsing.** Nothing here parses an address, folds or unfolds a header line, or reads a MIME structure. A producer gives
  the schema what it has already extracted; the schema only says where each fact goes and how it is written.
* **UTF-8.** Every string is UTF-8. An implementation takes care of the characters a JSON string requires escaped, so a producer
  does not.

## One record per outcome

A record describes one outcome for one message, or, with the envelope, one recipient of it. A gateway whose recipients get
different outcomes for the same message (one delivered, one rejected, one quarantined) can send one record per differing
outcome, each with its own `email.envelope.recipients` and its own `email.action`, instead of one record trying to hold several
actions at once. A producer which has a status per recipient but one action can also carry the statuses in
`email.envelope.recipients` of a single record. `email.queue.id` ties records of the same message together, and `email.event` says
which point of the message's life each one is.

The same goes for more than one scanner: a message scanned by two engines is two records with the same `email.queue.id`, each
with its own `email.spam.engine.name` or `email.virus.engine.name` and its own result, not one record with a list of engines.

## Privacy and cardinality

Email metadata is personal data in most jurisdictions, and much of it has an unbounded number of distinct values. A producer
should record only what its operational and security purposes need, and what its privacy policy allows. The attributes below
deserve particular care:

* **Addresses:** `email.from.address`, `email.to.addresses`, `email.cc.addresses`, `email.bcc.addresses`,
  `email.envelope.from.address` and the addresses in `email.envelope.recipients` identify people. `email.bcc.addresses` is
  usually not in a delivered copy on purpose.
* **`email.subject`:** free text written by a person, it can hold anything.
* **`email.header.<name>`:** an arbitrary header can carry personal, confidential, authentication or tracking data. These are
  Opt-In: a producer does not collect every header of a message by default.
* **`email.message.id` and `email.queue.id`:** unique per message, so a very high cardinality. Good for finding one message,
  wrong as a label or an index of a metrics backend.
* **Attachment names (`email.attachments`):** free text, personal or confidential, and high cardinality. A hash and a size say
  more about an attachment than its name does. Only metadata is recorded, never the content.
* **TLS certificate subject and issuer (`tls.client.subject`, `tls.client.issuer`, `tls.server.subject`, `tls.server.issuer`):** distinguished
  names, which can name a person or an organisation. They are Opt-In. The fingerprint and the dates identify a certificate without
  naming anyone.
* **`email.smtp.response.text` and `email.reason`:** free text from a remote server or a policy, which often contains an
  address or a name. `email.policy.name` and `email.policy.id` are usually low-cardinality and safe.

The default `body` (see [Body](#body)) avoids the subject and the addresses of the recipients for the same reason, and only
counts them. Which attributes to send is the producer's decision; this schema only defines what each one means when it is sent.

## Reserved

Named here so a future version does not collide with what a producer might already be sending under a different key, but not
part of this version:

| Likely key                             | What it would hold                                                           | Why it is not in this version                                                                                                                                                                          |
| :------------------------------------- | :--------------------------------------------------------------------------- | :----------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------------- |
| an authenticated user                  | the SMTP AUTH identity, if any                                               | likely an existing generic OpenTelemetry attribute for an end user, not `email.*`; needs checking against the current OpenTelemetry semantic conventions before a name is picked                       |
| `email.dlp.result`, `email.dlp.policy` | the outcome of a data-loss-prevention check, and which DLP policy decided it | mail-specific, but there is no concrete producer to design against yet; `email.policy.name`, `email.policy.id`, `email.action` and `email.reason` already cover most of what a DLP result needs to say |

## Versioning

`scope.version` follows the version of this document. Adding an attribute is a minor version increase. Renaming or removing one,
or changing what an existing one means, is a major version increase, so a consumer can tell whether it needs to change how it
reads a record. `0.x` means the schema is still being shaped and any change can be breaking; `1.0.0` is for once the attributes
above are considered settled.

