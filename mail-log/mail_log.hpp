
/* MailLog - builds one OTel log record (the flat record format of otelfwd, one JSON object per line) out of the sender, the
   recipients, the subject and other meta data of a mail message. It has no other dependency than the C++ standard library: no
   rapidjson, no libcurl, nothing else. Copy this folder, or the two files mail_log.hpp and mail_log.cpp.

   This is meant to be a schema several programs can share, not only this one class: see SCHEMA.md for the field list on its
   own, independent of the C++ API. Build() stamps every record with the scope of that schema ("mail-log", a version), so a
   consumer, or a second implementation in another language, can tell which rules a record follows.

   How it works
   ------------

   - Every "Set..." method replaces the value it has. Every "Add..." method can be called more than once, and the values are kept
     in the order they were added. An empty text, or a number of 0 or less, is ignored everywhere in this class: it never
     replaces a value which was set before. That is the only way to clear a value which was already set: build a new object, or
     call Clear().

   - Build() returns one JSON object, the record: scope, resource, time_unix_nano, observed_time_unix_nano, severity_number,
     severity_text, body and attributes, in the flat record format otelfwd reads from its socket inputs (see the main README,
     "Records received via socket inputs"). scope is always there: it says which schema produced the record, and at what
     version, whether or not any other field was set. Everything else which was never set is left out, so the receiver uses its
     own default (the README: "All fields are optional"). Build() does not add a new line at the end: the sender of the socket
     adds one.

   - Attributes: the sender is "email.from.address", the recipients are "email.to.addresses", "email.cc.addresses" and "email.bcc.addresses" (always a JSON array, also with
     one recipient), the subject is "email.subject", the message id is "email.message.id", the queue id is "email.queue.id", the
     direction is "email.direction" (a free text, not checked: "inbound", "outbound", "relay", whatever the caller's own mail
     system calls it), the size is "email.size_bytes", the attachments are "email.attachments" and the envelope recipients are
     "email.envelope.recipients" (a plain JSON number, and two arrays of objects, added after the other attributes, then
     client.port, server.port, network.local.port, network.peer.port, email.smtp.response.code, email.signed, email.encrypted, tls.established, email.spam.score and
     email.virus.checked if they were set, in that order). A header added with AddHeader()
     is "email.header.<name>", with its name in lower case and everything which is not a letter, a digit or an underscore
     replaced by an underscore ("Reply-To" becomes "email.header.reply_to"). A header added more than once becomes a JSON array,
     the same as a recipient. Apart from email.size_bytes, email.attachments, email.envelope.recipients, client.port, server.port, network.local.port, network.peer.port,
     email.smtp.response.code, email.signed, email.encrypted, tls.established, email.spam.score and email.virus.checked, the
     attributes appear in the order their key was first used, whichever method used it.

   - The envelope, if the caller knows it (RFC 5321: MAIL FROM and RCPT TO), is separate from the header fields above: it can
     differ from them (a mailing list, an alias, a recipient which only got the message through Bcc, which is usually not in the
     headers of the delivered copy at all). SetEnvelopeFrom() is "email.envelope.from.address", AddRecipient() is "email.envelope.recipients"
     (an array of objects, one per recipient: the address, and optionally a status of that recipient, its SMTP reply code,
     enhanced status code and reply text). SetEnvelopeFromEmpty() sends "email.envelope.from.address"
     as an empty string: the envelope sender of a bounce (a delivery status notification) is empty by design (MAIL FROM:<>), and
     that is not the same as not knowing the envelope sender at all.

   - AddAttachment(): a name, a size and, optionally, its SHA-256 hash and its content type. One entry of "email.attachments" is
     left out if the name is empty (there is no attachment then), and a size of 0, an empty hash or an empty content type are
     left out of that one entry, the same as everywhere else in this class. Only SHA-256 has a field of its own ("sha256"): one
     algorithm is enough for now, and a caller which also wants another one can add it as a header instead
     (AddHeader("X-Attachment-MD5", ...)) until there is a real need for more here.

   - Whether the message is signed and/or encrypted, and in what format: "email.signed" and "email.encrypted" (real JSON booleans,
     true or false, not a string), "email.crypto.protocol.name" is free text ("smime", "pgp", whatever the caller's own terms are),
     the same as email.direction. Unlike everywhere else in this class, false is a real, common, meaningful value for
     email.signed/email.encrypted, not "unspecified" (most mail is not signed, and that is worth saying). So there is no way to
     write "not known" other than not calling SetSigned()/SetEncrypted() at all: unlike every other Set method, they always take
     effect, there is no value which is ignored. SetSpamScore() is the same: 0 is a real, common value (a clean message), so it
     always takes effect too, except with NaN or infinite, which have no JSON number and are silently not sent.
     SetVirusChecked() follows the same false-is-real rule, but it only answers whether a scan was performed at all, not what it
     found: false means the producer knows for certain no scan happened, which is different from not knowing whether one did.
     The result of a scan, if there was one, is email.virus.result, a separate free text field: email.virus.checked=true and
     email.virus.result="clean" together are the normal clean-message combination. email.spam.result, email.virus.result,
     email.virus.threat.name, email.spam.engine.name, email.virus.engine.name, email.spf.result, email.dkim.result, email.dmarc.result, email.spf.domain, email.dkim.domain, email.dkim.selector,
     email.event, email.action, email.reason, email.policy.name, email.policy.id, email.smtp.helo, email.smtp.response.enhanced_status_code,
     email.smtp.response.text, client.address and server.address (there is no equivalent domain for DMARC: it checks
     alignment with email.spf.domain/email.dkim.domain, it has no domain of its own), email.classification and email.auto_submitted
     are all free text, the same as email.direction and email.crypto.protocol.name. SetTlsEstablished() follows the same false-is-real
     rule as SetSigned()/SetEncrypted(); tls.protocol.name, tls.protocol.version and tls.cipher are free text like the rest.

   - The SMTP connection's peer: client.address, client.port, server.address, server.port - the existing, generic OpenTelemetry
     attributes (verified against the current semantic conventions), not "email.*": a connection's peer is not specific to mail,
     the same reasoning as tls.*. client.port and server.port render as a plain JSON number, like email.size_bytes: 0 is not a
     valid port number, so it means "not set" here too. client.* and server.* are the logical parties (a name, or an IP address
     if that is all the caller has: no reverse lookup just to fill them), network.local.* and network.peer.* are the actual
     socket endpoints of the TCP connection, which differ behind a proxy, a gateway or a load balancer.
   - The SMTP session itself, genuinely mail-specific (unlike the connection's peer above, and unlike tls.*): email.smtp.helo,
     the HELO/EHLO identity the client gave - this is "the HELO domain" email.spf.domain's own description already refers to,
     with nowhere to record it until now. email.smtp.response.code, email.smtp.response.enhanced_status_code and
     email.smtp.response.text are the actual SMTP reply the server sent for this message (the 3-digit code, the RFC 3463
     enhanced status code such as "5.7.1", and the free text after it) - a different fact than email.action/email.reason, which
     are the mail system's own verdict and explanation, not the protocol-level reply it put on the wire.
     email.smtp.response.code renders as a plain JSON number: 0 is not a valid SMTP reply code, so it means "not set" here too.

   - Resource: who and where the mail message came from - not the message itself. host.name, service.name, service.namespace and
     service.instance.id are the generic OpenTelemetry names, the same ones otelfwd's own default resource uses (see the main
     README): they are not "email.*", because they are not specific to mail. SetResourceAttribute() is the escape hatch for
     anything else. "resource" is only in the record if at least one of these was set. otelfwd's socket inputs do not merge a
     record's resource with its own default: a record which sends any resource field replaces the whole thing, not only that
     field. A caller which sets, say, only SetHostName() loses otelfwd's own service.name for that record too, so it should set
     what it needs together (its own service.name for the mail system, not otelfwd's "domino").

   - One record is one outcome for one message (or, with the envelope, one recipient of it). A gateway whose recipients get
     different outcomes for the same message (one delivered, one rejected, one quarantined) sends one record per differing
     outcome, each with its own email.envelope.recipients and its own email.action - not one record trying to hold several actions at
     once (a producer which has a status per recipient can also put it into email.envelope.recipients). email.queue.id ties those records together, email.event says which point of the message's life each one is.

   - The caller passes UTF-8. The class does not check it, does not parse an address or a header, and does not fold or unfold a
     header line: that is the caller's job. It only stores what it is given and writes it as a JSON string, with the characters a
     JSON string requires escaped (", \ and the control characters 0x00-0x1F). The bytes of a UTF-8 character (0x80 and above)
     are written as they are.

   - A text parameter takes a std::string, and a literal 0 or a null pointer is a null pointer to the compiler: AddRecipient
     ("a@x", 0) or SetSeverity (9, 0) builds a std::string from a null pointer, which is undefined behaviour. Pass "" for "not
     given". The bool setters (SetSigned and the others) refuse a string literal at compile time, SetSigned ("false") would
     otherwise be true.

   - The attributes which have their own method are never also sent as a header: AddHeader ("Message-ID", ...) would put the
     message id into the record twice (email.message.id and email.header.message_id). The class does not stop a caller from doing
     it, that is the caller's job (see SCHEMA.md, "Message attributes"): use SetFrom, AddTo, AddCc, AddBcc, SetSubject,
     SetMessageId and SetAutoSubmitted for those.

   - Not thread safe: one object is for one thread, like a std::string. Clear() empties the object, so it can build the next
     record without being constructed again. */

#pragma once

#include <stdint.h>

#include <string>
#include <utility>
#include <vector>


class MailLog
{

public:

    MailLog() {}

    /* Resource: which mail server this is, and where it runs - not a header, not an attribute of the message. Empty is ignored,
       like everywhere else. See "Resource" above for the rule about otelfwd's socket inputs not merging a partial resource */
    void SetHostName          (const std::string& Name);
    void SetServiceName       (const std::string& Name);
    void SetServiceNamespace  (const std::string& Namespace);
    void SetServiceInstanceId (const std::string& Id);
    void SetResourceAttribute (const std::string& Key, const std::string& Value);

    void SetFrom (const std::string& Address);
    void AddTo   (const std::string& Address);
    void AddCc   (const std::string& Address);
    void AddBcc  (const std::string& Address);

    /* The envelope (RFC 5321), separate from the header fields above: it can differ from them (a mailing list, an alias, a
       recipient which only got the message through Bcc). An empty address is ignored, like everywhere else in this class */
    void SetEnvelopeFrom (const std::string& Address);

    /* One envelope recipient (RCPT TO), and optionally what became of it: a status in the producer's own words ("delivered",
       "deferred", "bounced", whatever it calls it), the SMTP reply code for this recipient (0: not known), its RFC 3463
       enhanced status code and the reply text. Only the address is required, the rest can be left out: a producer which has
       no status per recipient calls AddRecipient (Address). Can be called more than once. email.envelope.recipients, an array
       of objects, one per call. An empty address is ignored, so is a code of 0 or less */
    void AddRecipient (const std::string& Address, const std::string& Status = std::string(), int64_t Code = 0,
                       const std::string& EnhancedStatus = std::string(), const std::string& Text = std::string());

    /* The envelope sender is empty on purpose: a bounce (a delivery status notification) has "MAIL FROM:<>" by design. That is
       not the same as not knowing the envelope sender, so it is a method of its own, not SetEnvelopeFrom("") */
    void SetEnvelopeFromEmpty();

    void SetSubject   (const std::string& Subject);
    void SetMessageId (const std::string& Id);

    /* An id which ties several log records of the same message together (its life often produces more than one): the queue id of
       the mail system if it has one, or one the caller makes up. email.queue.id */
    void SetQueueId (const std::string& Id);

    /* Which point of the message's life this one record is about: "received", "scanned", "delivery", "bounce", whatever the
       producer's own stages are called. Free text, not checked against a fixed list. Without this, email.queue.id ties records
       together but does not say what each one represents. email.event */
    void SetEvent (const std::string& Event);

    /* Free text, not checked against a fixed list: "inbound", "outbound", "relay", whatever the caller's mail system calls it.
       email.direction */
    void SetDirection (const std::string& Direction);

    /* The size of the message in bytes, as the attribute email.size_bytes */
    void SetSize (int64_t Bytes);

    /* One attachment: its name, its size in bytes (0: not known), its SHA-256 hash as a hex string, and its content type
       (MIME type). Only the name is required, the rest can be left empty or 0. Can be called more than once. email.attachments,
       an array of objects, one per call */
    void AddAttachment (const std::string& Name, int64_t Size, const std::string& Sha256 = std::string(), const std::string& ContentType = std::string());

    /* Any other header, for example "Reply-To" or "X-Mailer". Can be called more than once for the same name: a message can have
       several headers with the same name, for example several "Received" */
    void AddHeader (const std::string& Name, const std::string& Value);

    /* Whether the message is signed / encrypted. Unlike every other Set method in this class, false is a real value here and
       always takes effect: call these only when the caller actually knows the answer, there is no way to say "not known" other
       than not calling them. email.signed, email.encrypted */
    void SetSigned    (bool Value);
    void SetSigned    (const char *) = delete;        /* SetSigned ("false") would be true: a pointer is not a bool here */
    void SetEncrypted (bool Value);
    void SetEncrypted (const char *) = delete;

    /* Free text, not checked against a fixed list, the same as SetDirection(): "smime", "pgp", whatever the caller calls it.
       email.crypto.protocol.name */
    void SetCryptoProtocol (const std::string& Protocol);

    /* The SMTP connection's own transport encryption (STARTTLS) - a different fact than SetSigned()/SetEncrypted() above,
       which are about the message content, not the connection it was carried on: a message can be encrypted in transit with
       plain content, or the reverse. These are the existing, generic OpenTelemetry TLS attributes (tls.*, verified against
       the current semantic conventions), not "email.*": TLS is not specific to mail. tls.established is a real boolean, the
       same rule as SetSigned()/SetEncrypted(): false (the connection was not encrypted) always takes effect once called.
       tls.protocol.name/tls.protocol.version/tls.cipher are free text, not enforced here either, though OpenTelemetry's own
       registry gives real example values: "tls", "1.2"/"1.3", "TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256" */
    void SetTlsEstablished     (bool Value);
    void SetTlsEstablished     (const char *) = delete;
    void SetTlsProtocolName    (const std::string& Name);
    void SetTlsProtocolVersion (const std::string& Version);
    void SetTlsCipher          (const std::string& Cipher);

    /* The certificate used for the TLS connection, as the generic OpenTelemetry tls.client.* / tls.server.* attributes: the
       client's certificate (only there if the client offered one, mutual TLS) and the server's. Which side is which follows
       the role in the connection, not the direction of the mail: for mail sent out, the server certificate is the remote
       mail server's. The subject and the issuer are distinguished names (for example "CN=mx.example.net, O=Example"), the
       hash is the SHA-256 fingerprint of the DER encoded certificate, and the two dates are ISO 8601 text
       ("2027-01-01T00:00:00.000Z"): the class does not check any of them. All free text, all optional. A subject can name a
       person or an organisation: send these only if the operator asked for them. tls.client.subject, tls.client.issuer,
       tls.client.hash.sha256, tls.client.not_before, tls.client.not_after and the same five as tls.server.* */
    void SetTlsClientSubject   (const std::string& Subject);
    void SetTlsClientIssuer    (const std::string& Issuer);
    void SetTlsClientSha256    (const std::string& Hash);
    void SetTlsClientNotBefore (const std::string& Time);
    void SetTlsClientNotAfter  (const std::string& Time);
    void SetTlsServerSubject   (const std::string& Subject);
    void SetTlsServerIssuer    (const std::string& Issuer);
    void SetTlsServerSha256    (const std::string& Hash);
    void SetTlsServerNotBefore (const std::string& Time);
    void SetTlsServerNotAfter  (const std::string& Time);

    /* The SMTP connection's peer: the existing, generic OpenTelemetry client/server attributes, not "email.*" - a connection's
       peer is not specific to mail, the same reasoning as SetTlsEstablished() above. client.address/server.address are free
       text (an IP address, or a hostname if the caller resolved one). client.port/server.port render as a plain JSON number:
       0 is not a valid port number, so it is "not set" here too, the same rule as SetSize() */
    void SetClientAddress (const std::string& Address);
    void SetClientPort    (int64_t Port);
    void SetServerAddress (const std::string& Address);
    void SetServerPort    (int64_t Port);

    /* The actual TCP connection, as opposed to the logical client/server above: network.local.address/port is this side's
       socket endpoint, network.peer.address/port is the endpoint directly connected to it (normally an IP address, and the
       port). They differ from the client.* and server.* attributes behind a proxy, a gateway or a load balancer: those say who the
       parties are, network.* says which socket carried the connection. Do not look up a hostname just to fill client.address:
       an IP address is a valid value. Same rules as above: a port of 0 or less is "not set" */
    void SetNetworkLocalAddress (const std::string& Address);
    void SetNetworkLocalPort    (int64_t Port);
    void SetNetworkPeerAddress  (const std::string& Address);
    void SetNetworkPeerPort     (int64_t Port);

    /* The HELO/EHLO identity the client gave for this SMTP session - genuinely mail-specific, unlike the connection's peer
       above: there is no generic OpenTelemetry attribute for it. This is "the HELO domain" SetSpfDomain() already refers to
       (SPF falls back to it when the envelope sender is empty). Free text. email.smtp.helo */
    void SetSmtpHelo (const std::string& Helo);

    /* The actual SMTP reply the server sent for this message - a different fact than SetAction()/SetReason() above, which are
       the mail system's own verdict and explanation, not the protocol-level reply it put on the wire: the 3-digit reply code
       (email.smtp.response.code, a plain JSON number: 0 is not a valid code, so it is "not set" here too, the same rule as
       SetSize()), the RFC 3463 enhanced status code such as "5.7.1" (email.smtp.response.enhanced_status_code, free text: this
       schema does not enforce the RFC's own vocabulary), and the free text after it (email.smtp.response.text) */
    void SetSmtpResponseCode   (int64_t Code);
    void SetSmtpEnhancedStatus (const std::string& Status);
    void SetSmtpResponseText   (const std::string& Text);

    /* A spam score from whatever scanner the caller has. 0 is a real, common value (a clean message), not "unspecified", so this
       always takes effect once called, the same as SetSigned()/SetEncrypted(): there is no way to say "not known" other than not
       calling it. NaN and infinite are the one exception: there is no JSON number for them, so they are not sent, and because
       a call always takes effect, a NaN after a valid score replaces it: the last call is the one which counts and nothing is
       sent. A whole number
       is written with ".0" (3.0), so it stays a double for a reader which types a number by its text. email.spam.score */
    void SetSpamScore (double Score);

    /* Free text, not checked against a fixed list: "spam", "ham", whatever the caller's scanner calls it. email.spam.result */
    void SetSpamResult (const std::string& Result);

    /* Which scanner produced the spam score and result: "SpamAssassin", "Rspamd", whatever the product is called. Free text,
       not checked. Useful when two scanners disagree. email.spam.engine.name */
    void SetSpamEngine (const std::string& Engine);

    /* Free text, not checked against a fixed list: "pass", "fail", "neutral", "softfail", "none", whatever the checker calls it.
       email.spf.result, email.dkim.result, email.dmarc.result */
    void SetSpfResult   (const std::string& Result);
    void SetDkimResult  (const std::string& Result);
    void SetDmarcResult (const std::string& Result);

    /* The domain the check actually ran against: SPF checks the envelope sender's domain (or the HELO domain, if the envelope
       sender is empty), DKIM checks the "d=" domain of its signature. Not the same as email.from.address's domain: a message can claim
       one domain while the check ran against another. There is no equivalent for DMARC: it checks whether the header From's
       domain aligns with these two, it has no domain of its own. email.spf.domain, email.dkim.domain */
    void SetSpfDomain  (const std::string& Domain);
    void SetDkimDomain (const std::string& Domain);

    /* The "s=" selector of the DKIM signature: together with the "d=" domain (SetDkimDomain) it identifies the exact signing
       key/configuration used, which matters when a domain has more than one (key rotation, several sending systems). Free
       text. email.dkim.selector */
    void SetDkimSelector (const std::string& Selector);

    /* Whether the message was virus-scanned at all - not what the scan found (that is SetVirusResult() below). false is a real
       value here too, the same as SetSigned()/SetEncrypted(): it means the producer knows for certain no scan happened, which
       is different from not knowing whether one did. email.virus.checked */
    void SetVirusChecked (bool Value);
    void SetVirusChecked (const char *) = delete;

    /* Free text, not checked against a fixed list: "clean", "infected", "suspicious", whatever the scanner calls it.
       email.virus.checked=true and email.virus.result="clean" together are the normal clean-message combination.
       email.virus.result */
    void SetVirusResult (const std::string& Result);

    /* The name of what was found, if email.virus.result says something was: a signature or threat name like the scanner would
       give it, for example "Win32/Whatever". Free text, not checked. email.virus.threat.name */
    void SetVirusName (const std::string& Name);

    /* Which scanner produced the virus result: "ClamAV", whatever the product is called. Free text, not checked.
       email.virus.engine.name */
    void SetVirusEngine (const std::string& Engine);

    /* Free text, not checked against a fixed list: the traditional four-tier scheme ("public", "internal", "confidential",
       "restricted") is a common choice, not enforced. email.classification */
    void SetClassification (const std::string& Classification);

    /* Free text, not checked against a fixed list: the header Auto-Submitted (RFC 3834), which tells other automated systems
       this is not human-originated mail, so they do not reply to it in turn (an autoresponder loop). "no", "auto-generated" and
       "auto-replied" are the three values the RFC itself defines; some mail systems use others. email.auto_submitted */
    void SetAutoSubmitted (const std::string& Value);

    /* Free text, not checked against a fixed list: what the mail system actually did with the message, not a verdict about it
       (that is email.spam.result/email.virus.result/... above). "quarantine", "reject", "discard", "redirect" and "deliver" are
       common, not enforced: every gateway product has its own words for this, and there is no RFC for it, unlike SPF/DKIM/
       DMARC/Auto-Submitted. email.action */
    void SetAction (const std::string& Action);

    /* Free text: why email.action happened - which rule or check actually triggered it, in whatever words the producer's own
       system uses. email.reason */
    void SetReason (const std::string& Reason);

    /* The policy or rule which decided the action, if the mail system has one, of whatever kind (a transport rule, a gateway
       policy, an AV or DLP rule): its name and its identifier. It does not have to be a mail rule and names no product. Free
       text, not checked. email.policy.name, email.policy.id */
    void SetPolicyName (const std::string& Name);
    void SetPolicyId   (const std::string& Id);

    /* The body of the record: the text of the log line. A log viewer shows it as the line. Set one for every record: the original
       log line if the caller parsed one, else a short sentence of its own. If none is set, Build() makes a short summary of what
       is there: email.event, email.action, the sender ("from=<envelope sender>", "from=<>" for a bounce, else the header From),
       "message-id=..." and "nrcpt=<number of recipients>" - for example
       "scanned quarantine from=<attacker@example.net> message-id=<m@x> nrcpt=1". Never the subject (it can be long and hold
       personal data) and never the addresses of the recipients. A body which was set is used as it is. If there is nothing to
       summarize there is no body */
    void SetBody (const std::string& Text);

    /* The severity, as OpenTelemetry defines it (1 trace, 5 debug, 9 info, 13 warn, 17 error, 21 fatal; the numbers in between
       are the steps of a level, 1 to 24 in all). A number outside 1 to 24 is unspecified: nothing is sent, also if Text is not
       empty. The number and the text are one pair: a new call replaces both, so SetSeverity (17, "") after SetSeverity (9, "INFO")
       leaves the number 17 with no text, not with the old "INFO" */
    void SetSeverity (int64_t Number, const std::string& Text);

    /* The time of the record, and the time it was observed (read, received). Nanoseconds since the epoch. Neither is sent by
       default: the receiver then uses its own default (the observed time, and the time it reads the record) */
    void SetTime         (int64_t TimeUnixNano);
    void SetObservedTime (int64_t TimeUnixNano);

    /* Empties the object: the next Build() starts from nothing, as with a newly constructed object */
    void Clear();

    /* The record, as one line of JSON, without a new line at the end */
    std::string Build() const;

private:

    /* One attribute: a key and the values it was given, in the order they were added. One value: a plain JSON string. More than
       one: a JSON array */
    typedef std::vector<std::pair<std::string, std::vector<std::string>>> AttributeList;

    /* One entry of email.envelope.recipients */
    struct Recipient
    {
        std::string Address;
        std::string Status;
        int64_t     Code = 0;         /* 0: not known */
        std::string EnhancedStatus;
        std::string Text;
    };

    /* One entry of email.attachments */
    struct Attachment
    {
        std::string Name;
        int64_t     Size = 0;         /* 0: not known */
        std::string Sha256;
        std::string ContentType;
    };

    /* Helpers, which do not need the state of an object */
    static const std::vector<std::string> *FindValues (const AttributeList& List, const std::string& Key);
    static void        SetValue          (AttributeList& retList, const std::string& Key, const std::string& Value);
    static void        AddValue          (AttributeList& retList, const std::string& Key, const std::string& Value);
    static void        AppendJsonString  (std::string& retJson, const std::string& Text);
    static void        AppendAttachment  (std::string& retJson, const Attachment& Entry);
    static void        AppendRecipient   (std::string& retJson, const Recipient& Entry);
    static void        AppendResource    (std::string& retJson, const AttributeList& Resource);
    static std::string NormalizeHeaderName (const std::string& Name);
    static bool        IsRecipientKey     (const std::string& Key);

    /* The body Build() uses if the caller set none: a short summary of what the object has. See mail_log.cpp */
    std::string BuildDefaultBody() const;

    std::string m_Body;
    int64_t     m_SeverityNumber       = 0;      /* 0: unspecified */
    std::string m_SeverityText;
    int64_t     m_TimeUnixNano         = 0;      /* 0: not set */
    int64_t     m_ObservedTimeUnixNano = 0;      /* 0: not set */
    int64_t     m_Size                 = 0;      /* 0: not set */
    int64_t     m_ClientPort           = 0;      /* 0: not set, not a valid port number */
    int64_t     m_ServerPort           = 0;      /* 0: not set, not a valid port number */
    int64_t     m_NetworkLocalPort     = 0;      /* 0: not set, not a valid port number */
    int64_t     m_NetworkPeerPort      = 0;      /* 0: not set, not a valid port number */
    int64_t     m_SmtpResponseCode     = 0;      /* 0: not set, not a valid SMTP reply code */
    bool        m_bSigned              = false;  /* only meaningful if m_bSignedSet */
    bool        m_bSignedSet           = false;
    bool        m_bEncrypted           = false;  /* only meaningful if m_bEncryptedSet */
    bool        m_bEncryptedSet        = false;
    bool        m_bTlsEstablished      = false;  /* only meaningful if m_bTlsEstablishedSet */
    bool        m_bTlsEstablishedSet   = false;
    double      m_SpamScore            = 0.0;    /* only meaningful if m_bSpamScoreSet */
    bool        m_bSpamScoreSet        = false;
    bool        m_bVirusChecked        = false;  /* only meaningful if m_bVirusCheckedSet */
    bool        m_bVirusCheckedSet     = false;

    AttributeList             m_Resource;
    AttributeList             m_Attributes;
    std::vector<Recipient>    m_Recipients;
    std::vector<Attachment>   m_Attachments;
};
