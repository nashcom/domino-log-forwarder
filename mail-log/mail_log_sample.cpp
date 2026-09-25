
/* mail_log_sample.cpp - how to use MailLog (mail_log.hpp) in another program.

   Build and run:   cd mail-log && make && ./mail_log_sample          (ten records: four common cases, a complete one using
                                                                        every field of the class, and five more: quarantined, rejected, two outcomes, deferred)
                     ./mail_log_sample 20                              (20 synthetic records instead, for a quick check of volume)

   Every record is written to stdout as one line. MailLog::Build() does not add the new line: this program does, the same way a
   caller which sends the record over a socket would (see domfwd_durable.hpp: "the sender adds a new line"). That means the
   output can be piped straight into a receiver which reads the flat record format, for example otelfwd's socket input:

     ./mail_log_sample | nc -N -U /local/notesdata/domino/otelfwd.sock

   (-N makes the OpenBSD netcat close the socket when the input ends, without it nc waits for the server for ever. With the
   traditional netcat use -q0 instead, or socat - UNIX-CONNECT:<socket>)

   (otelfwd needs OTLP_PUSH_API_URL, and a UNIX socket listening there - either the default one, or OTELFWD_UNIX_SOCKET. See the
   main README, "Socket inputs")

   The build is in the makefile next to this file. It shows what a program needs to use MailLog: mail_log.hpp and mail_log.cpp,
   nothing else. */

#include <stdio.h>
#include <stdlib.h>

#include <string>

#include "mail_log.hpp"


/* One example for every common case: a plain delivery (with the envelope matching the headers, and a resource: the mail server
   which may not be the machine otelfwd runs on), a bounce (the envelope sender empty by design), a message with several
   recipients (one of them only in the envelope, a Bcc), one with headers and an attachment, a complete one which uses every
   field of the class in a single record except SetEnvelopeFromEmpty (case 1) and SetVirusName (case 5), a UTF-8 subject too, and
   then a virus gateway's quarantined
   message, which is where SetVirusName is shown (and uses the default body), an inbound message rejected in the SMTP dialogue
   (failed SPF/DMARC, a real "false" for TLS, signature and virus scan), two records of one message whose recipients got
   different outcomes (cases 7 and 8), and mail sent out and deferred because the remote certificate expired (the TLS
   certificate). Called with an index from 0 on, returns false once there is none left */
static bool BuildExample (int Index, MailLog& retLog)
{
    retLog.Clear();

    switch (Index)
    {
        case 0:
            retLog.SetHostName     ("mail01.example.com");     /* the mail server, which may not be the machine otelfwd runs on */
            retLog.SetServiceName  ("postfix");
            retLog.SetFrom         ("alice@example.com");
            retLog.AddTo           ("bob@example.com");
            retLog.SetEnvelopeFrom ("alice@example.com");
            retLog.AddRecipient    ("bob@example.com", "delivered");     /* here the envelope matches the header: the plain case, with a status */
            retLog.SetSubject      ("Project update");
            retLog.SetMessageId    ("<1.1758000000@example.com>");
            retLog.SetQueueId      ("4X1abc-000001");
            retLog.SetDirection    ("outbound");
            retLog.SetSize         (2048);
            retLog.SetSeverity     (9, "INFO");
            retLog.SetBody         ("Delivered to bob@example.com");
            return true;

        case 1:
            retLog.SetFrom            ("mailer-daemon@example.com");
            retLog.AddTo              ("alice@example.com");
            retLog.SetEnvelopeFromEmpty();                     /* a bounce: MAIL FROM:<>, RFC 5321. Not the same as "not known" */
            retLog.AddRecipient       ("alice@example.com");
            retLog.SetSubject         ("Undeliverable: Project update");
            retLog.SetQueueId         ("4X1abc-000002");
            retLog.SetDirection       ("outbound");
            retLog.SetSeverity        (17, "ERROR");
            retLog.AddHeader          ("X-Failed-Recipients", "charlie@example.com");
            retLog.SetBody            ("550 5.1.1 user unknown");
            return true;

        case 2:
            retLog.SetFrom      ("newsletter@example.com");
            retLog.AddTo        ("bob@example.com");
            retLog.AddTo        ("charlie@example.com");
            retLog.AddTo        ("dora@example.com");
            retLog.AddCc        ("archive@example.com");
            retLog.AddRecipient ("bob@example.com");
            retLog.AddRecipient ("charlie@example.com");
            retLog.AddRecipient ("dora@example.com");
            retLog.AddRecipient ("archive@example.com");
            retLog.AddRecipient ("compliance-bcc@example.com");    /* a Bcc: an envelope recipient with no header at all */
            retLog.SetSubject        ("Monthly newsletter");
            retLog.SetQueueId        ("4X1abc-000003");
            retLog.SetDirection      ("outbound");
            retLog.SetClassification ("public");
            retLog.SetSeverity  (9, "INFO");
            retLog.SetBody      ("Sent to 5 envelope recipients, 4 of them visible in the headers");
            return true;

        case 3:
            retLog.SetFrom      ("app@example.com");
            retLog.AddTo        ("bob@example.com");
            retLog.SetSubject   ("Your report is ready");
            retLog.SetMessageId ("<2.1758000000@example.com>");
            retLog.AddHeader    ("Reply-To", "support@example.com");
            retLog.AddHeader    ("X-Mailer", "ReportApp 1.0");
            retLog.SetSeverity  (9, "INFO");
            retLog.SetBody      ("Delivered to bob@example.com");
            retLog.AddAttachment ("report.pdf", 88214, "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08", "application/pdf");
            retLog.SetSigned         (true);            /* signed, S/MIME. Not encrypted: SetEncrypted is not called, so it is
                                                             "not known" here, not "false" */
            retLog.SetCryptoProtocol ("smime");
            return true;

        case 4:
            /* Every field the class has, in one record: a complete example. Two methods are deliberately not used here:
               SetEnvelopeFromEmpty() - that is specifically for a bounce's null sender, already shown in case 1 - and
               SetVirusName(), which has nothing to report on a clean message and is shown instead in case 5, an infected
               message that gets quarantined. Signed and encrypted are both set here (unlike case 3, which leaves "encrypted"
               unknown on purpose). Also: the SPF/DKIM/DMARC results (with the domains SPF/DKIM checked), the TLS connection
               (a fact separate from the PGP encryption above), the connection's peer and the SMTP session (HELO, the accepted
               response), a spam score and result, a virus check, a classification, Auto-Submitted and the action taken */
            retLog.SetHostName          ("mail02.example.com");
            retLog.SetServiceName       ("exim");
            retLog.SetServiceNamespace  ("accounting");
            retLog.SetServiceInstanceId ("mail02-1");
            retLog.SetResourceAttribute ("cloud.region", "eu-central-1");

            retLog.SetFrom         ("buchhaltung@example.com");
            retLog.AddTo           ("kunde@example.com");
            retLog.AddCc           ("buchhaltung-archiv@example.com");
            retLog.AddBcc          ("controlling@example.com");
            retLog.SetEnvelopeFrom ("buchhaltung@example.com");
            retLog.AddRecipient    ("kunde@example.com", "delivered", 250, "2.0.0", "Message accepted for delivery");
            retLog.AddRecipient    ("buchhaltung-archiv@example.com", "delivered");
            retLog.AddRecipient    ("controlling@example.com");

            retLog.SetSubject   ("Rechnung f\xc3\xbcr Bestellung #42");     /* "Rechnung fuer Bestellung #42", the u-umlaut as UTF-8 */
            retLog.SetMessageId ("<3.1758000000@example.com>");
            retLog.SetQueueId   ("4X1abc-000004");
            retLog.SetEvent     ("delivery");
            retLog.SetDirection ("inbound");     /* this server, mail02, received it from an external client and delivers it: the server side below is mail02 itself */
            retLog.SetSize      (53500);

            retLog.AddAttachment ("Rechnung_42.pdf", 51302, "9f86d081884c7d659a2feaa0c55ad015a3bf4f1b2b0b822cd15d6c15b0f00a08", "application/pdf");

            retLog.AddHeader ("Date", "Thu, 24 Sep 2026 09:15:00 +0000");    /* the raw header, next to SetTime's parsed value below */
            retLog.AddHeader ("X-Invoice-Number", "RE-2026-0042");

            retLog.SetSigned         (true);
            retLog.SetEncrypted      (true);
            retLog.SetCryptoProtocol ("pgp");

            retLog.SetTlsEstablished     (true);      /* the SMTP connection itself - a different fact than the PGP encryption above */
            retLog.SetTlsProtocolName    ("tls");
            retLog.SetTlsProtocolVersion ("1.3");
            retLog.SetTlsCipher          ("TLS_ECDHE_RSA_WITH_AES_128_GCM_SHA256");
            retLog.SetTlsServerSubject   ("CN=mail02.example.com, O=Example");     /* the certificate of the connection: for mail received it is this
                                                                                    server's own, and a client certificate only with mutual TLS */
            retLog.SetTlsServerIssuer    ("CN=Example CA");
            retLog.SetTlsServerSha256    ("0687F666A054EF17A08E2F2162EAB4CBC0D265E1D7875BE74BF3C712CA92DAF0");
            retLog.SetTlsServerNotBefore ("2026-01-01T00:00:00.000Z");
            retLog.SetTlsServerNotAfter  ("2027-01-01T00:00:00.000Z");
            retLog.SetTlsClientSubject   ("CN=mail-client.example.net");
            retLog.SetTlsClientIssuer    ("CN=Example CA");
            retLog.SetTlsClientSha256    ("9E393D93138888D288266C2D915214D1D1CCEB2A9E393D93138888D288266C2D");
            retLog.SetTlsClientNotBefore ("2026-03-01T00:00:00.000Z");
            retLog.SetTlsClientNotAfter  ("2027-03-01T00:00:00.000Z");

            retLog.SetClientAddress ("203.0.113.10");     /* the logical client - not email.*, the same reasoning as tls.* above */
            retLog.SetClientPort    (51234);
            retLog.SetServerAddress ("mail02.example.com");
            retLog.SetServerPort    (25);

            retLog.SetNetworkLocalAddress ("192.0.2.20");     /* the actual TCP connection: here a gateway in front of the client */
            retLog.SetNetworkLocalPort    (25);
            retLog.SetNetworkPeerAddress  ("198.51.100.7");
            retLog.SetNetworkPeerPort     (41022);

            retLog.SetSmtpHelo            ("mail-client.example.net");     /* genuinely mail-specific, unlike the peer above */
            retLog.SetSmtpResponseCode    (250);
            retLog.SetSmtpEnhancedStatus  ("2.6.0");
            retLog.SetSmtpResponseText    ("Message accepted for delivery");

            retLog.SetSpfResult    ("pass");
            retLog.SetSpfDomain    ("example.com");
            retLog.SetDkimResult   ("pass");
            retLog.SetDkimDomain   ("example.com");
            retLog.SetDkimSelector ("selector1");
            retLog.SetDmarcResult  ("pass");

            retLog.SetSpamScore  (0.1);
            retLog.SetSpamResult ("ham");
            retLog.SetSpamEngine ("Rspamd");

            retLog.SetVirusChecked (true);
            retLog.SetVirusResult  ("clean");
            retLog.SetVirusEngine  ("ClamAV");

            retLog.SetAction     ("deliver");
            retLog.SetReason     ("SPF/DKIM/DMARC pass, no policy matched");
            retLog.SetPolicyName ("Default-Inbound");
            retLog.SetPolicyId   ("POL-0001");

            retLog.SetClassification ("confidential");
            retLog.SetAutoSubmitted  ("no");     /* explicitly not automated: a human in accounting sent this */

            retLog.SetBody         ("Delivered to kunde@example.com");
            retLog.SetSeverity     (9, "INFO");
            retLog.SetTime         (1790241300000000000LL);      /* 2026-09-24T09:15:00Z, the message's own time (the Date header) */
            retLog.SetObservedTime (1790241303000000000LL);      /* 2026-09-24T09:15:03Z, a few seconds later: when this was logged */
            return true;

        case 5:
            /* An infected message: SetVirusName(), the one field left out of case 4, together with the action, reason and
               policy a virus gateway would set for a message it quarantines instead of delivering */
            retLog.SetFrom      ("attacker@example.net");
            retLog.AddTo        ("bob@example.com");
            retLog.SetSubject   ("Invoice attached");
            retLog.SetQueueId   ("4X1abc-000005");
            retLog.SetEvent     ("scanned");
            retLog.SetDirection ("inbound");

            retLog.AddAttachment ("invoice.exe", 184320, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "application/x-msdownload");

            retLog.SetVirusChecked (true);
            retLog.SetVirusResult  ("infected");
            retLog.SetVirusName    ("Win32/Whatever");
            retLog.SetVirusEngine  ("ClamAV");

            retLog.SetAction     ("quarantine");
            retLog.SetReason     ("Attachment matched AV signature");
            retLog.SetPolicyName ("AV-Block-Executables");
            retLog.SetPolicyId   ("POL-1042");

            retLog.SetSeverity (13, "WARN");
            /* No SetBody: Build() makes the body from what is there, here "scanned quarantine from=attacker@example.net nrcpt=1"
               (no subject, no recipient address). The other cases set their own text, which is what a producer should do
               when it has a log line of its own */
            return true;

        case 6:
            /* An inbound message rejected during the SMTP dialogue, before it was queued or scanned: the SMTP reply and the
               policy which decided it, failed sender authentication, and "false" as a real value - the producer knows that
               there was no TLS, no signature and no virus scan (email.virus.checked false, which is not "not known"). No queue
               id: the message never got one */
            retLog.SetHostName     ("mx01.example.com");
            retLog.SetServiceName  ("exim");

            retLog.SetFrom         ("ceo@example.com");                /* what the message claims ... */
            retLog.SetEnvelopeFrom ("bounce@bulk.example.net");        /* ... and what the SMTP dialogue says */
            retLog.AddRecipient    ("bob@example.com", "rejected", 550, "5.7.1", "SPF check failed");
            retLog.SetEvent        ("rejected");
            retLog.SetDirection    ("inbound");

            retLog.SetClientAddress ("192.0.2.77");
            retLog.SetClientPort    (40512);
            retLog.SetServerAddress ("mx01.example.com");
            retLog.SetServerPort    (25);
            retLog.SetSmtpHelo      ("bulk.example.net");
            retLog.SetTlsEstablished (false);

            retLog.SetSmtpResponseCode   (550);
            retLog.SetSmtpEnhancedStatus ("5.7.1");
            retLog.SetSmtpResponseText   ("SPF check failed");

            retLog.SetSpfResult   ("fail");
            retLog.SetSpfDomain   ("bulk.example.net");
            retLog.SetDkimResult  ("none");
            retLog.SetDmarcResult ("fail");

            retLog.SetSpamScore  (8.4);
            retLog.SetSpamResult ("spam");
            retLog.SetSpamEngine ("Rspamd");

            retLog.SetSigned       (false);
            retLog.SetVirusChecked (false);      /* rejected before the scan: the producer knows no scan happened */

            retLog.SetAction     ("reject");
            retLog.SetReason     ("DMARC policy reject");
            retLog.SetPolicyName ("Inbound-DMARC");
            retLog.SetPolicyId   ("POL-0007");

            retLog.SetSeverity (13, "WARN");
            retLog.SetBody     ("550 5.7.1 SPF check failed");
            return true;

        case 7:
        case 8:
            /* Recipients which get different outcomes for the same message: one record per outcome, tied together by the same
               queue id and message id (cases 7 and 8). Each record has its own recipient, its own status and its own action.
               Case 0 shows the other way: one record, with the status of the recipient inside the recipient */
            retLog.SetHostName  ("mail01.example.com");
            retLog.SetServiceName ("postfix");
            retLog.SetFrom      ("alice@example.com");
            retLog.SetMessageId ("<7.1758000000@example.com>");
            retLog.SetQueueId   ("4X1abc-000007");
            retLog.SetDirection ("outbound");

            if (7 == Index)
            {
                retLog.SetEvent      ("delivery");
                retLog.AddRecipient  ("bob@example.com", "delivered", 250, "2.0.0", "OK queued");
                retLog.SetAction     ("deliver");
                retLog.SetSeverity   (9, "INFO");
                retLog.SetBody       ("to=<bob@example.com>, status=sent (250 2.0.0 OK queued)");     /* a Postfix style log line */
            }
            else
            {
                retLog.SetEvent      ("bounce");
                retLog.AddRecipient  ("charlie@example.com", "bounced", 550, "5.1.1", "user unknown");
                retLog.SetAction     ("reject");
                retLog.SetReason     ("Recipient does not exist");
                retLog.SetSeverity   (17, "ERROR");
                retLog.SetBody       ("to=<charlie@example.com>, status=bounced (550 5.1.1 user unknown)");
            }

            return true;

        case 9:
            /* Mail sent out, deferred because of the certificate of the remote server: this side is the SMTP client, so the
               certificate is tls.server.*, and the connection to look at is the remote one. The reason is the mail system's own
               decision, there is no SMTP reply from the remote server for it (so no email.smtp.response.*) */
            retLog.SetHostName  ("mail01.example.com");
            retLog.SetServiceName ("postfix");
            retLog.SetFrom      ("alice@example.com");
            retLog.SetMessageId ("<9.1758000000@example.com>");
            retLog.SetQueueId   ("4X1abc-000009");
            retLog.SetEvent     ("deferred");
            retLog.SetDirection ("outbound");

            retLog.AddRecipient ("partner@example.org", "deferred");

            retLog.SetClientAddress       ("mail01.example.com");
            retLog.SetServerAddress       ("mx.example.org");                /* the logical server, a name */
            retLog.SetServerPort          (25);
            retLog.SetNetworkPeerAddress  ("203.0.113.44");                  /* the socket endpoint it resolved to */
            retLog.SetNetworkPeerPort     (25);
            retLog.SetTlsEstablished      (false);                           /* the handshake was not accepted */
            retLog.SetTlsProtocolVersion  ("1.3");
            retLog.SetTlsServerSubject    ("CN=mx.example.org, O=Example Org");
            retLog.SetTlsServerIssuer     ("CN=Example Org CA");
            retLog.SetTlsServerSha256     ("9E393D93138888D288266C2D915214D1D1CCEB2A9E393D93138888D288266C2D");
            retLog.SetTlsServerNotBefore  ("2025-08-01T00:00:00.000Z");
            retLog.SetTlsServerNotAfter   ("2026-08-01T00:00:00.000Z");      /* in the past */

            retLog.SetAction     ("defer");
            retLog.SetReason     ("Certificate of the remote server has expired");
            retLog.SetPolicyName ("MTA-STS enforce");

            retLog.SetSeverity (13, "WARN");
            retLog.SetBody     ("delivery to mx.example.org deferred: certificate expired on 2026-08-01");
            return true;

        default:
            return false;
    }
}


/* COUNT synthetic records, for a quick check of volume rather than of the fields */
static void BuildSynthetic (int Index, MailLog& retLog)
{
    std::string Number = std::to_string (Index);

    retLog.Clear();
    retLog.SetFrom     ("sender@example.com");
    retLog.AddTo       ("recipient" + Number + "@example.com");
    retLog.SetSubject  ("Test message " + Number);
    retLog.SetSize     (static_cast<int64_t> (100 + Index));
    retLog.SetSeverity (9, "INFO");
    retLog.SetBody     ("generated test record " + Number);
}


int main (int argc, char *argv[])
{
    MailLog Log;

    if (argc > 1)
    {
        int Count = atoi (argv[1]);

        if (Count <= 0)
        {
            fprintf (stderr, "Usage: %s [COUNT]\n", argv[0]);
            return 1;
        }

        for (int i = 1; i <= Count; i++)
        {
            BuildSynthetic (i, Log);
            printf ("%s\n", Log.Build().c_str());
        }

        return 0;
    }

    for (int i = 0; BuildExample (i, Log); i++)
        printf ("%s\n", Log.Build().c_str());

    return 0;
}
