
/* MailLog - see mail_log.hpp */

#include "mail_log.hpp"

#include <math.h>
#include <stdio.h>

#include <charconv>


void MailLog::SetHostName (const std::string& Name)
{
    if (Name.empty())
        return;

    SetValue (m_Resource, "host.name", Name);
}


void MailLog::SetServiceName (const std::string& Name)
{
    if (Name.empty())
        return;

    SetValue (m_Resource, "service.name", Name);
}


void MailLog::SetServiceNamespace (const std::string& Namespace)
{
    if (Namespace.empty())
        return;

    SetValue (m_Resource, "service.namespace", Namespace);
}


void MailLog::SetServiceInstanceId (const std::string& Id)
{
    if (Id.empty())
        return;

    SetValue (m_Resource, "service.instance.id", Id);
}


void MailLog::SetResourceAttribute (const std::string& Key, const std::string& Value)
{
    if (Key.empty() || Value.empty())
        return;

    SetValue (m_Resource, Key, Value);
}


void MailLog::SetFrom (const std::string& Address)
{
    if (Address.empty())
        return;

    SetValue (m_Attributes, "email.from.address", Address);
}


void MailLog::AddTo (const std::string& Address)
{
    if (Address.empty())
        return;

    AddValue (m_Attributes, "email.to.addresses", Address);
}


void MailLog::AddCc (const std::string& Address)
{
    if (Address.empty())
        return;

    AddValue (m_Attributes, "email.cc.addresses", Address);
}


void MailLog::AddBcc (const std::string& Address)
{
    if (Address.empty())
        return;

    AddValue (m_Attributes, "email.bcc.addresses", Address);
}


void MailLog::SetEnvelopeFrom (const std::string& Address)
{
    if (Address.empty())
        return;

    SetValue (m_Attributes, "email.envelope.from.address", Address);
}


void MailLog::SetEnvelopeFromEmpty()
{
    /* Bypasses the empty check on purpose: an empty envelope sender is the value here, not "nothing to set" */
    SetValue (m_Attributes, "email.envelope.from.address", "");
}


void MailLog::AddRecipient (const std::string& Address, const std::string& Status, int64_t Code, const std::string& EnhancedStatus, const std::string& Text)
{
    if (Address.empty())
        return;

    Recipient Entry;

    Entry.Address        = Address;
    Entry.Status         = Status;
    Entry.Code           = (Code > 0) ? Code : 0;
    Entry.EnhancedStatus = EnhancedStatus;
    Entry.Text           = Text;

    m_Recipients.push_back (Entry);
}


void MailLog::SetSubject (const std::string& Subject)
{
    if (Subject.empty())
        return;

    SetValue (m_Attributes, "email.subject", Subject);
}


void MailLog::SetMessageId (const std::string& Id)
{
    if (Id.empty())
        return;

    SetValue (m_Attributes, "email.message.id", Id);
}


void MailLog::SetQueueId (const std::string& Id)
{
    if (Id.empty())
        return;

    SetValue (m_Attributes, "email.queue.id", Id);
}


void MailLog::SetEvent (const std::string& Event)
{
    if (Event.empty())
        return;

    SetValue (m_Attributes, "email.event", Event);
}


void MailLog::SetDirection (const std::string& Direction)
{
    if (Direction.empty())
        return;

    SetValue (m_Attributes, "email.direction", Direction);
}


void MailLog::SetSize (int64_t Bytes)
{
    if (Bytes <= 0)
        return;

    m_Size = Bytes;
}


void MailLog::AddAttachment (const std::string& Name, int64_t Size, const std::string& Sha256, const std::string& ContentType)
{
    if (Name.empty())
        return;

    Attachment Entry;

    Entry.Name        = Name;
    Entry.Size        = (Size > 0) ? Size : 0;
    Entry.Sha256      = Sha256;
    Entry.ContentType = ContentType;

    m_Attachments.push_back (Entry);
}


void MailLog::AddHeader (const std::string& Name, const std::string& Value)
{
    if (Name.empty() || Value.empty())
        return;

    AddValue (m_Attributes, "email.header." + NormalizeHeaderName (Name), Value);
}


void MailLog::SetSigned (bool Value)
{
    m_bSigned    = Value;
    m_bSignedSet = true;
}


void MailLog::SetEncrypted (bool Value)
{
    m_bEncrypted    = Value;
    m_bEncryptedSet = true;
}


void MailLog::SetCryptoProtocol (const std::string& Protocol)
{
    if (Protocol.empty())
        return;

    SetValue (m_Attributes, "email.crypto.protocol.name", Protocol);
}


void MailLog::SetTlsEstablished (bool Value)
{
    m_bTlsEstablished    = Value;
    m_bTlsEstablishedSet = true;
}


void MailLog::SetTlsProtocolName (const std::string& Name)
{
    if (Name.empty())
        return;

    SetValue (m_Attributes, "tls.protocol.name", Name);
}


void MailLog::SetTlsProtocolVersion (const std::string& Version)
{
    if (Version.empty())
        return;

    SetValue (m_Attributes, "tls.protocol.version", Version);
}


void MailLog::SetTlsCipher (const std::string& Cipher)
{
    if (Cipher.empty())
        return;

    SetValue (m_Attributes, "tls.cipher", Cipher);
}


void MailLog::SetTlsClientSubject (const std::string& Subject)
{
    if (Subject.empty())
        return;

    SetValue (m_Attributes, "tls.client.subject", Subject);
}


void MailLog::SetTlsClientIssuer (const std::string& Issuer)
{
    if (Issuer.empty())
        return;

    SetValue (m_Attributes, "tls.client.issuer", Issuer);
}


void MailLog::SetTlsClientSha256 (const std::string& Hash)
{
    if (Hash.empty())
        return;

    SetValue (m_Attributes, "tls.client.hash.sha256", Hash);
}


void MailLog::SetTlsClientNotBefore (const std::string& Time)
{
    if (Time.empty())
        return;

    SetValue (m_Attributes, "tls.client.not_before", Time);
}


void MailLog::SetTlsClientNotAfter (const std::string& Time)
{
    if (Time.empty())
        return;

    SetValue (m_Attributes, "tls.client.not_after", Time);
}


void MailLog::SetTlsServerSubject (const std::string& Subject)
{
    if (Subject.empty())
        return;

    SetValue (m_Attributes, "tls.server.subject", Subject);
}


void MailLog::SetTlsServerIssuer (const std::string& Issuer)
{
    if (Issuer.empty())
        return;

    SetValue (m_Attributes, "tls.server.issuer", Issuer);
}


void MailLog::SetTlsServerSha256 (const std::string& Hash)
{
    if (Hash.empty())
        return;

    SetValue (m_Attributes, "tls.server.hash.sha256", Hash);
}


void MailLog::SetTlsServerNotBefore (const std::string& Time)
{
    if (Time.empty())
        return;

    SetValue (m_Attributes, "tls.server.not_before", Time);
}


void MailLog::SetTlsServerNotAfter (const std::string& Time)
{
    if (Time.empty())
        return;

    SetValue (m_Attributes, "tls.server.not_after", Time);
}


void MailLog::SetClientAddress (const std::string& Address)
{
    if (Address.empty())
        return;

    SetValue (m_Attributes, "client.address", Address);
}


void MailLog::SetClientPort (int64_t Port)
{
    if (Port <= 0)
        return;

    m_ClientPort = Port;
}


void MailLog::SetServerAddress (const std::string& Address)
{
    if (Address.empty())
        return;

    SetValue (m_Attributes, "server.address", Address);
}


void MailLog::SetServerPort (int64_t Port)
{
    if (Port <= 0)
        return;

    m_ServerPort = Port;
}


void MailLog::SetNetworkLocalAddress (const std::string& Address)
{
    if (Address.empty())
        return;

    SetValue (m_Attributes, "network.local.address", Address);
}


void MailLog::SetNetworkLocalPort (int64_t Port)
{
    if (Port <= 0)
        return;

    m_NetworkLocalPort = Port;
}


void MailLog::SetNetworkPeerAddress (const std::string& Address)
{
    if (Address.empty())
        return;

    SetValue (m_Attributes, "network.peer.address", Address);
}


void MailLog::SetNetworkPeerPort (int64_t Port)
{
    if (Port <= 0)
        return;

    m_NetworkPeerPort = Port;
}


void MailLog::SetSmtpHelo (const std::string& Helo)
{
    if (Helo.empty())
        return;

    SetValue (m_Attributes, "email.smtp.helo", Helo);
}


void MailLog::SetSmtpResponseCode (int64_t Code)
{
    if (Code <= 0)
        return;

    m_SmtpResponseCode = Code;
}


void MailLog::SetSmtpEnhancedStatus (const std::string& Status)
{
    if (Status.empty())
        return;

    SetValue (m_Attributes, "email.smtp.response.enhanced_status_code", Status);
}


void MailLog::SetSmtpResponseText (const std::string& Text)
{
    if (Text.empty())
        return;

    SetValue (m_Attributes, "email.smtp.response.text", Text);
}


void MailLog::SetSpamScore (double Score)
{
    m_SpamScore     = Score;
    m_bSpamScoreSet = true;
}


void MailLog::SetSpamResult (const std::string& Result)
{
    if (Result.empty())
        return;

    SetValue (m_Attributes, "email.spam.result", Result);
}


void MailLog::SetSpfResult (const std::string& Result)
{
    if (Result.empty())
        return;

    SetValue (m_Attributes, "email.spf.result", Result);
}


void MailLog::SetDkimResult (const std::string& Result)
{
    if (Result.empty())
        return;

    SetValue (m_Attributes, "email.dkim.result", Result);
}


void MailLog::SetDmarcResult (const std::string& Result)
{
    if (Result.empty())
        return;

    SetValue (m_Attributes, "email.dmarc.result", Result);
}


void MailLog::SetSpfDomain (const std::string& Domain)
{
    if (Domain.empty())
        return;

    SetValue (m_Attributes, "email.spf.domain", Domain);
}


void MailLog::SetDkimDomain (const std::string& Domain)
{
    if (Domain.empty())
        return;

    SetValue (m_Attributes, "email.dkim.domain", Domain);
}


void MailLog::SetDkimSelector (const std::string& Selector)
{
    if (Selector.empty())
        return;

    SetValue (m_Attributes, "email.dkim.selector", Selector);
}


void MailLog::SetVirusChecked (bool Value)
{
    m_bVirusChecked    = Value;
    m_bVirusCheckedSet = true;
}


void MailLog::SetVirusResult (const std::string& Result)
{
    if (Result.empty())
        return;

    SetValue (m_Attributes, "email.virus.result", Result);
}


void MailLog::SetSpamEngine (const std::string& Engine)
{
    if (Engine.empty())
        return;

    SetValue (m_Attributes, "email.spam.engine.name", Engine);
}


void MailLog::SetVirusEngine (const std::string& Engine)
{
    if (Engine.empty())
        return;

    SetValue (m_Attributes, "email.virus.engine.name", Engine);
}


void MailLog::SetVirusName (const std::string& Name)
{
    if (Name.empty())
        return;

    SetValue (m_Attributes, "email.virus.threat.name", Name);
}


void MailLog::SetClassification (const std::string& Classification)
{
    if (Classification.empty())
        return;

    SetValue (m_Attributes, "email.classification", Classification);
}


void MailLog::SetAutoSubmitted (const std::string& Value)
{
    if (Value.empty())
        return;

    SetValue (m_Attributes, "email.auto_submitted", Value);
}


void MailLog::SetAction (const std::string& Action)
{
    if (Action.empty())
        return;

    SetValue (m_Attributes, "email.action", Action);
}


void MailLog::SetReason (const std::string& Reason)
{
    if (Reason.empty())
        return;

    SetValue (m_Attributes, "email.reason", Reason);
}


void MailLog::SetPolicyName (const std::string& Name)
{
    if (Name.empty())
        return;

    SetValue (m_Attributes, "email.policy.name", Name);
}


void MailLog::SetPolicyId (const std::string& Id)
{
    if (Id.empty())
        return;

    SetValue (m_Attributes, "email.policy.id", Id);
}


void MailLog::SetBody (const std::string& Text)
{
    if (Text.empty())
        return;

    m_Body = Text;
}


void MailLog::SetSeverity (int64_t Number, const std::string& Text)
{
    if ( (Number <= 0) || (Number > 24) )
        return;

    m_SeverityNumber = Number;
    m_SeverityText   = Text;
}


void MailLog::SetTime (int64_t TimeUnixNano)
{
    if (TimeUnixNano <= 0)
        return;

    m_TimeUnixNano = TimeUnixNano;
}


void MailLog::SetObservedTime (int64_t TimeUnixNano)
{
    if (TimeUnixNano <= 0)
        return;

    m_ObservedTimeUnixNano = TimeUnixNano;
}


void MailLog::Clear()
{
    m_Body.clear();
    m_SeverityNumber = 0;
    m_SeverityText.clear();
    m_TimeUnixNano         = 0;
    m_ObservedTimeUnixNano = 0;
    m_Size             = 0;
    m_ClientPort       = 0;
    m_ServerPort       = 0;
    m_NetworkLocalPort = 0;
    m_NetworkPeerPort  = 0;
    m_SmtpResponseCode = 0;
    m_bSigned       = false;
    m_bSignedSet    = false;
    m_bEncrypted    = false;
    m_bEncryptedSet = false;
    m_bTlsEstablished    = false;
    m_bTlsEstablishedSet = false;
    m_SpamScore        = 0.0;
    m_bSpamScoreSet    = false;
    m_bVirusChecked    = false;
    m_bVirusCheckedSet = false;
    m_Resource.clear();
    m_Attributes.clear();
    m_Recipients.clear();
    m_Attachments.clear();
}


/* The schema this class builds: SCHEMA.md is the source of truth, this is only the scope which says so in the record itself */
#define MAIL_LOG_SCHEMA_NAME    "mail-log"
#define MAIL_LOG_SCHEMA_VERSION "0.1.0"


std::string MailLog::Build() const
{
    std::vector<std::string> Fields;

    /* Always there, whether or not anything else was set: it says which schema the record follows, not what the message was */
    Fields.push_back ("\"scope\":{\"name\":\"" MAIL_LOG_SCHEMA_NAME "\",\"version\":\"" MAIL_LOG_SCHEMA_VERSION "\"}");

    /* Only if at least one resource field was set. otelfwd does not merge this with its own default resource: see mail_log.hpp */
    if (false == m_Resource.empty())
    {
        std::string Field = "\"resource\":";

        AppendResource (Field, m_Resource);
        Fields.push_back (Field);
    }

    if (0 != m_TimeUnixNano)
        Fields.push_back ("\"time_unix_nano\":\"" + std::to_string (m_TimeUnixNano) + "\"");

    if (0 != m_ObservedTimeUnixNano)
        Fields.push_back ("\"observed_time_unix_nano\":\"" + std::to_string (m_ObservedTimeUnixNano) + "\"");

    if (0 != m_SeverityNumber)
    {
        Fields.push_back ("\"severity_number\":" + std::to_string (m_SeverityNumber));

        if (false == m_SeverityText.empty())
        {
            std::string Field = "\"severity_text\":";

            AppendJsonString (Field, m_SeverityText);
            Fields.push_back (Field);
        }
    }

    /* The body the caller set, else a short summary of what is there (BuildDefaultBody), else none */
    std::string Body = m_Body.empty() ? BuildDefaultBody() : m_Body;

    if (false == Body.empty())
    {
        std::string Field = "\"body\":";

        AppendJsonString (Field, Body);
        Fields.push_back (Field);
    }

    /* The attributes: email.from.address, email.to.addresses, email.cc.addresses, email.bcc.addresses, email.envelope.from.address, email.subject, email.message.id,
       email.queue.id, email.event, email.direction, email.crypto.protocol.name, tls.protocol.name, tls.protocol.version, tls.cipher, tls.client.subject, tls.client.issuer,
       tls.client.hash.sha256, tls.client.not_before, tls.client.not_after, tls.server.subject, tls.server.issuer, tls.server.hash.sha256,
       tls.server.not_before, tls.server.not_after,
       client.address, server.address, network.local.address, network.peer.address, email.smtp.helo, email.smtp.response.enhanced_status_code, email.smtp.response.text,
       email.spam.result, email.spam.engine.name, email.spf.result, email.dkim.result, email.dmarc.result, email.spf.domain, email.dkim.domain,
       email.dkim.selector, email.virus.result, email.virus.threat.name, email.virus.engine.name, email.classification, email.auto_submitted, email.action,
       email.reason, email.policy.name, email.policy.id and the headers, in the order their key was first used, then
       email.size_bytes, email.attachments, email.envelope.recipients, client.port, server.port, network.local.port, network.peer.port, email.smtp.response.code,
       email.signed, email.encrypted, tls.established, email.spam.score and email.virus.checked, always in that order */
    std::vector<std::string> AttributeFields;

    for (const auto& Entry : m_Attributes)
    {
        std::string Field;

        AppendJsonString (Field, Entry.first);
        Field += ':';

        if ( (1 == Entry.second.size()) && (false == IsRecipientKey (Entry.first)) )
        {
            AppendJsonString (Field, Entry.second[0]);
        }
        else
        {
            Field += '[';

            for (size_t i = 0; i < Entry.second.size(); i++)
            {
                if (i > 0)
                    Field += ',';

                AppendJsonString (Field, Entry.second[i]);
            }

            Field += ']';
        }

        AttributeFields.push_back (Field);
    }

    if (0 != m_Size)
        AttributeFields.push_back ("\"email.size_bytes\":" + std::to_string (m_Size));

    if (false == m_Attachments.empty())
    {
        std::string Field = "\"email.attachments\":[";

        for (size_t i = 0; i < m_Attachments.size(); i++)
        {
            if (i > 0)
                Field += ',';

            AppendAttachment (Field, m_Attachments[i]);
        }

        Field += ']';
        AttributeFields.push_back (Field);
    }

    if (false == m_Recipients.empty())
    {
        std::string Field = "\"email.envelope.recipients\":[";

        for (size_t i = 0; i < m_Recipients.size(); i++)
        {
            if (i > 0)
                Field += ',';

            AppendRecipient (Field, m_Recipients[i]);
        }

        Field += ']';
        AttributeFields.push_back (Field);
    }

    /* Plain JSON numbers, not strings, the same rule as email.size_bytes above: 0 is never a valid port number or SMTP reply
       code, so it means "not set" here too */
    if (0 != m_ClientPort)
        AttributeFields.push_back ("\"client.port\":" + std::to_string (m_ClientPort));

    if (0 != m_ServerPort)
        AttributeFields.push_back ("\"server.port\":" + std::to_string (m_ServerPort));

    if (0 != m_NetworkLocalPort)
        AttributeFields.push_back ("\"network.local.port\":" + std::to_string (m_NetworkLocalPort));

    if (0 != m_NetworkPeerPort)
        AttributeFields.push_back ("\"network.peer.port\":" + std::to_string (m_NetworkPeerPort));

    if (0 != m_SmtpResponseCode)
        AttributeFields.push_back ("\"email.smtp.response.code\":" + std::to_string (m_SmtpResponseCode));

    /* Real JSON booleans, not strings. false is a meaningful value, not "unspecified": these are only sent if SetSigned()/
       SetEncrypted() was actually called */
    if (m_bSignedSet)
        AttributeFields.push_back (std::string ("\"email.signed\":") + (m_bSigned ? "true" : "false"));

    if (m_bEncryptedSet)
        AttributeFields.push_back (std::string ("\"email.encrypted\":") + (m_bEncrypted ? "true" : "false"));

    if (m_bTlsEstablishedSet)
        AttributeFields.push_back (std::string ("\"tls.established\":") + (m_bTlsEstablished ? "true" : "false"));

    /* A plain JSON number, not a string. NaN and infinite have no JSON number: silently not sent, the one case SetSpamScore()
       does not always take effect */
    if (m_bSpamScoreSet && isfinite (m_SpamScore))
    {
        /* to_chars: always a "." and never the locale's decimal separator, and the shortest text which reads back as exactly
           this double (3.14159265 stays 3.14159265, where "%g" would cut it to 6 digits). Its output is valid JSON */
        char szScore[64] = {0};

        std::to_chars_result Result = std::to_chars (szScore, szScore + sizeof (szScore) - 1, m_SpamScore);

        *Result.ptr = '\0';

        /* A score which is a whole number is written with ".0" (3.0, not 3): a reader which types a number by its text, otelfwd
           does, would otherwise send it as an integer, and the same attribute would be an int in one record and a double in the
           next. A number with an exponent ("1e+10") is a double already */
        std::string Score = szScore;

        if (std::string::npos == Score.find_first_of (".eE"))
            Score += ".0";

        AttributeFields.push_back (std::string ("\"email.spam.score\":") + Score);
    }

    if (m_bVirusCheckedSet)
        AttributeFields.push_back (std::string ("\"email.virus.checked\":") + (m_bVirusChecked ? "true" : "false"));

    if (false == AttributeFields.empty())
    {
        std::string Field = "\"attributes\":{";

        for (size_t i = 0; i < AttributeFields.size(); i++)
        {
            if (i > 0)
                Field += ',';

            Field += AttributeFields[i];
        }

        Field += '}';
        Fields.push_back (Field);
    }

    std::string Json = "{";

    for (size_t i = 0; i < Fields.size(); i++)
    {
        if (i > 0)
            Json += ',';

        Json += Fields[i];
    }

    Json += '}';

    return Json;
}


/* The body when the caller set none, so a log viewer has a line to show: what happened (email.event, email.action), from whom
   (the envelope sender if there is one, "<>" for a bounce, else the header From), which message (email.message.id) and to how
   many recipients (a count, not the addresses: a list can be long). Only what is there: nothing there, no body. Never the
   subject: it can be long and can hold personal data. For example:
   scanned quarantine from=<attacker@example.net> message-id=<2.1758000000@example.com> nrcpt=1 */
std::string MailLog::BuildDefaultBody() const
{
    std::string Text;

    auto AddPart = [&Text] (const std::string& Part)
    {
        if (false == Text.empty())
            Text += ' ';

        Text += Part;
    };

    const std::vector<std::string> *pEvent = FindValues (m_Attributes, "email.event");

    if (pEvent)
        AddPart ((*pEvent)[0]);

    const std::vector<std::string> *pAction = FindValues (m_Attributes, "email.action");

    if (pAction)
        AddPart ((*pAction)[0]);

    const std::vector<std::string> *pEnvelopeFrom = FindValues (m_Attributes, "email.envelope.from.address");
    const std::vector<std::string> *pFrom         = FindValues (m_Attributes, "email.from.address");

    if (pEnvelopeFrom)
        AddPart ("from=<" + (*pEnvelopeFrom)[0] + ">");
    else if (pFrom)
        AddPart ("from=" + (*pFrom)[0]);

    const std::vector<std::string> *pMessageId = FindValues (m_Attributes, "email.message.id");

    if (pMessageId)
        AddPart ("message-id=" + (*pMessageId)[0]);

    size_t Recipients = 0;

    if (false == m_Recipients.empty())
    {
        Recipients = m_Recipients.size();
    }
    else
    {
        const char *pszHeaders[] = { "email.to.addresses", "email.cc.addresses", "email.bcc.addresses" };

        for (const char *pszHeader : pszHeaders)
        {
            const std::vector<std::string> *pValues = FindValues (m_Attributes, pszHeader);

            if (pValues)
                Recipients += pValues->size();
        }
    }

    if (Recipients > 0)
        AddPart ("nrcpt=" + std::to_string (Recipients));

    return Text;
}


/* The recipient keys are always a JSON array, also with one recipient: one type for a consumer to query, whatever the number */
bool MailLog::IsRecipientKey (const std::string& Key)
{
    return ("email.to.addresses" == Key) || ("email.cc.addresses" == Key) || ("email.bcc.addresses" == Key);
}


/* The values of a key, or NULL if the key was never used */
const std::vector<std::string> *MailLog::FindValues (const AttributeList& List, const std::string& Key)
{
    for (const auto& Entry : List)
    {
        if (Entry.first == Key)
            return &Entry.second;
    }

    return NULL;
}


/* Looks the key up in the list. Found: replaces its values with one value. Not found: a new entry with one value, at the end */
void MailLog::SetValue (AttributeList& retList, const std::string& Key, const std::string& Value)
{
    for (auto& Entry : retList)
    {
        if (Entry.first == Key)
        {
            Entry.second.clear();
            Entry.second.push_back (Value);
            return;
        }
    }

    retList.push_back (std::make_pair (Key, std::vector<std::string> { Value }));
}


/* Looks the key up in the list. Found: adds the value. Not found: a new entry with one value, at the end */
void MailLog::AddValue (AttributeList& retList, const std::string& Key, const std::string& Value)
{
    for (auto& Entry : retList)
    {
        if (Entry.first == Key)
        {
            Entry.second.push_back (Value);
            return;
        }
    }

    retList.push_back (std::make_pair (Key, std::vector<std::string> { Value }));
}


/* Appends Text as a JSON string, with the quotes. ", \ and the control characters (0x00-0x1F) are escaped. Bytes of a UTF-8
   sequence (0x80 and above) are appended as they are: a JSON string is UTF-8, and they need no escaping */
void MailLog::AppendJsonString (std::string& retJson, const std::string& Text)
{
    retJson += '"';

    for (unsigned char c : Text)
    {
        switch (c)
        {
            case '"':  retJson += "\\\""; break;
            case '\\': retJson += "\\\\"; break;
            case '\b': retJson += "\\b";  break;
            case '\f': retJson += "\\f";  break;
            case '\n': retJson += "\\n";  break;
            case '\r': retJson += "\\r";  break;
            case '\t': retJson += "\\t";  break;

            default:

                if (c < 0x20)
                {
                    char szEscape[8] = {0};

                    snprintf (szEscape, sizeof (szEscape), "\\u%04x", c);
                    retJson += szEscape;
                }
                else
                {
                    retJson += static_cast<char> (c);
                }

                break;
        }
    }

    retJson += '"';
}


/* Appends one entry of email.envelope.recipients: the address (required), then the status, the SMTP reply code, its enhanced
   status code and the reply text, each left out if not given, the same rule as everywhere else in this class */
void MailLog::AppendRecipient (std::string& retJson, const Recipient& Entry)
{
    retJson += "{\"address\":";
    AppendJsonString (retJson, Entry.Address);

    if (false == Entry.Status.empty())
    {
        retJson += ",\"status\":";
        AppendJsonString (retJson, Entry.Status);
    }

    if (0 != Entry.Code)
        retJson += ",\"code\":" + std::to_string (Entry.Code);

    if (false == Entry.EnhancedStatus.empty())
    {
        retJson += ",\"enhanced_status_code\":";
        AppendJsonString (retJson, Entry.EnhancedStatus);
    }

    if (false == Entry.Text.empty())
    {
        retJson += ",\"text\":";
        AppendJsonString (retJson, Entry.Text);
    }

    retJson += '}';
}


/* Appends one entry of email.attachments: the name (required), the size, the SHA-256 hash and the content type (all three left
   out if not given, the same rule as everywhere else in this class) */
void MailLog::AppendAttachment (std::string& retJson, const Attachment& Entry)
{
    retJson += "{\"name\":";
    AppendJsonString (retJson, Entry.Name);

    if (0 != Entry.Size)
        retJson += ",\"size_bytes\":" + std::to_string (Entry.Size);

    if (false == Entry.Sha256.empty())
    {
        retJson += ",\"sha256\":";
        AppendJsonString (retJson, Entry.Sha256);
    }

    if (false == Entry.ContentType.empty())
    {
        retJson += ",\"content_type\":";
        AppendJsonString (retJson, Entry.ContentType);
    }

    retJson += '}';
}


/* Renders a resource as {"key":"value",...}: one plain string per key, never an array. SetHostName() and the other resource
   setters only ever call SetValue (never AddValue), so every entry here has exactly one value */
void MailLog::AppendResource (std::string& retJson, const AttributeList& Resource)
{
    retJson += '{';

    for (size_t i = 0; i < Resource.size(); i++)
    {
        if (i > 0)
            retJson += ',';

        AppendJsonString (retJson, Resource[i].first);
        retJson += ':';
        AppendJsonString (retJson, Resource[i].second[0]);
    }

    retJson += '}';
}


/* Lower case, and everything which is not a letter, a digit or an underscore becomes an underscore. "Reply-To" becomes "reply_to" */
std::string MailLog::NormalizeHeaderName (const std::string& Name)
{
    std::string Result;

    Result.reserve (Name.size());

    for (unsigned char c : Name)
    {
        if ( (c >= 'a') && (c <= 'z') )
            Result += static_cast<char> (c);
        else if ( (c >= 'A') && (c <= 'Z') )
            Result += static_cast<char> (c - 'A' + 'a');
        else if ( ( (c >= '0') && (c <= '9') ) || ('_' == c) )
            Result += static_cast<char> (c);
        else
            Result += '_';
    }

    return Result;
}
