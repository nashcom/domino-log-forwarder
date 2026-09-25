
/* Unit test of the MailLog module (mail_log.cpp). A separate program which only links MailLog: no otelfwd, no network, no file.

   Build and run:  cd mail-log && make test        (or: make mail_log_unit_test && ./mail_log_unit_test. From the repository root: make test)

   Every check prints [PASS] or [FAIL]. The end of the output has a section "Failed checks" (only if there are any) and a section
   "Result" with the overall status. The exit code is 0 if every check passed and 1 if any check failed.

   Groups of tests, in the order of the output:

   - Empty record:            nothing set gives just the scope
   - Scope:                   it is always there, and it is the first field of the record
   - Resource:                host.name, service.name/namespace/instance.id, SetResourceAttribute, only present if used, right
                              after scope
   - Body, severity, time:    each field on its own, an empty text or the number/time 0 ignored, and the fixed order of Build()
   - Sender and recipients:   email.from.address, email.to.addresses, email.cc.addresses, email.bcc.addresses, always a JSON
                              array, also with one recipient, and the order of the attributes (the order a key was first used,
                              not the order the values were added)
   - Envelope:                email.envelope.from.address, SetEnvelopeFromEmpty for a bounce's null sender, and that the
                              envelope is kept apart from the header fields
   - Envelope recipients:     email.envelope.recipients: address required, status, reply code, enhanced status and text optional
   - Subject and message id:  email.subject, email.message.id
   - Queue id, event, direction: email.queue.id, email.event, email.direction
   - Headers:                 the name in lower case with an underscore for anything else, more than one value a JSON array
   - Escaping of the values:  a quote, a backslash, a new line, a tab, a control character, a UTF-8 character
   - Size:                    email.size_bytes as a plain JSON number, added after the other attributes
   - Attachments:             name, size_bytes, SHA-256 hash and content type, all but the name optional, more than one an
                              array, added after email.size_bytes
   - Signed, encrypted, TLS:  email.signed/email.encrypted/tls.established as real booleans (false is meaningful, always sent
                              if set), email.crypto.protocol.name and the tls.protocol.name/version/tls.cipher attributes as
                              free text
   - Connection and SMTP:     client/server address and port, network.local/peer address and port (generic, 0 not a valid port),
                              the TLS client and server certificate, email.smtp.helo, email.smtp.response.code/
                              enhanced_status_code/text (0 not a valid SMTP reply code), and their place in the fixed order
   - Spam and virus:          email.spam.score (a real number, 0 meaningful, NaN/infinite not sent), email.spam.result and engine,
                              email.virus.checked (a real boolean, only whether a scan happened, not what it found),
                              email.virus.result, threat name and engine, and the order of the special-position fields it sets
                              (the whole order is pinned by the full record)
   - Authentication results:  email.spf.result, email.dkim.result, email.dmarc.result, email.spf.domain, email.dkim.domain (no
                              equivalent domain for DMARC), email.dkim.selector, free text, in the order they were set
   - Classification, Auto-Submitted, action, reason, policy: all free text, reason and policy added after email.action
   - Every text setter:       one line per setter, from a table: an empty text sends nothing on a new object, never replaces a
                              value which was set, and a second value replaces (Set) or is kept next to the first (Add)
   - Default body:            no SetBody: a summary of event, action, sender, message id and number of recipients, never the
                              subject; a body which was set is used as it is
   - Extreme values:          the biggest int64 as size and time, NUL, DEL and invalid UTF-8 bytes, a body of 100000 characters
   - Second value of a number, a boolean, the score: a second value replaces the first, false replaces true
   - Severity:                1 to 24, the number and the text are one pair
   - Header names which become the same key: X-A, X_A and x.a are one key, one array
   - Negative numbers:        size, attachment size, ports, SMTP reply code, severity, times: 0 or less is "not set"
   - Escaping everywhere:     an attribute value, every value of an array, a header, the resource (also the key of
                              SetResourceAttribute), an attachment name
   - Spam score formatting:   always a valid JSON number, with a point or an exponent, which reads back as the same double, also
                              for tiny, huge and negative zero; no digits lost, no locale decimal comma (the comma check is
                              skipped, and says so, if the machine has no de_DE locale)
   - Full record:             every setter of the class in one record, the whole string pinned, so that no method can be added
                              without a test which sees it
   - Clear and Build again:   Clear() empties the object (except the scope), also after every setter has been used,
                              Build() does not change it and gives the same string every time

   The name of a check starts with what it tests, for example "to:" or "attachment:" */

#include <stdio.h>

#include <locale.h>
#include <stdint.h>

#include <charconv>
#include <limits>
#include <string>
#include <vector>

#include "mail_log.hpp"


static int g_Total  = 0;
static int g_Failed = 0;
static std::vector<std::string> g_FailedNames;

/* The scope Build() always sends first, copied from the literal in mail_log.cpp (MAIL_LOG_SCHEMA_NAME, MAIL_LOG_SCHEMA_VERSION).
   Every expected string in this file is built from it, so a change of the schema name or version only has to be made here once */
static const std::string Scope = "\"scope\":{\"name\":\"mail-log\",\"version\":\"0.1.0\"}";


static void Check (bool bCondition, const char *pszName)
{
    printf ("[%s]  %s\n", bCondition ? "PASS" : "FAIL", pszName);

    g_Total++;

    if (false == bCondition)
    {
        g_Failed++;
        g_FailedNames.push_back (pszName);
    }
}


static void Group (const char *pszTitle)
{
    std::string Line (80, '-');

    printf ("\n%s\n%s\n%s\n\n", Line.c_str(), pszTitle, Line.c_str());
}


static void TestEmpty()
{
    MailLog Log;

    Check (("{" + Scope + "}") == Log.Build(), "empty: nothing else set gives just the scope");
}


static void TestScope()
{
    MailLog Log;

    Check (("{" + Scope + "}") == Log.Build(), "scope: always there, even with nothing else set");

    Log.SetBody ("x");

    std::string Built = Log.Build();

    Check (0 == Built.compare (1, Scope.size(), Scope), "scope: the first field of the record, right after the opening brace, whatever else is set");
}


static void TestResource()
{
    MailLog Host;

    Host.SetHostName ("mail01.example.com");
    Check (("{" + Scope + ",\"resource\":{\"host.name\":\"mail01.example.com\"}}") == Host.Build(), "resource: host.name, right after scope");

    Host.SetHostName ("");
    Check (("{" + Scope + ",\"resource\":{\"host.name\":\"mail01.example.com\"}}") == Host.Build(), "resource: an empty name is ignored, the previous value stays");

    MailLog Service;

    Service.SetServiceName       ("postfix");
    Service.SetServiceNamespace  ("mail");
    Service.SetServiceInstanceId ("mail01");
    Check (("{" + Scope + ",\"resource\":{\"service.name\":\"postfix\",\"service.namespace\":\"mail\",\"service.instance.id\":\"mail01\"}}") == Service.Build(),
           "resource: service.name, service.namespace, service.instance.id, in the order they were set");

    MailLog Attribute;

    Attribute.SetResourceAttribute ("cloud.region", "eu-central-1");
    Check (("{" + Scope + ",\"resource\":{\"cloud.region\":\"eu-central-1\"}}") == Attribute.Build(), "resource: SetResourceAttribute is the escape hatch for anything else, used exactly as given");

    Attribute.SetResourceAttribute ("cloud.region", "");
    Check (("{" + Scope + ",\"resource\":{\"cloud.region\":\"eu-central-1\"}}") == Attribute.Build(), "resource: an empty value is ignored, the previous value stays");

    Attribute.SetResourceAttribute ("cloud.region", "us-east-1");
    Check (("{" + Scope + ",\"resource\":{\"cloud.region\":\"us-east-1\"}}") == Attribute.Build(), "resource: the same key again replaces the value, it does not become an array");

    MailLog Empty;

    Check (("{" + Scope + "}") == Empty.Build(), "resource: nothing set, no resource field at all - not even an empty object");

    /* resource is its own field, separate from the email.* attributes, and it comes before the record's own fields */
    MailLog Combined;

    Combined.SetHostName ("mail01.example.com");
    Combined.SetBody     ("x");
    Combined.SetFrom     ("a@b");

    Check (("{" + Scope + ",\"resource\":{\"host.name\":\"mail01.example.com\"},\"body\":\"x\",\"attributes\":{\"email.from.address\":\"a@b\"}}") == Combined.Build(),
           "resource: comes right after scope, before body and the attributes");
}


static void TestBodySeverityTime()
{
    MailLog Body;

    Body.SetBody ("hello");
    Check (("{" + Scope + ",\"body\":\"hello\"}") == Body.Build(), "body: appears as body");

    Body.SetBody ("");
    Check (("{" + Scope + ",\"body\":\"hello\"}") == Body.Build(), "body: an empty text is ignored, the previous value stays");

    MailLog Severity;

    Severity.SetSeverity (9, "INFO");
    Check (("{" + Scope + ",\"severity_number\":9,\"severity_text\":\"INFO\"}") == Severity.Build(), "severity: number and text");

    MailLog SeverityNoText;

    SeverityNoText.SetSeverity (17, "");
    Check (("{" + Scope + ",\"severity_number\":17}") == SeverityNoText.Build(), "severity: an empty text is not sent, only the number");

    MailLog SeverityZero;

    SeverityZero.SetSeverity (0, "INFO");
    Check (("{" + Scope + "}") == SeverityZero.Build(), "severity: the number 0 is ignored, the text alone is not sent");

    MailLog Time;

    Time.SetTime (1789899303123456789LL);
    Check (("{" + Scope + ",\"time_unix_nano\":\"1789899303123456789\"}") == Time.Build(), "time: sent as a string, like the other record producers of this project");

    MailLog TimeZero;

    TimeZero.SetTime (0);
    Check (("{" + Scope + "}") == TimeZero.Build(), "time: 0 is ignored, like elsewhere in this project");

    MailLog Observed;

    Observed.SetObservedTime (42);
    Check (("{" + Scope + ",\"observed_time_unix_nano\":\"42\"}") == Observed.Build(), "observed time: sent as a string");

    /* The fixed order of Build(): scope, time, observed time, severity, body, then the attributes - whatever order the methods
       were called in. Here observed time and time are set in the wrong order on purpose */
    MailLog All;

    All.SetObservedTime (2);
    All.SetTime (1);
    All.SetSeverity (9, "INFO");
    All.SetBody ("test");
    All.SetFrom ("a@b");

    Check (("{" + Scope + ",\"time_unix_nano\":\"1\",\"observed_time_unix_nano\":\"2\",\"severity_number\":9,\"severity_text\":\"INFO\",\"body\":\"test\",\"attributes\":{\"email.from.address\":\"a@b\"}}") == All.Build(),
           "order: scope, time, observed time, severity, body, then the attributes - never the order the methods were called in");
}


static void TestRecipients()
{
    MailLog From;

    From.SetFrom ("sender@example.com");
    Check (("{" + Scope + ",\"body\":\"from=sender@example.com\",\"attributes\":{\"email.from.address\":\"sender@example.com\"}}") == From.Build(), "from: email.from.address");

    From.SetFrom ("");
    Check (("{" + Scope + ",\"body\":\"from=sender@example.com\",\"attributes\":{\"email.from.address\":\"sender@example.com\"}}") == From.Build(), "from: an empty address is ignored, the previous value stays");

    MailLog To;

    To.AddTo ("x@y.com");
    Check (("{" + Scope + ",\"body\":\"nrcpt=1\",\"attributes\":{\"email.to.addresses\":[\"x@y.com\"]}}") == To.Build(), "to: one recipient is still an array, recipients are always arrays");

    To.AddTo ("z@w.com");
    Check (("{" + Scope + ",\"body\":\"nrcpt=2\",\"attributes\":{\"email.to.addresses\":[\"x@y.com\",\"z@w.com\"]}}") == To.Build(), "to: more than one recipient, in the order they were added");

    MailLog CcBcc;

    CcBcc.AddCc  ("cc@x.com");
    CcBcc.AddBcc ("bcc@x.com");
    Check (("{" + Scope + ",\"body\":\"nrcpt=2\",\"attributes\":{\"email.cc.addresses\":[\"cc@x.com\"],\"email.bcc.addresses\":[\"bcc@x.com\"]}}") == CcBcc.Build(), "cc and bcc: email.cc.addresses and email.bcc.addresses");

    MailLog Empty;

    Empty.AddTo  ("");
    Empty.AddCc  ("");
    Empty.AddBcc ("");
    Check (("{" + Scope + "}") == Empty.Build(), "to, cc, bcc: an empty address is ignored");

    /* The order of the attributes is the order a KEY was first used, not the order the values were added: email.to.addresses was used
       first (AddTo), so it comes before email.from.address (SetFrom), although email.to.addresses got a second value after that */
    MailLog Order;

    Order.AddTo ("first-to@x.com");
    Order.SetFrom ("from@x.com");
    Order.AddTo ("second-to@x.com");

    Check (("{" + Scope + ",\"body\":\"from=from@x.com nrcpt=2\",\"attributes\":{\"email.to.addresses\":[\"first-to@x.com\",\"second-to@x.com\"],\"email.from.address\":\"from@x.com\"}}") == Order.Build(),
           "attribute order: the order in which a key was first used, email.to.addresses before email.from.address here because it was used first");
}


static void TestEnvelope()
{
    MailLog EnvFrom;

    EnvFrom.SetEnvelopeFrom ("bounce-handler@example.com");
    Check (("{" + Scope + ",\"body\":\"from=<bounce-handler@example.com>\",\"attributes\":{\"email.envelope.from.address\":\"bounce-handler@example.com\"}}") == EnvFrom.Build(), "envelope from: email.envelope.from.address");

    EnvFrom.SetEnvelopeFrom ("");
    Check (("{" + Scope + ",\"body\":\"from=<bounce-handler@example.com>\",\"attributes\":{\"email.envelope.from.address\":\"bounce-handler@example.com\"}}") == EnvFrom.Build(), "envelope from: an empty address is ignored, the previous value stays");

    MailLog EnvEmpty;

    EnvEmpty.SetEnvelopeFromEmpty();
    Check (("{" + Scope + ",\"body\":\"from=<>\",\"attributes\":{\"email.envelope.from.address\":\"\"}}") == EnvEmpty.Build(),
           "envelope from: SetEnvelopeFromEmpty sends an empty string, a bounce's null sender, not \"nothing set\"");

    /* SetEnvelopeFromEmpty and SetEnvelopeFrom both replace: whichever is called last wins */
    MailLog EnvOverwrite;

    EnvOverwrite.SetEnvelopeFrom ("a@b");
    EnvOverwrite.SetEnvelopeFromEmpty();
    Check (("{" + Scope + ",\"body\":\"from=<>\",\"attributes\":{\"email.envelope.from.address\":\"\"}}") == EnvOverwrite.Build(), "envelope from: SetEnvelopeFromEmpty after SetEnvelopeFrom replaces it");

    EnvOverwrite.SetEnvelopeFrom ("c@d");
    Check (("{" + Scope + ",\"body\":\"from=<c@d>\",\"attributes\":{\"email.envelope.from.address\":\"c@d\"}}") == EnvOverwrite.Build(), "envelope from: SetEnvelopeFrom after SetEnvelopeFromEmpty replaces it too");

    MailLog Recipient;

    Recipient.AddRecipient ("bob@example.com");
    Check (("{" + Scope + ",\"body\":\"nrcpt=1\",\"attributes\":{\"email.envelope.recipients\":[{\"address\":\"bob@example.com\"}]}}") == Recipient.Build(), "envelope recipients: one recipient is an array with one object, only the address given");

    Recipient.AddRecipient ("bcc-recipient@example.com");
    Check (("{" + Scope + ",\"body\":\"nrcpt=2\",\"attributes\":{\"email.envelope.recipients\":[{\"address\":\"bob@example.com\"},{\"address\":\"bcc-recipient@example.com\"}]}}") == Recipient.Build(),
           "envelope to: more than one is a JSON array, in the order they were added - this is how a Bcc recipient (never in a header) still shows up");

    MailLog RecipientEmpty;

    RecipientEmpty.AddRecipient ("");
    Check (("{" + Scope + "}") == RecipientEmpty.Build(), "envelope to: an empty address is ignored");

    /* The envelope is kept apart from the headers: a header To and an envelope recipient can both be set, and they keep their own keys */
    MailLog Both;

    Both.AddTo ("visible@example.com");
    Both.SetEnvelopeFrom ("envelope-sender@example.com");
    Both.AddRecipient ("visible@example.com");
    Both.AddRecipient ("hidden-bcc@example.com");

    Check (("{" + Scope + ",\"body\":\"from=<envelope-sender@example.com> nrcpt=2\",\"attributes\":{\"email.to.addresses\":[\"visible@example.com\"],\"email.envelope.from.address\":\"envelope-sender@example.com\",\"email.envelope.recipients\":[{\"address\":\"visible@example.com\"},{\"address\":\"hidden-bcc@example.com\"}]}}") == Both.Build(),
           "envelope and headers: kept apart under their own keys, in the order their key was first used");
}


/* email.envelope.recipients: one object per AddRecipient, the address required, the status, reply code, enhanced status and
   reply text optional. Each is left out if not given, like an attachment's fields */
static void TestRecipientStatus()
{
    MailLog All;

    All.AddRecipient ("bob@x", "delivered", 250, "2.0.0", "OK queued");
    Check (("{" + Scope + R"(,"body":"nrcpt=1","attributes":{"email.envelope.recipients":[{"address":"bob@x","status":"delivered","code":250,"enhanced_status_code":"2.0.0","text":"OK queued"}]}})") == All.Build(),
           "recipient status: address, status, reply code, enhanced status and text, in that fixed order");

    MailLog StatusOnly;

    StatusOnly.AddRecipient ("a@x", "deferred");
    Check (("{" + Scope + R"(,"body":"nrcpt=1","attributes":{"email.envelope.recipients":[{"address":"a@x","status":"deferred"}]}})") == StatusOnly.Build(),
           "recipient status: a field which was not given is left out, not sent empty");

    MailLog CodeOnly;

    CodeOnly.AddRecipient ("a@x", "", 550);
    Check (("{" + Scope + R"(,"body":"nrcpt=1","attributes":{"email.envelope.recipients":[{"address":"a@x","code":550}]}})") == CodeOnly.Build(),
           "recipient status: a reply code alone is enough");

    MailLog Negative;

    Negative.AddRecipient ("a@x", "bounced", -5);
    Check (("{" + Scope + R"(,"body":"nrcpt=1","attributes":{"email.envelope.recipients":[{"address":"a@x","status":"bounced"}]}})") == Negative.Build(),
           "recipient status: a reply code of 0 or less is not known, it is left out like 0");

    MailLog NoAddress;

    NoAddress.AddRecipient ("", "delivered", 250);
    Check (("{" + Scope + "}") == NoAddress.Build(), "recipient status: without an address the call is ignored, whatever else was given");

    MailLog Several;

    Several.AddRecipient ("a@x", "delivered", 250);
    Several.AddRecipient ("b@x", "bounced", 550, "5.1.1", "user unknown");
    Several.AddRecipient ("c@x");
    Check (("{" + Scope + R"(,"body":"nrcpt=3","attributes":{"email.envelope.recipients":[{"address":"a@x","status":"delivered","code":250},)"
            R"({"address":"b@x","status":"bounced","code":550,"enhanced_status_code":"5.1.1","text":"user unknown"},{"address":"c@x"}]}})") == Several.Build(),
           "recipient status: one object per recipient in the order they were added, one with a status and one without side by side");

    MailLog Escaped;

    Escaped.AddRecipient ("a@x", "", 0, "", "say \"hi\"");
    Check (("{" + Scope + R"(,"body":"nrcpt=1","attributes":{"email.envelope.recipients":[{"address":"a@x","text":"say \"hi\""}]}})") == Escaped.Build(),
           "recipient status: escaped like every other text");

    /* email.envelope.recipients is added after email.attachments and before client.port, whatever order the methods were called in */
    MailLog Order;

    Order.SetClientPort (1);
    Order.AddRecipient  ("a@x");
    Order.AddAttachment ("f", 1);
    Order.SetFrom       ("z");
    Check (("{" + Scope + R"(,"body":"from=z nrcpt=1","attributes":{"email.from.address":"z","email.attachments":[{"name":"f","size_bytes":1}],"email.envelope.recipients":[{"address":"a@x"}],"client.port":1}})") == Order.Build(),
           "recipient status: email.envelope.recipients comes after email.attachments and before client.port, whatever order the methods were called in");
}


static void TestSubjectAndMessageId()
{
    MailLog Log;

    Log.SetSubject   ("Hello World");
    Log.SetMessageId ("<123@host>");

    Check (("{" + Scope + ",\"body\":\"message-id=<123@host>\",\"attributes\":{\"email.subject\":\"Hello World\",\"email.message.id\":\"<123@host>\"}}") == Log.Build(), "subject and message id: email.subject before email.message.id, as they were set");
}


static void TestQueueIdEventAndDirection()
{
    MailLog Log;

    Log.SetQueueId ("ABC123");
    Check (("{" + Scope + ",\"attributes\":{\"email.queue.id\":\"ABC123\"}}") == Log.Build(), "queue id: email.queue.id, ties several log records of the same message together");

    Log.SetQueueId ("");
    Check (("{" + Scope + ",\"attributes\":{\"email.queue.id\":\"ABC123\"}}") == Log.Build(), "queue id: an empty id is ignored, the previous value stays");

    MailLog Event;

    Event.SetEvent ("scanned");
    Check (("{" + Scope + ",\"body\":\"scanned\",\"attributes\":{\"email.event\":\"scanned\"}}") == Event.Build(), "event: email.event, which point of the message's life this record is about");

    Event.SetEvent ("");
    Check (("{" + Scope + ",\"body\":\"scanned\",\"attributes\":{\"email.event\":\"scanned\"}}") == Event.Build(), "event: an empty text is ignored, the previous value stays");

    MailLog Direction;

    Direction.SetDirection ("outbound");
    Check (("{" + Scope + ",\"attributes\":{\"email.direction\":\"outbound\"}}") == Direction.Build(), "direction: free text, not checked against a fixed list");

    Direction.SetDirection ("");
    Check (("{" + Scope + ",\"attributes\":{\"email.direction\":\"outbound\"}}") == Direction.Build(), "direction: an empty text is ignored, the previous value stays");
}


static void TestHeaders()
{
    MailLog Log;

    Log.AddHeader ("Reply-To", "r@s.com");
    Check (("{" + Scope + ",\"attributes\":{\"email.header.reply_to\":\"r@s.com\"}}") == Log.Build(), "header: the name becomes lower case, a dash becomes an underscore, and email.header. is put in front");

    Log.AddHeader ("X-Mailer", "A");
    Log.AddHeader ("X-Mailer", "B");
    Check (("{" + Scope + ",\"attributes\":{\"email.header.reply_to\":\"r@s.com\",\"email.header.x_mailer\":[\"A\",\"B\"]}}") == Log.Build(),
           "header: the same name more than once becomes a JSON array, like a recipient");

    MailLog Punctuation;

    Punctuation.AddHeader ("X-My.Header!", "v");
    Check (("{" + Scope + ",\"attributes\":{\"email.header.x_my_header_\":\"v\"}}") == Punctuation.Build(), "header: a dot and other punctuation become underscores too, not only a dash");

    MailLog Empty;

    Empty.AddHeader ("", "v");
    Empty.AddHeader ("Name", "");
    Check (("{" + Scope + "}") == Empty.Build(), "header: an empty name or an empty value is ignored");

    MailLog Date;

    Date.AddHeader ("Date", "Mon, 21 Sep 2026 10:04:56 +0000");
    Check (("{" + Scope + ",\"attributes\":{\"email.header.date\":\"Mon, 21 Sep 2026 10:04:56 +0000\"}}") == Date.Build(),
           "header: the raw Date header can still be kept as a header, next to SetTime's own parsed time_unix_nano");
}


static void TestEscaping()
{
    MailLog Quote;

    Quote.SetBody ("a \"quoted\" word");
    Check (("{" + Scope + ",\"body\":\"a \\\"quoted\\\" word\"}") == Quote.Build(), "escaping: a double quote becomes \\\"");

    MailLog Backslash;

    Backslash.SetBody ("back\\slash");
    Check (("{" + Scope + ",\"body\":\"back\\\\slash\"}") == Backslash.Build(), "escaping: a backslash becomes \\\\");

    MailLog NewlineTab;

    NewlineTab.SetBody ("line1\nline2\tend");
    Check (("{" + Scope + ",\"body\":\"line1\\nline2\\tend\"}") == NewlineTab.Build(), "escaping: a new line becomes \\n, a tab becomes \\t");

    /* A control character below 0x20 which is none of the named ones (\b \f \n \r \t) becomes \u00xx. The literals "a\x01" and
       "b" are two separate string literals concatenated by the compiler, so the hex escape does not read "b" as another hex
       digit */
    MailLog Control;

    Control.SetBody (std::string ("a\x01" "b"));
    Check (("{" + Scope + ",\"body\":\"a\\u0001b\"}") == Control.Build(), "escaping: a control character below 0x20 becomes \\u00xx");

    /* The bytes of a UTF-8 character (here \xc3\xa9, the two bytes of e-acute) pass through unescaped: nothing follows them in
       the literal that could be misread as part of the hex escape */
    MailLog Utf8;

    Utf8.SetBody (std::string ("caf\xc3\xa9"));
    Check (std::string::npos != Utf8.Build().find (std::string ("caf\xc3\xa9")), "escaping: the bytes of a UTF-8 character pass through unescaped");
}


static void TestSize()
{
    MailLog Size;

    Size.SetSize (12345);
    Check (("{" + Scope + ",\"attributes\":{\"email.size_bytes\":12345}}") == Size.Build(), "size: a plain JSON number, not a string");

    Size.SetSize (0);
    Check (("{" + Scope + ",\"attributes\":{\"email.size_bytes\":12345}}") == Size.Build(), "size: 0 is ignored, like the other numbers in this class");

    /* email.size_bytes is added after the other attributes, whatever order the methods were called in: here SetSize came first */
    MailLog Order;

    Order.SetSize (99);
    Order.SetFrom ("a@b");

    Check (("{" + Scope + ",\"body\":\"from=a@b\",\"attributes\":{\"email.from.address\":\"a@b\",\"email.size_bytes\":99}}") == Order.Build(), "size: added after the other attributes, whatever order the methods were called in");
}


static void TestAttachments()
{
    MailLog Basic;

    Basic.AddAttachment ("invoice.pdf", 45231);
    Check (("{" + Scope + ",\"attributes\":{\"email.attachments\":[{\"name\":\"invoice.pdf\",\"size_bytes\":45231}]}}") == Basic.Build(),
           "attachment: name and size, nothing else given");

    MailLog WithHash;

    WithHash.AddAttachment ("contract.pdf", 102400, "ab12cd34");
    Check (("{" + Scope + ",\"attributes\":{\"email.attachments\":[{\"name\":\"contract.pdf\",\"size_bytes\":102400,\"sha256\":\"ab12cd34\"}]}}") == WithHash.Build(),
           "attachment: with a SHA-256 hash");

    MailLog WithContentType;

    WithContentType.AddAttachment ("photo.jpg", 900, "", "image/jpeg");
    Check (("{" + Scope + ",\"attributes\":{\"email.attachments\":[{\"name\":\"photo.jpg\",\"size_bytes\":900,\"content_type\":\"image/jpeg\"}]}}") == WithContentType.Build(),
           "attachment: with a content type but no hash - a field which was not given is left out, not sent empty");

    MailLog Full;

    Full.AddAttachment ("data.csv", 55, "deadbeef", "text/csv");
    Check (("{" + Scope + ",\"attributes\":{\"email.attachments\":[{\"name\":\"data.csv\",\"size_bytes\":55,\"sha256\":\"deadbeef\",\"content_type\":\"text/csv\"}]}}") == Full.Build(),
           "attachment: name, size, hash and content type together, in that fixed order");

    MailLog NoSize;

    NoSize.AddAttachment ("unknown.bin", 0);
    Check (("{" + Scope + ",\"attributes\":{\"email.attachments\":[{\"name\":\"unknown.bin\"}]}}") == NoSize.Build(),
           "attachment: 0 is not known, like the other numbers in this class - only the name is sent");

    MailLog NoName;

    NoName.AddAttachment ("", 100);
    Check (("{" + Scope + "}") == NoName.Build(), "attachment: without a name the call is ignored, there is no attachment");

    MailLog Several;

    Several.AddAttachment ("a.txt", 1);
    Several.AddAttachment ("b.txt", 2);
    Check (("{" + Scope + ",\"attributes\":{\"email.attachments\":[{\"name\":\"a.txt\",\"size_bytes\":1},{\"name\":\"b.txt\",\"size_bytes\":2}]}}") == Several.Build(),
           "attachment: more than one, in the order they were added");

    /* email.attachments is added after email.size_bytes, whatever order the methods were called in */
    MailLog Order;

    Order.AddAttachment ("x.txt", 1);
    Order.SetSize (500);
    Order.SetFrom ("a@b");

    Check (("{" + Scope + ",\"body\":\"from=a@b\",\"attributes\":{\"email.from.address\":\"a@b\",\"email.size_bytes\":500,\"email.attachments\":[{\"name\":\"x.txt\",\"size_bytes\":1}]}}") == Order.Build(),
           "attachment: email.attachments comes after email.size_bytes, whatever order the methods were called in");
}


static void TestCryptoAndTls()
{
    MailLog Signed;

    Signed.SetSigned (true);
    Check (("{" + Scope + ",\"attributes\":{\"email.signed\":true}}") == Signed.Build(), "signed: a real JSON boolean, not a string");

    MailLog SignedFalse;

    SignedFalse.SetSigned (false);
    Check (("{" + Scope + ",\"attributes\":{\"email.signed\":false}}") == SignedFalse.Build(),
           "signed: false is sent too - unlike every other Set method, false always takes effect here, it is not ignored");

    MailLog Encrypted;

    Encrypted.SetEncrypted (true);
    Check (("{" + Scope + ",\"attributes\":{\"email.encrypted\":true}}") == Encrypted.Build(), "encrypted: email.encrypted");

    MailLog Neither;

    Check (("{" + Scope + "}") == Neither.Build(), "signed/encrypted: neither called, neither field is sent, not even false");

    MailLog Protocol;

    Protocol.SetCryptoProtocol ("pgp");
    Check (("{" + Scope + ",\"attributes\":{\"email.crypto.protocol.name\":\"pgp\"}}") == Protocol.Build(), "crypto protocol: free text, like email.direction");

    Protocol.SetCryptoProtocol ("");
    Check (("{" + Scope + ",\"attributes\":{\"email.crypto.protocol.name\":\"pgp\"}}") == Protocol.Build(), "crypto protocol: an empty text is ignored, the previous value stays");

    /* email.signed and email.encrypted are always in that fixed position (after email.size_bytes), whatever order the methods were
       called in: here SetEncrypted is called before SetSigned, but SetSigned still renders first */
    MailLog Combined;

    Combined.SetFrom           ("a@b");
    Combined.SetCryptoProtocol ("smime");
    Combined.SetSize           (100);
    Combined.SetEncrypted      (true);
    Combined.SetSigned         (false);

    Check (("{" + Scope + ",\"body\":\"from=a@b\",\"attributes\":{\"email.from.address\":\"a@b\",\"email.crypto.protocol.name\":\"smime\",\"email.size_bytes\":100,\"email.signed\":false,\"email.encrypted\":true}}") == Combined.Build(),
           "signed/encrypted: fixed position after email.size_bytes, signed before encrypted, regardless of call order");

    /* tls.established is a different fact than email.encrypted: the connection, not the message content. Same real-boolean rule */
    MailLog TlsEstablished;

    TlsEstablished.SetTlsEstablished (true);
    Check (("{" + Scope + ",\"attributes\":{\"tls.established\":true}}") == TlsEstablished.Build(), "tls established: a real JSON boolean, the connection, not the message content");

    MailLog TlsEstablishedFalse;

    TlsEstablishedFalse.SetTlsEstablished (false);
    Check (("{" + Scope + ",\"attributes\":{\"tls.established\":false}}") == TlsEstablishedFalse.Build(), "tls established: false is sent too, the same rule as email.signed/email.encrypted");

    MailLog Tls;

    Tls.SetTlsProtocolName    ("tls");
    Tls.SetTlsProtocolVersion ("1.3");
    Tls.SetTlsCipher          ("TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256");

    Check (("{" + Scope + ",\"attributes\":{\"tls.protocol.name\":\"tls\",\"tls.protocol.version\":\"1.3\",\"tls.cipher\":\"TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256\"}}") == Tls.Build(),
           "tls protocol name, version, cipher: free text, generic OpenTelemetry attributes, not email.*");

    Tls.SetTlsCipher ("");
    Check (("{" + Scope + ",\"attributes\":{\"tls.protocol.name\":\"tls\",\"tls.protocol.version\":\"1.3\",\"tls.cipher\":\"TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256\"}}") == Tls.Build(),
           "tls cipher: an empty text is ignored, the previous value stays");
}


static void TestConnectionAndSmtp()
{
    MailLog Peer;

    Peer.SetClientAddress ("198.51.100.5");
    Peer.SetServerAddress ("mail01.example.com");
    Check (("{" + Scope + ",\"attributes\":{\"client.address\":\"198.51.100.5\",\"server.address\":\"mail01.example.com\"}}") == Peer.Build(),
           "connection peer: client.address, server.address, free text, generic OpenTelemetry attributes, not email.* - the same reasoning as tls.*");

    Peer.SetClientAddress ("");
    Check (("{" + Scope + ",\"attributes\":{\"client.address\":\"198.51.100.5\",\"server.address\":\"mail01.example.com\"}}") == Peer.Build(),
           "connection peer: an empty address is ignored, the previous value stays");

    MailLog Port;

    Port.SetClientPort (51234);
    Port.SetServerPort (25);
    Check (("{" + Scope + ",\"attributes\":{\"client.port\":51234,\"server.port\":25}}") == Port.Build(),
           "connection peer: client.port, server.port, plain JSON numbers - unlike email.signed/email.spam.score, 0 is never a valid port number, so it is ignored the same as everywhere else, not a special exception");

    Port.SetClientPort (0);
    Check (("{" + Scope + ",\"attributes\":{\"client.port\":51234,\"server.port\":25}}") == Port.Build(),
           "connection peer: 0 is ignored, the previous value stays");

    /* client.*, server.* are the logical parties, network.* the actual socket endpoints: they can all be there at once, and differ
       (a proxy or a load balancer in between). The ports are numbers and come after the texts, in the fixed tail */
    MailLog Network;

    Network.SetClientAddress       ("sender.example.com");
    Network.SetServerAddress       ("mx.example.net");
    Network.SetNetworkLocalAddress ("192.0.2.20");
    Network.SetNetworkLocalPort    (25);
    Network.SetNetworkPeerAddress  ("203.0.113.10");
    Network.SetNetworkPeerPort     (52341);
    Check (("{" + Scope + ",\"attributes\":{\"client.address\":\"sender.example.com\",\"server.address\":\"mx.example.net\",\"network.local.address\":\"192.0.2.20\",\"network.peer.address\":\"203.0.113.10\",\"network.local.port\":25,\"network.peer.port\":52341}}") == Network.Build(),
           "network: the logical client/server and the actual local/peer socket endpoint, all optional and independent of each other");

    MailLog OnlyIp;

    OnlyIp.SetClientAddress ("203.0.113.10");
    Check (("{" + Scope + ",\"attributes\":{\"client.address\":\"203.0.113.10\"}}") == OnlyIp.Build(),
           "network: if an IP address is all the caller has it is a valid client.address, nothing else has to be set");

    MailLog NetworkOrder;

    NetworkOrder.SetNetworkPeerPort  (2);
    NetworkOrder.SetServerPort       (3);
    NetworkOrder.SetNetworkLocalPort (1);
    NetworkOrder.SetClientPort       (4);
    Check (("{" + Scope + ",\"attributes\":{\"client.port\":4,\"server.port\":3,\"network.local.port\":1,\"network.peer.port\":2}}") == NetworkOrder.Build(),
           "network: client.port, server.port, network.local.port, network.peer.port, always in that order, whatever order the methods were called in");

    /* The certificate of the TLS connection: for mail sent out only the remote server's, for mutual TLS the client's too. All text,
       the class does not check a distinguished name or a date */
    MailLog Certificate;

    Certificate.SetTlsServerSubject   ("CN=mx.example.net, O=Example");
    Certificate.SetTlsServerIssuer    ("CN=Example CA");
    Certificate.SetTlsServerSha256    ("0687F666A054EF17A08E2F2162EAB4CBC0D265E1D7875BE74BF3C712CA92DAF0");
    Certificate.SetTlsServerNotBefore ("2026-01-01T00:00:00.000Z");
    Certificate.SetTlsServerNotAfter  ("2027-01-01T00:00:00.000Z");
    Check (("{" + Scope + R"(,"attributes":{"tls.server.subject":"CN=mx.example.net, O=Example","tls.server.issuer":"CN=Example CA")"
            R"(,"tls.server.hash.sha256":"0687F666A054EF17A08E2F2162EAB4CBC0D265E1D7875BE74BF3C712CA92DAF0")"
            R"(,"tls.server.not_before":"2026-01-01T00:00:00.000Z","tls.server.not_after":"2027-01-01T00:00:00.000Z"}})") == Certificate.Build(),
           "tls certificate: the server's subject, issuer, SHA-256 fingerprint and validity dates, generic OpenTelemetry attributes, not email.*");

    MailLog MutualTls;

    MutualTls.SetTlsClientSubject ("CN=partner.example.org");
    MutualTls.SetTlsServerSubject ("CN=mx.example.net");
    Check (("{" + Scope + R"(,"attributes":{"tls.client.subject":"CN=partner.example.org","tls.server.subject":"CN=mx.example.net"}})") == MutualTls.Build(),
           "tls certificate: the client's certificate (mutual TLS) and the server's are independent of each other, each one optional");

    MailLog Helo;

    Helo.SetSmtpHelo ("mail-client.example.net");
    Check (("{" + Scope + ",\"attributes\":{\"email.smtp.helo\":\"mail-client.example.net\"}}") == Helo.Build(),
           "smtp helo: email.smtp.helo, free text - genuinely mail-specific, unlike client.address/server.address above, the same reasoning as email.spf.result");

    MailLog Response;

    Response.SetSmtpResponseCode   (550);
    Response.SetSmtpEnhancedStatus ("5.7.1");
    Response.SetSmtpResponseText   ("Relay access denied");
    Check (("{" + Scope + ",\"attributes\":{\"email.smtp.response.enhanced_status_code\":\"5.7.1\",\"email.smtp.response.text\":\"Relay access denied\",\"email.smtp.response.code\":550}}") == Response.Build(),
           "smtp response: the 3-digit code as a plain JSON number (fixed position, after the free-text fields), the RFC 3463 enhanced status and the free text, a different fact than email.action/email.reason");

    Response.SetSmtpResponseCode (0);
    Check (("{" + Scope + ",\"attributes\":{\"email.smtp.response.enhanced_status_code\":\"5.7.1\",\"email.smtp.response.text\":\"Relay access denied\",\"email.smtp.response.code\":550}}") == Response.Build(),
           "smtp response code: 0 is ignored, the previous value stays");

    /* client.port/server.port/email.smtp.response.code are added after email.attachments, whatever order the methods were called in */
    MailLog Order;

    Order.SetSmtpResponseCode (250);
    Order.SetServerPort       (25);
    Order.SetFrom             ("a@b");
    Order.SetClientPort       (51234);

    Check (("{" + Scope + ",\"body\":\"from=a@b\",\"attributes\":{\"email.from.address\":\"a@b\",\"client.port\":51234,\"server.port\":25,\"email.smtp.response.code\":250}}") == Order.Build(),
           "connection peer and smtp response: client.port, server.port, email.smtp.response.code, always in that order, whatever order the methods were called in");
}


static void TestSpamAndVirus()
{
    MailLog Score;

    Score.SetSpamScore (3.2);
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.score\":3.2}}") == Score.Build(), "spam score: a plain JSON number, not a string");

    MailLog ScoreZero;

    ScoreZero.SetSpamScore (0.0);
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.score\":0.0}}") == ScoreZero.Build(), "spam score: 0 is sent, as 0.0 so that it stays a double - unlike every other number in this class it is a real value, not \"unspecified\"");

    MailLog ScoreNegative;

    ScoreNegative.SetSpamScore (-1.5);
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.score\":-1.5}}") == ScoreNegative.Build(), "spam score: a negative score, like SpamAssassin gives a clean message");

    MailLog NotANumber;

    NotANumber.SetSpamScore (std::numeric_limits<double>::quiet_NaN());
    Check (("{" + Scope + "}") == NotANumber.Build(), "spam score: NaN has no JSON number, it is not sent");

    MailLog Infinite;

    Infinite.SetSpamScore (std::numeric_limits<double>::infinity());
    Check (("{" + Scope + "}") == Infinite.Build(), "spam score: infinite has no JSON number either, it is not sent");

    MailLog Result;

    Result.SetSpamResult ("spam");
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.result\":\"spam\"}}") == Result.Build(), "spam result: free text, like email.direction");

    Result.SetSpamResult ("");
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.result\":\"spam\"}}") == Result.Build(), "spam result: an empty text is ignored, the previous value stays");

    MailLog VirusChecked;

    VirusChecked.SetVirusChecked (true);
    Check (("{" + Scope + ",\"attributes\":{\"email.virus.checked\":true}}") == VirusChecked.Build(),
           "virus checked: a real JSON boolean, not a string - only whether a scan happened, not what it found");

    MailLog VirusCheckedFalse;

    VirusCheckedFalse.SetVirusChecked (false);
    Check (("{" + Scope + ",\"attributes\":{\"email.virus.checked\":false}}") == VirusCheckedFalse.Build(),
           "virus checked: false is sent too - the producer knows for certain no scan happened, which is different from not knowing whether one did");

    MailLog VirusResult;

    VirusResult.SetVirusResult ("clean");
    Check (("{" + Scope + ",\"attributes\":{\"email.virus.result\":\"clean\"}}") == VirusResult.Build(),
           "virus result: free text - email.virus.checked=true and email.virus.result=\"clean\" together are the normal clean-message combination");

    MailLog SpamEngine;

    SpamEngine.SetSpamEngine ("Rspamd");
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.engine.name\":\"Rspamd\"}}") == SpamEngine.Build(), "spam engine: free text, which scanner produced the score and result");

    MailLog VirusEngine;

    VirusEngine.SetVirusEngine ("ClamAV");
    Check (("{" + Scope + ",\"attributes\":{\"email.virus.engine.name\":\"ClamAV\"}}") == VirusEngine.Build(), "virus engine: free text, which scanner produced the virus result");

    MailLog VirusName;

    VirusName.SetVirusName ("Win32/Whatever");
    Check (("{" + Scope + ",\"attributes\":{\"email.virus.threat.name\":\"Win32/Whatever\"}}") == VirusName.Build(), "virus name: free text, the signature/threat name if one was found");

    /* Every special-position field together, set in a different order than Build() puts them in, to pin their order (the whole order is in the full record):
       generic attributes (first use order), then email.size_bytes, email.attachments, client.port, server.port,
       email.smtp.response.code, email.signed, email.encrypted, tls.established, email.spam.score, email.virus.checked */
    MailLog Combined;

    Combined.SetFrom             ("a@b");
    Combined.SetSpamResult       ("spam");
    Combined.SetVirusChecked     (true);
    Combined.SetSmtpResponseCode (550);
    Combined.SetEncrypted        (true);
    Combined.SetServerPort       (25);
    Combined.SetSpamScore        (5.5);
    Combined.SetSigned           (false);
    Combined.AddAttachment       ("x.txt", 1);
    Combined.SetTlsEstablished   (true);
    Combined.SetClientPort       (51234);
    Combined.SetSize             (10);

    Check (("{" + Scope + ",\"body\":\"from=a@b\",\"attributes\":{\"email.from.address\":\"a@b\",\"email.spam.result\":\"spam\",\"email.size_bytes\":10,\"email.attachments\":[{\"name\":\"x.txt\",\"size_bytes\":1}],\"client.port\":51234,\"server.port\":25,\"email.smtp.response.code\":550,\"email.signed\":false,\"email.encrypted\":true,\"tls.established\":true,\"email.spam.score\":5.5,\"email.virus.checked\":true}}") == Combined.Build(),
           "order: email.size_bytes, email.attachments, client.port, server.port, email.smtp.response.code, email.signed, "
           "email.encrypted, tls.established, email.spam.score, email.virus.checked, always in that order, whatever order the "
           "methods were called in");
}


static void TestAuthResults()
{
    MailLog Auth;

    Auth.SetSpfResult   ("pass");
    Auth.SetDkimResult  ("pass");
    Auth.SetDmarcResult ("pass");

    Check (("{" + Scope + ",\"attributes\":{\"email.spf.result\":\"pass\",\"email.dkim.result\":\"pass\",\"email.dmarc.result\":\"pass\"}}") == Auth.Build(),
           "auth results: spf, dkim, dmarc, in the order they were set - free text, not checked against the RFCs' own vocabulary");

    Auth.SetSpfResult ("");
    Check (("{" + Scope + ",\"attributes\":{\"email.spf.result\":\"pass\",\"email.dkim.result\":\"pass\",\"email.dmarc.result\":\"pass\"}}") == Auth.Build(),
           "auth results: an empty result is ignored, the previous value stays");

    Auth.SetSpfDomain  ("example.com");
    Auth.SetDkimDomain ("example.com");
    Check (("{" + Scope + ",\"attributes\":{\"email.spf.result\":\"pass\",\"email.dkim.result\":\"pass\",\"email.dmarc.result\":\"pass\",\"email.spf.domain\":\"example.com\",\"email.dkim.domain\":\"example.com\"}}") == Auth.Build(),
           "auth domains: the domain each check actually ran against, added after the results - there is no equivalent for DMARC");

    Auth.SetSpfDomain ("");
    Check (("{" + Scope + ",\"attributes\":{\"email.spf.result\":\"pass\",\"email.dkim.result\":\"pass\",\"email.dmarc.result\":\"pass\",\"email.spf.domain\":\"example.com\",\"email.dkim.domain\":\"example.com\"}}") == Auth.Build(),
           "auth domains: an empty domain is ignored, the previous value stays");

    Auth.SetDkimSelector ("selector1");
    Check (("{" + Scope + ",\"attributes\":{\"email.spf.result\":\"pass\",\"email.dkim.result\":\"pass\",\"email.dmarc.result\":\"pass\",\"email.spf.domain\":\"example.com\",\"email.dkim.domain\":\"example.com\",\"email.dkim.selector\":\"selector1\"}}") == Auth.Build(),
           "dkim selector: together with the domain identifies the exact signing key/configuration used");
}


static void TestClassificationAutoSubmittedActionAndPolicy()
{
    MailLog Classification;

    Classification.SetClassification ("confidential");
    Check (("{" + Scope + ",\"attributes\":{\"email.classification\":\"confidential\"}}") == Classification.Build(), "classification: free text, not checked against the four-tier scheme or any other list");

    Classification.SetClassification ("");
    Check (("{" + Scope + ",\"attributes\":{\"email.classification\":\"confidential\"}}") == Classification.Build(), "classification: an empty text is ignored, the previous value stays");

    MailLog AutoSubmitted;

    AutoSubmitted.SetAutoSubmitted ("auto-replied");
    Check (("{" + Scope + ",\"attributes\":{\"email.auto_submitted\":\"auto-replied\"}}") == AutoSubmitted.Build(), "auto submitted: email.auto_submitted, free text, the header Auto-Submitted (RFC 3834)");

    MailLog Action;

    Action.SetAction ("quarantine");
    Check (("{" + Scope + ",\"body\":\"quarantine\",\"attributes\":{\"email.action\":\"quarantine\"}}") == Action.Build(), "action: email.action, free text, what the mail system did, not a verdict");

    Action.SetAction ("");
    Check (("{" + Scope + ",\"body\":\"quarantine\",\"attributes\":{\"email.action\":\"quarantine\"}}") == Action.Build(), "action: an empty text is ignored, the previous value stays");

    Action.SetReason     ("Attachment matched AV signature");
    Action.SetPolicyName ("AV-Block-Executables");
    Action.SetPolicyId   ("POL-1042");

    Check (("{" + Scope + ",\"body\":\"quarantine\",\"attributes\":{\"email.action\":\"quarantine\",\"email.reason\":\"Attachment matched AV signature\",\"email.policy.name\":\"AV-Block-Executables\",\"email.policy.id\":\"POL-1042\"}}") == Action.Build(),
           "reason and policy: why the action happened, and which rule decided it - added after email.action, in the order they were set");

    Action.SetReason ("");
    Check (("{" + Scope + ",\"body\":\"quarantine\",\"attributes\":{\"email.action\":\"quarantine\",\"email.reason\":\"Attachment matched AV signature\",\"email.policy.name\":\"AV-Block-Executables\",\"email.policy.id\":\"POL-1042\"}}") == Action.Build(),
           "reason: an empty text is ignored, the previous value stays");
}


/* Calls every setter of the class once (SetEnvelopeFromEmpty is the one exception, it would replace SetEnvelopeFrom: see
   TestClearAndRepeat), each with a short value which says what it is. The generic attributes are set in the order the record
   below expects them: the order their key was first used */
static void FillAll (MailLog& retLog)
{
    retLog.SetHostName          ("h");
    retLog.SetServiceName       ("s");
    retLog.SetServiceNamespace  ("n");
    retLog.SetServiceInstanceId ("i");
    retLog.SetResourceAttribute ("r", "v");

    retLog.SetFrom               ("a");
    retLog.AddTo                 ("t");
    retLog.AddCc                 ("c");
    retLog.AddBcc                ("b");
    retLog.SetEnvelopeFrom       ("ef");
    retLog.AddRecipient          ("er", "st", 250, "2.0.0", "tx");
    retLog.SetSubject            ("s");
    retLog.SetMessageId          ("m");
    retLog.SetQueueId            ("q");
    retLog.SetEvent              ("e");
    retLog.SetDirection          ("d");
    retLog.AddHeader             ("X-H", "h");
    retLog.SetCryptoProtocol     ("p");
    retLog.SetTlsProtocolName    ("tn");
    retLog.SetTlsProtocolVersion ("tv");
    retLog.SetTlsCipher          ("tc");
    retLog.SetTlsClientSubject   ("cs");
    retLog.SetTlsClientIssuer    ("ci");
    retLog.SetTlsClientSha256    ("ch");
    retLog.SetTlsClientNotBefore ("cb");
    retLog.SetTlsClientNotAfter  ("ca2");
    retLog.SetTlsServerSubject   ("ss");
    retLog.SetTlsServerIssuer    ("si");
    retLog.SetTlsServerSha256    ("sh");
    retLog.SetTlsServerNotBefore ("sb");
    retLog.SetTlsServerNotAfter  ("sn");
    retLog.SetClientAddress      ("ca");
    retLog.SetServerAddress      ("sa");
    retLog.SetNetworkLocalAddress ("la");
    retLog.SetNetworkPeerAddress  ("pa");
    retLog.SetSmtpHelo           ("helo");
    retLog.SetSmtpEnhancedStatus ("es");
    retLog.SetSmtpResponseText   ("rt");
    retLog.SetSpamResult         ("sr");
    retLog.SetSpamEngine         ("se");
    retLog.SetSpfResult          ("spf");
    retLog.SetDkimResult         ("dkim");
    retLog.SetDmarcResult        ("dmarc");
    retLog.SetSpfDomain          ("sd");
    retLog.SetDkimDomain         ("dd");
    retLog.SetDkimSelector       ("ds");
    retLog.SetVirusResult        ("vr");
    retLog.SetVirusName          ("vn");
    retLog.SetVirusEngine        ("ve");
    retLog.SetClassification     ("cl");
    retLog.SetAutoSubmitted      ("au");
    retLog.SetAction             ("ac");
    retLog.SetReason             ("re");
    retLog.SetPolicyName         ("pn");
    retLog.SetPolicyId           ("pi");

    retLog.SetSize             (10);
    retLog.AddAttachment       ("f", 5, "h1", "t/x");
    retLog.SetClientPort       (1);
    retLog.SetServerPort       (2);
    retLog.SetNetworkLocalPort (3);
    retLog.SetNetworkPeerPort  (4);
    retLog.SetSmtpResponseCode (250);
    retLog.SetSigned           (true);
    retLog.SetEncrypted        (false);
    retLog.SetTlsEstablished   (true);
    retLog.SetSpamScore        (1.5);
    retLog.SetVirusChecked     (true);

    retLog.SetBody         ("b");
    retLog.SetSeverity     (9, "INFO");
    retLog.SetTime         (1);
    retLog.SetObservedTime (2);
}


/* One record with every field the class has, the whole string pinned: this is what keeps a new method from being added to the
   class without a test which sees it in a complete record, and it pins the order of every field against every other one */
static void TestFullRecord()
{
    MailLog Log;

    FillAll (Log);

    Check (("{" + Scope +
            R"(,"resource":{"host.name":"h","service.name":"s","service.namespace":"n","service.instance.id":"i","r":"v"})"
            R"(,"time_unix_nano":"1","observed_time_unix_nano":"2","severity_number":9,"severity_text":"INFO","body":"b")"
            R"(,"attributes":{"email.from.address":"a","email.to.addresses":["t"],"email.cc.addresses":["c"],"email.bcc.addresses":["b"],"email.envelope.from.address":"ef")"
            R"(,"email.subject":"s","email.message.id":"m","email.queue.id":"q","email.event":"e","email.direction":"d","email.header.x_h":"h")"
            R"(,"email.crypto.protocol.name":"p","tls.protocol.name":"tn","tls.protocol.version":"tv","tls.cipher":"tc","tls.client.subject":"cs","tls.client.issuer":"ci","tls.client.hash.sha256":"ch","tls.client.not_before":"cb","tls.client.not_after":"ca2","tls.server.subject":"ss","tls.server.issuer":"si","tls.server.hash.sha256":"sh","tls.server.not_before":"sb","tls.server.not_after":"sn")"
            R"(,"client.address":"ca","server.address":"sa","network.local.address":"la","network.peer.address":"pa","email.smtp.helo":"helo","email.smtp.response.enhanced_status_code":"es")"
            R"(,"email.smtp.response.text":"rt","email.spam.result":"sr","email.spam.engine.name":"se","email.spf.result":"spf","email.dkim.result":"dkim")"
            R"(,"email.dmarc.result":"dmarc","email.spf.domain":"sd","email.dkim.domain":"dd","email.dkim.selector":"ds")"
            R"(,"email.virus.result":"vr","email.virus.threat.name":"vn","email.virus.engine.name":"ve","email.classification":"cl","email.auto_submitted":"au")"
            R"(,"email.action":"ac","email.reason":"re","email.policy.name":"pn","email.policy.id":"pi")"
            R"(,"email.size_bytes":10,"email.attachments":[{"name":"f","size_bytes":5,"sha256":"h1","content_type":"t/x"}])"
            R"(,"email.envelope.recipients":[{"address":"er","status":"st","code":250,"enhanced_status_code":"2.0.0","text":"tx"}])"
            R"(,"client.port":1,"server.port":2,"network.local.port":3,"network.peer.port":4,"email.smtp.response.code":250,"email.signed":true,"email.encrypted":false)"
            R"(,"tls.established":true,"email.spam.score":1.5,"email.virus.checked":true}})") == Log.Build(),
           "full record: every setter of the class in one record, every field and its position pinned");
}


/* Every setter which takes a single text (and Add... which does too, AddRecipient has more parameters and is tested on its own): an empty text sends nothing on a new object and never
   replaces a value which was set before. The same rule in 53 places, so the list is a table */
struct EmptyCase
{
    const char *pszName;
    void (MailLog::*pSetter) (const std::string&);
    bool bAdd;                                        /* an Add... method: more than one value is kept, a Set... replaces */
};

#define EMPTY_CASE(Method)     { #Method, &MailLog::Method, false }
#define EMPTY_CASE_ADD(Method) { #Method, &MailLog::Method, true }

static const EmptyCase g_EmptyCases[] =
{
    EMPTY_CASE (SetHostName),          EMPTY_CASE (SetServiceName),        EMPTY_CASE (SetServiceNamespace),
    EMPTY_CASE (SetServiceInstanceId), EMPTY_CASE (SetFrom),               EMPTY_CASE_ADD (AddTo),
    EMPTY_CASE_ADD (AddCc),            EMPTY_CASE_ADD (AddBcc),            EMPTY_CASE (SetEnvelopeFrom),
    EMPTY_CASE (SetSubject),            EMPTY_CASE (SetMessageId),
    EMPTY_CASE (SetQueueId),           EMPTY_CASE (SetEvent),              EMPTY_CASE (SetDirection),
    EMPTY_CASE (SetCryptoProtocol),    EMPTY_CASE (SetTlsProtocolName),    EMPTY_CASE (SetTlsProtocolVersion),
    EMPTY_CASE (SetTlsCipher),         EMPTY_CASE (SetClientAddress),      EMPTY_CASE (SetServerAddress),
    EMPTY_CASE (SetSmtpHelo),          EMPTY_CASE (SetSmtpEnhancedStatus), EMPTY_CASE (SetSmtpResponseText),
    EMPTY_CASE (SetSpamResult),        EMPTY_CASE (SetSpfResult),          EMPTY_CASE (SetDkimResult),
    EMPTY_CASE (SetDmarcResult),       EMPTY_CASE (SetSpfDomain),          EMPTY_CASE (SetDkimDomain),
    EMPTY_CASE (SetDkimSelector),      EMPTY_CASE (SetVirusResult),        EMPTY_CASE (SetVirusName),
    EMPTY_CASE (SetClassification),    EMPTY_CASE (SetAutoSubmitted),      EMPTY_CASE (SetAction),
    EMPTY_CASE (SetReason),            EMPTY_CASE (SetPolicyName),         EMPTY_CASE (SetPolicyId),
    EMPTY_CASE (SetSpamEngine),        EMPTY_CASE (SetVirusEngine),        EMPTY_CASE (SetNetworkLocalAddress),
    EMPTY_CASE (SetNetworkPeerAddress), EMPTY_CASE (SetTlsClientSubject),  EMPTY_CASE (SetTlsClientIssuer),
    EMPTY_CASE (SetTlsClientSha256),    EMPTY_CASE (SetTlsClientNotBefore), EMPTY_CASE (SetTlsClientNotAfter),
    EMPTY_CASE (SetTlsServerSubject),   EMPTY_CASE (SetTlsServerIssuer),    EMPTY_CASE (SetTlsServerSha256),
    EMPTY_CASE (SetTlsServerNotBefore), EMPTY_CASE (SetTlsServerNotAfter),  EMPTY_CASE (SetBody)
};

static void TestEmptyIgnored()
{
    /* One line per setter, three things in it: an empty text on a new object sends nothing, an empty text never replaces or removes a
       value which was set, and a second value replaces the first one (a Set...) or is kept next to it (an Add...) */
    for (const EmptyCase& Case : g_EmptyCases)
    {
        MailLog Fresh;

        (Fresh.*Case.pSetter) ("");

        bool bFresh = (("{" + Scope + "}") == Fresh.Build());

        MailLog Kept;

        (Kept.*Case.pSetter) ("v");

        std::string Before = Kept.Build();

        (Kept.*Case.pSetter) ("");

        bool bKept = (Before == Kept.Build());

        MailLog Twice;
        MailLog Once;

        (Twice.*Case.pSetter) ("v1");
        (Twice.*Case.pSetter) ("v2");
        (Once.*Case.pSetter)  ("v2");

        std::string Built = Twice.Build();
        bool        bSecond;

        if (Case.bAdd)
            bSecond = (std::string::npos != Built.find ("\"v1\"")) && (std::string::npos != Built.find ("\"v2\"")) && (Built != Once.Build());
        else
            bSecond = (Built == Once.Build());

        std::string Name = std::string (Case.pszName) + ": an empty text is ignored, a second value " + (Case.bAdd ? "is kept next to the first" : "replaces the first");

        if ( (false == bFresh) || (false == bKept) || (false == bSecond) )
            Name += std::string (" [new object=") + (bFresh ? "ok" : "WRONG") + " existing value=" + (bKept ? "ok" : "WRONG") + " second value=" + (bSecond ? "ok" : "WRONG") + "]";

        Check (bFresh && bKept && bSecond, Name.c_str());
    }

    MailLog NoKey;

    NoKey.SetResourceAttribute ("", "v");
    Check (("{" + Scope + "}") == NoKey.Build(), "SetResourceAttribute: an empty key is ignored");
}


/* The text of a JSON number as the grammar of RFC 8259 has it: an optional minus, an integer part without leading zeros, an optional
   fraction, an optional exponent. No decimal comma, no "nan", no "inf" */
static bool IsJsonNumber (const std::string& Text)
{
    size_t i = 0;

    if ( (i < Text.size()) && ('-' == Text[i]) )
        i++;

    if (i >= Text.size())
        return false;

    if ('0' == Text[i])
    {
        i++;
    }
    else if ( (Text[i] >= '1') && (Text[i] <= '9') )
    {
        while ( (i < Text.size()) && (Text[i] >= '0') && (Text[i] <= '9') )
            i++;
    }
    else
    {
        return false;
    }

    if ( (i < Text.size()) && ('.' == Text[i]) )
    {
        i++;

        size_t Start = i;

        while ( (i < Text.size()) && (Text[i] >= '0') && (Text[i] <= '9') )
            i++;

        if (i == Start)
            return false;
    }

    if ( (i < Text.size()) && (('e' == Text[i]) || ('E' == Text[i])) )
    {
        i++;

        if ( (i < Text.size()) && (('+' == Text[i]) || ('-' == Text[i])) )
            i++;

        size_t Start = i;

        while ( (i < Text.size()) && (Text[i] >= '0') && (Text[i] <= '9') )
            i++;

        if (i == Start)
            return false;
    }

    return i == Text.size();
}


/* Whatever the value of a score is, what is written is a valid JSON number, and it reads back as exactly the same double. The exact
   text (1e-04 or 0.0001) is up to std::to_chars, only its validity and its exactness are the contract */
static void TestSpamScoreIsAlwaysValidJson()
{
    /* No subnormal number here (5e-324): std::from_chars of g++ 11 reports out of range for those, newer libraries do not, and this test is
       about what is written, not about the reading library */
    const double Scores[] = { 0.0, -0.0, 0.1, 3.2, -1.5, 3.0, 123456.0, 0.0001, 1e-7, 100000.0, 123456789.123, 1e10, 1e300,
                              2.2250738585072014e-308, 1.7976931348623157e308, 0.1 + 0.2, 1.0 / 3.0 };
    const std::string Key = "\"email.spam.score\":";

    for (double Score : Scores)
    {
        MailLog Log;

        Log.SetSpamScore (Score);

        std::string Built = Log.Build();
        size_t      Pos   = Built.find (Key);

        if (std::string::npos == Pos)
        {
            Check (false, "spam score: the score is missing from the record");
            continue;
        }

        /* the number ends at the two closing braces: the attributes and the record */
        std::string Number = Built.substr (Pos + Key.size(), Built.size() - (Pos + Key.size()) - 2);

        /* One line per score, named by the text which was written (1e+300, not the 300 digits of the value). Three things must hold:
           a valid JSON number, a point or an exponent in it (a reader which types a number by its text, otelfwd does, must see a
           double, never "3"), and it reads back as exactly the same double */
        bool bValid  = IsJsonNumber (Number);
        bool bDouble = (std::string::npos != Number.find_first_of (".eE"));
        bool bExact  = false;
        double Back  = 0.0;

        std::from_chars_result Result = std::from_chars (Number.data(), Number.data() + Number.size(), Back);

        bExact = (std::errc() == Result.ec) && (Back == Score);

        std::string Name = "spam score written as " + Number + ": a valid JSON number, a double, reads back exactly";

        if ( (false == bValid) || (false == bDouble) || (false == bExact) )
            Name += std::string (" [valid=") + (bValid ? "yes" : "NO") + " double=" + (bDouble ? "yes" : "NO") + " exact=" + (bExact ? "yes" : "NO") + "]";

        Check (bValid && bDouble && bExact, Name.c_str());
    }

    /* Whole numbers are written with ".0", pinned exactly: the text is part of the contract, not only its validity */
    MailLog WholeNumber;

    WholeNumber.SetSpamScore (3.0);
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.score\":3.0}}") == WholeNumber.Build(), "spam score: 3.0 is written as 3.0, not 3 (which would be an integer for otelfwd)");

    MailLog NegativeZero;

    NegativeZero.SetSpamScore (-0.0);
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.score\":-0.0}}") == NegativeZero.Build(), "spam score: negative zero is written as -0.0");

    MailLog NegativeInfinity;

    NegativeInfinity.SetSpamScore (-std::numeric_limits<double>::infinity());
    Check (("{" + Scope + "}") == NegativeInfinity.Build(), "spam score: minus infinity has no JSON number either, it is not sent");
}


/* The biggest numbers, and the characters at the edge of what a JSON string has to escape */
static void TestExtremeValues()
{
    const int64_t Max = std::numeric_limits<int64_t>::max();

    MailLog Size;

    Size.SetSize (Max);
    Check (("{" + Scope + ",\"attributes\":{\"email.size_bytes\":9223372036854775807}}") == Size.Build(), "size: the biggest int64 is written with all its digits");

    MailLog Time;

    Time.SetTime         (Max);
    Time.SetObservedTime (Max);
    Check (("{" + Scope + ",\"time_unix_nano\":\"9223372036854775807\",\"observed_time_unix_nano\":\"9223372036854775807\"}") == Time.Build(),
           "time: the biggest int64 is written with all its digits");

    MailLog Nul;

    Nul.SetBody (std::string ("a\0b", 3));
    Check (("{" + Scope + ",\"body\":\"a\\u0000b\"}") == Nul.Build(), "escaping: a NUL character inside a text is \\u0000, the text does not end there");

    MailLog Del;

    Del.SetBody (std::string ("a\x7f" "b"));
    Check (("{" + Scope + ",\"body\":\"a\x7f" "b\"}") == Del.Build(), "escaping: 0x7F (DEL) is not a control character JSON needs escaped, it is written as it is");

    MailLog Invalid;

    Invalid.SetBody (std::string ("caf\xe9"));
    Check (("{" + Scope + ",\"body\":\"caf\xe9\"}") == Invalid.Build(),
           "escaping: bytes which are not valid UTF-8 are written as they are, the class does not check or change them (otelfwd makes a text valid when it reads a record)");

    MailLog Long;

    Long.SetBody (std::string (100000, 'x'));
    Check (("{" + Scope + ",\"body\":\"" + std::string (100000, 'x') + "\"}") == Long.Build(), "a body of 100000 characters is written completely");
}


/* The numbers, the booleans and the score are not in the table of texts: a second value replaces the first for them too, and false
   replaces true (which is not "empty" and not ignored) */
static void TestNumbersAndFlagsReplace()
{
    MailLog Numbers;

    Numbers.SetSize             (5);
    Numbers.SetSize             (7);
    Numbers.SetClientPort       (1);
    Numbers.SetClientPort       (2);
    Numbers.SetServerPort       (3);
    Numbers.SetServerPort       (4);
    Numbers.SetNetworkLocalPort (5);
    Numbers.SetNetworkLocalPort (6);
    Numbers.SetNetworkPeerPort  (7);
    Numbers.SetNetworkPeerPort  (8);
    Numbers.SetSmtpResponseCode (250);
    Numbers.SetSmtpResponseCode (550);
    Numbers.SetTime             (10);
    Numbers.SetTime             (20);
    Numbers.SetObservedTime     (30);
    Numbers.SetObservedTime     (40);

    Check (("{" + Scope + R"(,"time_unix_nano":"20","observed_time_unix_nano":"40","attributes":{"email.size_bytes":7,"client.port":2,"server.port":4)"
            R"(,"network.local.port":6,"network.peer.port":8,"email.smtp.response.code":550}})") == Numbers.Build(),
           "second value: a number replaces the first one, for every number setter");

    MailLog Flags;

    Flags.SetSigned        (true);
    Flags.SetSigned        (false);
    Flags.SetEncrypted     (false);
    Flags.SetEncrypted     (true);
    Flags.SetTlsEstablished (true);
    Flags.SetTlsEstablished (false);
    Flags.SetVirusChecked  (false);
    Flags.SetVirusChecked  (true);
    Check (("{" + Scope + R"(,"attributes":{"email.signed":false,"email.encrypted":true,"tls.established":false,"email.virus.checked":true}})") == Flags.Build(),
           "second value: a boolean replaces the first one, false replaces true and true replaces false");

    MailLog Score;

    Score.SetSpamScore (1.5);
    Score.SetSpamScore (2.5);
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.score\":2.5}}") == Score.Build(), "second value: a spam score replaces the first one");

    Score.SetSpamScore (std::numeric_limits<double>::quiet_NaN());
    Check (("{" + Scope + "}") == Score.Build(), "second value: a NaN score replaces the earlier score, so nothing is sent (the last score is the one which counts)");
}


static void TestSeverity()
{
    MailLog Top;

    Top.SetSeverity (24, "FATAL4");
    Check (("{" + Scope + ",\"severity_number\":24,\"severity_text\":\"FATAL4\"}") == Top.Build(), "severity: 24 is the highest OpenTelemetry severity number and is accepted");

    MailLog Above;

    Above.SetSeverity (25, "TOO HIGH");
    Check (("{" + Scope + "}") == Above.Build(), "severity: a number above 24 is not a severity number, nothing is sent, not the text either");

    /* The number and the text are one pair: a new call replaces both, an empty text does not keep the old one */
    MailLog Pair;

    Pair.SetSeverity (9, "INFO");
    Pair.SetSeverity (17, "");
    Check (("{" + Scope + ",\"severity_number\":17}") == Pair.Build(), "severity: number and text are one pair, the second call replaces both, an empty text leaves no text");

    Pair.SetSeverity (0, "IGNORED");
    Check (("{" + Scope + ",\"severity_number\":17}") == Pair.Build(), "severity: a number which is not valid changes nothing, not the number and not the text");
}


/* Two header names which become the same key after the normalization are one key: the values are collected in one array. The
   caller can not tell them apart afterwards, which is the price of a normalized name */
static void TestHeaderNameCollision()
{
    MailLog Log;

    Log.AddHeader ("X-A", "1");
    Log.AddHeader ("X_A", "2");
    Log.AddHeader ("x.a", "3");
    Check (("{" + Scope + R"(,"attributes":{"email.header.x_a":["1","2","3"]}})") == Log.Build(),
           "header: X-A, X_A and x.a are the same key after the normalization, one array with the values in the order they were added");
}


static void TestNegativeNumbers()
{
    MailLog Size;

    Size.SetSize (-5);
    Check (("{" + Scope + "}") == Size.Build(), "size: a negative number is not a size, it is ignored like 0");

    Size.SetSize (5);
    Size.SetSize (-1);
    Check (("{" + Scope + ",\"attributes\":{\"email.size_bytes\":5}}") == Size.Build(), "size: a negative number never replaces a value which was set");

    MailLog Attachment;

    Attachment.AddAttachment ("a.bin", -5);
    Check (("{" + Scope + ",\"attributes\":{\"email.attachments\":[{\"name\":\"a.bin\"}]}}") == Attachment.Build(),
           "attachment: a negative size is not known, like 0 - only the name is sent");

    MailLog Ports;

    Ports.SetClientPort       (-1);
    Ports.SetServerPort       (-1);
    Ports.SetNetworkLocalPort (-1);
    Ports.SetNetworkPeerPort  (0);
    Check (("{" + Scope + "}") == Ports.Build(), "client.port, server.port, network.local.port, network.peer.port: a number of 0 or less is not a port, it is ignored");

    MailLog Response;

    Response.SetSmtpResponseCode (-250);
    Check (("{" + Scope + "}") == Response.Build(), "email.smtp.response.code: a negative number is not a reply code, it is ignored");

    MailLog Severity;

    Severity.SetSeverity (-9, "INFO");
    Check (("{" + Scope + "}") == Severity.Build(), "severity: a negative number is unspecified like 0, nothing is sent, also not the text");

    MailLog Times;

    Times.SetTime         (-1);
    Times.SetObservedTime (-1);
    Check (("{" + Scope + "}") == Times.Build(), "time, observed time: a negative time is ignored like 0");
}


/* Without SetBody() Build() makes a short summary of what is there: event, action, sender, message id, number of recipients.
   Never the subject, never the recipients' addresses */
static void TestDefaultBody()
{
    MailLog Full;

    Full.SetEvent          ("scanned");
    Full.SetAction         ("quarantine");
    Full.SetEnvelopeFrom   ("attacker@example.net");
    Full.SetMessageId      ("<m@x>");
    Full.AddRecipient      ("bob@x");
    Full.SetSubject        ("secret");

    Check (("{" + Scope +
            R"(,"body":"scanned quarantine from=<attacker@example.net> message-id=<m@x> nrcpt=1")"
            R"(,"attributes":{"email.event":"scanned","email.action":"quarantine","email.envelope.from.address":"attacker@example.net")"
            R"(,"email.message.id":"<m@x>","email.subject":"secret","email.envelope.recipients":[{"address":"bob@x"}]}})") == Full.Build(),
           "default body: event, action, envelope sender, message id and the number of recipients - not the subject, not the address");

    MailLog Explicit;

    Explicit.SetFrom ("a@b");
    Explicit.SetBody ("my own text");
    Check (("{" + Scope + ",\"body\":\"my own text\",\"attributes\":{\"email.from.address\":\"a@b\"}}") == Explicit.Build(),
           "default body: a body which was set is used as it is, nothing is added to it");

    MailLog SubjectOnly;

    SubjectOnly.SetSubject ("only a subject");
    Check (("{" + Scope + ",\"attributes\":{\"email.subject\":\"only a subject\"}}") == SubjectOnly.Build(),
           "default body: nothing to summarize (the subject does not count), no body at all");

    MailLog HeaderFrom;

    HeaderFrom.SetFrom ("CEO <ceo@example.com>");
    Check (("{" + Scope + R"(,"body":"from=CEO <ceo@example.com>","attributes":{"email.from.address":"CEO <ceo@example.com>"}})") == HeaderFrom.Build(),
           "default body: the header From is used as it is, without more brackets, if there is no envelope sender");

    MailLog Both;

    Both.SetFrom         ("header@x");
    Both.SetEnvelopeFrom ("envelope@x");
    Check (("{" + Scope + R"(,"body":"from=<envelope@x>","attributes":{"email.from.address":"header@x","email.envelope.from.address":"envelope@x"}})") == Both.Build(),
           "default body: the envelope sender wins over the header From, as in a mail server's own log");

    MailLog Bounce;

    Bounce.SetFrom ("mailer-daemon@x");
    Bounce.SetEnvelopeFromEmpty();
    Check (("{" + Scope + R"(,"body":"from=<>","attributes":{"email.from.address":"mailer-daemon@x","email.envelope.from.address":""}})") == Bounce.Build(),
           "default body: a bounce's empty envelope sender is from=<>, not the header From");

    MailLog Headers;

    Headers.AddTo  ("a@x");
    Headers.AddTo  ("b@x");
    Headers.AddCc  ("c@x");
    Headers.AddBcc ("d@x");
    Check (("{" + Scope + R"(,"body":"nrcpt=4","attributes":{"email.to.addresses":["a@x","b@x"],"email.cc.addresses":["c@x"],"email.bcc.addresses":["d@x"]}})") == Headers.Build(),
           "default body: without an envelope the recipients are counted from To, Cc and Bcc together");

    MailLog Envelope;

    Envelope.AddTo        ("a@x");
    Envelope.AddTo        ("b@x");
    Envelope.AddTo        ("c@x");
    Envelope.AddRecipient ("a@x");
    Check (("{" + Scope + R"(,"body":"nrcpt=1","attributes":{"email.to.addresses":["a@x","b@x","c@x"],"email.envelope.recipients":[{"address":"a@x"}]}})") == Envelope.Build(),
           "default body: with an envelope its recipients are the ones counted, they are the ones the message went to");

    /* The parts are always in the same order (event, action, sender, message id, recipients), whatever order the methods were called in */
    MailLog Order;

    Order.AddRecipient ("a@x");
    Order.SetMessageId ("<m@x>");
    Order.SetFrom      ("f@x");
    Order.SetAction    ("quarantine");
    Order.SetEvent     ("scanned");
    Check (("{" + Scope + R"(,"body":"scanned quarantine from=f@x message-id=<m@x> nrcpt=1","attributes":{"email.message.id":"<m@x>","email.from.address":"f@x","email.action":"quarantine","email.event":"scanned","email.envelope.recipients":[{"address":"a@x"}]}})") == Order.Build(),
           "default body: event, action, sender, message id, recipients, in that order whatever order the methods were called in");

    MailLog EmptyBody;

    EmptyBody.SetFrom ("f@x");
    EmptyBody.SetBody ("");
    Check (("{" + Scope + R"(,"body":"from=f@x","attributes":{"email.from.address":"f@x"}})") == EmptyBody.Build(),
           "default body: an empty body is not a body, the default is used, like for a body which was never set");

    MailLog Escaped;

    Escaped.SetEnvelopeFrom ("a\"b");
    Check (("{" + Scope + R"(,"body":"from=<a\"b>","attributes":{"email.envelope.from.address":"a\"b"}})") == Escaped.Build(),
           "default body: escaped like every other text");
}


/* The escaping is one function, but it is called from more than one place: check the places, not only the body */
static void TestEscapingEverywhere()
{
    MailLog Attribute;

    Attribute.SetSubject ("a\"b");
    Check (("{" + Scope + R"(,"attributes":{"email.subject":"a\"b"}})") == Attribute.Build(), "escaping: a quote in an attribute value");

    MailLog Recipient;

    Recipient.AddTo ("x\\y");
    Recipient.AddTo ("z\"w");
    Check (("{" + Scope + R"(,"body":"nrcpt=2","attributes":{"email.to.addresses":["x\\y","z\"w"]}})") == Recipient.Build(), "escaping: in every value of an array");

    MailLog Header;

    Header.AddHeader ("X-Q", "line1\nline2");
    Check (("{" + Scope + R"(,"attributes":{"email.header.x_q":"line1\nline2"}})") == Header.Build(), "escaping: a new line in a header value");

    MailLog Resource;

    Resource.SetHostName ("a\"b");
    Resource.SetResourceAttribute ("k\"", "v\\");
    Check (("{" + Scope + R"(,"resource":{"host.name":"a\"b","k\"":"v\\"}})") == Resource.Build(),
           "escaping: in the resource, the key of SetResourceAttribute (used exactly as given) as well as the values");

    MailLog Attachment;

    Attachment.AddAttachment ("my \"file\".txt", 1, "", "text/plain");
    Check (("{" + Scope + R"(,"attributes":{"email.attachments":[{"name":"my \"file\".txt","size_bytes":1,"content_type":"text/plain"}]}})") == Attachment.Build(),
           "escaping: in an attachment name");
}


/* email.spam.score is written with std::to_chars: the shortest text which reads back as exactly the same double. Not "%g", which
   cuts a score to 6 digits and writes the locale's decimal separator, which is not a JSON number in a locale like de_DE */
static void TestSpamScoreFormatting()
{
    MailLog Precision;

    Precision.SetSpamScore (3.14159265);
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.score\":3.14159265}}") == Precision.Build(), "spam score: no digits are lost, 3.14159265 stays 3.14159265");

    MailLog Large;

    Large.SetSpamScore (1234567.5);
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.score\":1234567.5}}") == Large.Build(), "spam score: a score with more than 6 digits is not cut");

    MailLog Small;

    Small.SetSpamScore (0.1);
    Check (("{" + Scope + ",\"attributes\":{\"email.spam.score\":0.1}}") == Small.Build(), "spam score: 0.1 is written as 0.1, not 0.10000000000000001");

    /* A locale with a comma as the decimal separator, if this machine has one: the score must still have its point. If it has
       none (a minimal container often has only C and POSIX), the check cannot say anything, and says so instead of passing */
    const char *pszLocale = setlocale (LC_NUMERIC, "de_DE.UTF-8");

    if (NULL == pszLocale)
        pszLocale = setlocale (LC_NUMERIC, "de_DE");

    if (NULL != pszLocale)
    {
        MailLog Locale;

        Locale.SetSpamScore (3.2);
        Check (("{" + Scope + ",\"attributes\":{\"email.spam.score\":3.2}}") == Locale.Build(), "spam score: a point, not a comma, in a locale which has a decimal comma");

        setlocale (LC_NUMERIC, "C");
    }
    else
    {
        printf ("[SKIP]  spam score: no de_DE locale on this machine, the decimal comma check was not run\n");
    }
}


static void TestClearAndRepeat()
{
    MailLog Log;

    FillAll (Log);
    Log.SetEnvelopeFromEmpty();

    Log.Clear();
    Check (("{" + Scope + "}") == Log.Build(), "clear: empties the object, the next Build starts from nothing (the scope is still there)");

    Log.SetBody ("y");
    Check (("{" + Scope + ",\"body\":\"y\"}") == Log.Build(), "clear: the object can be used again for the next record");

    std::string First  = Log.Build();
    std::string Second = Log.Build();

    Check (First == Second, "build: calling it again without changing anything gives the same string");
    Check (("{" + Scope + ",\"body\":\"y\"}") == First, "build: and it is the expected one");
}


int main()
{
    setvbuf (stdout, NULL, _IOLBF, 0);

    Group ("MailLog unit test");

    Group ("Empty record");
    TestEmpty();

    Group ("Scope");
    TestScope();

    Group ("Resource");
    TestResource();

    Group ("Body, severity, time, and the fixed order of Build()");
    TestBodySeverityTime();

    Group ("Sender and recipients");
    TestRecipients();

    Group ("Envelope");
    TestEnvelope();

    Group ("Envelope recipients and their status");
    TestRecipientStatus();

    Group ("Subject and message id");
    TestSubjectAndMessageId();

    Group ("Queue id, event, direction");
    TestQueueIdEventAndDirection();

    Group ("Headers");
    TestHeaders();

    Group ("Escaping of the values");
    TestEscaping();

    Group ("Size");
    TestSize();

    Group ("Attachments");
    TestAttachments();

    Group ("Signed, encrypted, TLS");
    TestCryptoAndTls();

    Group ("Connection peer and SMTP session");
    TestConnectionAndSmtp();

    Group ("Spam and virus");
    TestSpamAndVirus();

    Group ("Authentication results");
    TestAuthResults();

    Group ("Classification, Auto-Submitted, action, reason, policy");
    TestClassificationAutoSubmittedActionAndPolicy();

    Group ("Every text setter: empty ignored, a second value");
    TestEmptyIgnored();

    Group ("Default body");
    TestDefaultBody();

    Group ("The biggest numbers, and the edge of the escaping");
    TestExtremeValues();

    Group ("A second value of a number, a boolean, the score");
    TestNumbersAndFlagsReplace();

    Group ("Severity");
    TestSeverity();

    Group ("Header names which become the same key");
    TestHeaderNameCollision();

    Group ("A negative number is ignored");
    TestNegativeNumbers();

    Group ("Escaping in every place a value is written");
    TestEscapingEverywhere();

    Group ("Spam score formatting");
    TestSpamScoreFormatting();
    TestSpamScoreIsAlwaysValidJson();

    Group ("Full record");
    TestFullRecord();

    Group ("Clear, and calling Build more than once");
    TestClearAndRepeat();

    if (false == g_FailedNames.empty())
    {
        Group ("Failed checks");

        for (const std::string& Name : g_FailedNames)
            printf ("[FAIL]  %s\n", Name.c_str());
    }

    Group ("Result");
    printf ("%s  %d of %d checks passed, %d failed\n\n", g_Failed ? "[FAIL]" : "[PASS]", g_Total - g_Failed, g_Total, g_Failed);

    return g_Failed ? 1 : 0;
}
