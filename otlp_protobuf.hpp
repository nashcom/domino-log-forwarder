
/* Converter from OTLP/HTTP JSON to OTLP/HTTP protobuf, for the receivers which do not take JSON. VictoriaLogs answers a JSON
   request with HTTP 400 "json encoding isn't supported for opentelemetry format. Use protobuf encoding".

   otelfwd keeps JSON as its internal format: BuildOtlpPayload makes it and the WAL stores it. The conversion is the last step
   before a request is sent, so what is in the WAL never depends on the setting of the encoding.

   The protobuf is written by hand, without a library. The messages of the OTLP definition which are needed:

     ExportLogsServiceRequest  1 resource_logs
     ResourceLogs              1 resource, 2 scope_logs
     Resource                  1 attributes
     ScopeLogs                 1 scope, 2 log_records
     InstrumentationScope      1 name, 2 version
     LogRecord                 1 time_unix_nano, 2 severity_number, 3 severity_text, 5 body, 6 attributes, 11 observed_time_unix_nano
     AnyValue                  1 string_value, 2 bool_value, 3 int_value, 4 double_value
     KeyValue                  1 key, 2 value

   Rules:

   * Every member which is in the JSON is written, also if it is empty or zero. Members which are not in the JSON are not
     written. Fields are written in the order of their numbers, whatever the order in the JSON is. Records and groups keep their order.
   * The converter is strict on purpose. It converts the JSON which BuildOtlpPayload makes. A member or a type of value it does not
     know (a trace id, a list, bytes) would be lost in the conversion without a trace. It is an error which the caller logs and counts.
     If BuildOtlpPayload learns something new, this converter has to learn it as well.
   * A 64 bit integer is a decimal string in OTLP/JSON. A JSON number is accepted too. Times are unsigned, int values are signed.
   * On an error no bytes are returned, the error text names the member which is wrong, and the values are not repeated in it.
   * Only the given length of the input is read. It does not need a zero at the end (a WAL record has none) */

#pragma once

#include <errno.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include <initializer_list>
#include <string>

#include <rapidjson/document.h>
#include <rapidjson/error/en.h>


class OtlpJsonToProtobuf
{
public:

    bool Convert (const char *pszJson, size_t JsonLen, std::string& Out, std::string& Error)
    {
        Out.clear();
        Error.clear();
        m_Error.clear();

        std::string Request;

        if (false == ConvertRequest (pszJson, JsonLen, Request))
        {
            Error = m_Error.empty() ? "conversion failed" : m_Error;
            return false;
        }

        Out.swap (Request);
        return true;
    }

private:

    typedef rapidjson::Value JsonValue;

    std::string m_Error;


    bool Fail (const std::string& Text)
    {
        m_Error = Text;
        return false;
    }


    /* ---- protobuf writer: varint, tag, and the three kinds of fields which are needed ---- */

    static void AddVarint (std::string& Out, uint64_t Value)
    {
        while (Value >= 0x80)
        {
            Out.push_back (static_cast<char> ((Value & 0x7F) | 0x80));
            Value >>= 7;
        }

        Out.push_back (static_cast<char> (Value));
    }


    /* Wire types: 0 varint, 1 fixed 64 bit, 2 with a length */
    static void AddTag (std::string& Out, unsigned Field, unsigned WireType)
    {
        AddVarint (Out, (static_cast<uint64_t> (Field) << 3) | WireType);
    }


    static void AddVarintField (std::string& Out, unsigned Field, uint64_t Value)
    {
        AddTag (Out, Field, 0);
        AddVarint (Out, Value);
    }


    /* The bytes of a number, least significant byte first */
    static void AddFixed64Field (std::string& Out, unsigned Field, uint64_t Value)
    {
        AddTag (Out, Field, 1);

        for (int i = 0; i < 8; i++)
            Out.push_back (static_cast<char> ((Value >> (8 * i)) & 0xFF));
    }


    /* A string, or a message which was written to a string before */
    static void AddBytesField (std::string& Out, unsigned Field, const char *pszBytes, size_t Len)
    {
        AddTag (Out, Field, 2);
        AddVarint (Out, Len);
        Out.append (pszBytes, Len);
    }


    static void AddBytesField (std::string& Out, unsigned Field, const std::string& Bytes)
    {
        AddBytesField (Out, Field, Bytes.data(), Bytes.size());
    }


    /* ---- reading the JSON ---- */

    static const JsonValue *Find (const JsonValue& Object, const char *pszName)
    {
        JsonValue::ConstMemberIterator It = Object.FindMember (pszName);

        return (It == Object.MemberEnd()) ? NULL : &It->value;
    }


    /* The value must be an object which has only the given members. pszWhat names it in the error */
    bool CheckObject (const JsonValue& Value, const char *pszWhat, std::initializer_list<const char *> Known)
    {
        if (false == Value.IsObject())
            return Fail (std::string ("\"") + pszWhat + "\" is not a JSON object");

        for (JsonValue::ConstMemberIterator It = Value.MemberBegin(); It != Value.MemberEnd(); ++It)
        {
            bool bKnown = false;

            for (const char *pszKnown : Known)
            {
                if (0 == strcmp (It->name.GetString(), pszKnown))
                {
                    bKnown = true;
                    break;
                }
            }

            if (false == bKnown)
                return Fail (std::string ("unknown member \"") + It->name.GetString() + "\" in \"" + pszWhat + "\"");
        }

        return true;
    }


    bool CheckArray (const JsonValue& Value, const char *pszName)
    {
        if (false == Value.IsArray())
            return Fail (std::string ("\"") + pszName + "\" is not a JSON array");

        return true;
    }


    bool CheckString (const JsonValue& Value, const char *pszName)
    {
        if (false == Value.IsString())
            return Fail (std::string ("\"") + pszName + "\" is not a string");

        return true;
    }


    /* Only digits, and a minus if bSigned. A number as text can be longer than what fits, which strtoull and strtoll report */
    static bool IsDecimalText (const JsonValue& Value, bool bSigned)
    {
        const char *psz = Value.GetString();
        size_t      Len = Value.GetStringLength();

        if (strlen (psz) != Len)
            return false;

        if ( bSigned && ('-' == *psz) )
            psz++;

        if ('\0' == *psz)
            return false;

        for (; *psz; psz++)
        {
            if ( (*psz < '0') || (*psz > '9') )
                return false;
        }

        return true;
    }


    bool GetUint64 (const JsonValue& Value, const char *pszName, uint64_t& retValue)
    {
        if (Value.IsUint64())
        {
            retValue = Value.GetUint64();
            return true;
        }

        if ( Value.IsString() && IsDecimalText (Value, false) )
        {
            errno = 0;
            char *pszEnd = NULL;
            unsigned long long Number = strtoull (Value.GetString(), &pszEnd, 10);

            if ( (0 == errno) && (0 == *pszEnd) )
            {
                retValue = static_cast<uint64_t> (Number);
                return true;
            }
        }

        return Fail (std::string ("\"") + pszName + "\" is not an unsigned 64 bit integer");
    }


    bool GetInt64 (const JsonValue& Value, const char *pszName, int64_t& retValue)
    {
        if (Value.IsInt64())
        {
            retValue = Value.GetInt64();
            return true;
        }

        if ( Value.IsString() && IsDecimalText (Value, true) )
        {
            errno = 0;
            char *pszEnd = NULL;
            long long Number = strtoll (Value.GetString(), &pszEnd, 10);

            if ( (0 == errno) && (0 == *pszEnd) )
            {
                retValue = static_cast<int64_t> (Number);
                return true;
            }
        }

        return Fail (std::string ("\"") + pszName + "\" is not a signed 64 bit integer");
    }


    /* ---- one function for each message. They write to a string, the caller adds it with its tag and length ---- */

    bool ConvertAnyValue (const JsonValue& Value, const char *pszWhat, std::string& Out)
    {
        if (false == CheckObject (Value, pszWhat, { "stringValue", "intValue", "doubleValue", "boolValue" }))
            return false;

        if (Value.MemberCount() > 1)
            return Fail (std::string ("\"") + pszWhat + "\" has more than one value");

        const JsonValue *p;

        if ( (p = Find (Value, "stringValue")) )
        {
            if (false == CheckString (*p, "stringValue"))
                return false;

            AddBytesField (Out, 1, p->GetString(), p->GetStringLength());
        }

        if ( (p = Find (Value, "boolValue")) )
        {
            if (false == p->IsBool())
                return Fail ("\"boolValue\" is not true or false");

            AddVarintField (Out, 2, p->GetBool() ? 1 : 0);
        }

        if ( (p = Find (Value, "intValue")) )
        {
            int64_t Number;

            if (false == GetInt64 (*p, "intValue", Number))
                return false;

            AddVarintField (Out, 3, static_cast<uint64_t> (Number));
        }

        if ( (p = Find (Value, "doubleValue")) )
        {
            if (false == p->IsNumber())
                return Fail ("\"doubleValue\" is not a number");

            double Number = p->GetDouble();
            uint64_t Bits;

            memcpy (&Bits, &Number, sizeof (Bits));
            AddFixed64Field (Out, 4, Bits);
        }

        return true;
    }


    bool ConvertKeyValue (const JsonValue& Value, std::string& Out)
    {
        if (false == CheckObject (Value, "attribute", { "key", "value" }))
            return false;

        const JsonValue *p = Find (Value, "key");

        if (NULL == p)
            return Fail ("attribute without \"key\"");

        if (false == CheckString (*p, "key"))
            return false;

        AddBytesField (Out, 1, p->GetString(), p->GetStringLength());

        if ( (p = Find (Value, "value")) )
        {
            std::string Any;

            if (false == ConvertAnyValue (*p, "value", Any))
                return false;

            AddBytesField (Out, 2, Any);
        }

        return true;
    }


    bool ConvertAttributes (const JsonValue& Value, unsigned Field, std::string& Out)
    {
        if (false == CheckArray (Value, "attributes"))
            return false;

        for (JsonValue::ConstValueIterator It = Value.Begin(); It != Value.End(); ++It)
        {
            std::string KeyValue;

            if (false == ConvertKeyValue (*It, KeyValue))
                return false;

            AddBytesField (Out, Field, KeyValue);
        }

        return true;
    }


    bool ConvertLogRecord (const JsonValue& Value, std::string& Out)
    {
        if (false == CheckObject (Value, "logRecord", { "timeUnixNano", "observedTimeUnixNano", "severityNumber", "severityText", "body", "attributes" }))
            return false;

        const JsonValue *p;
        uint64_t         Time = 0;

        if ( (p = Find (Value, "timeUnixNano")) )
        {
            if (false == GetUint64 (*p, "timeUnixNano", Time))
                return false;

            AddFixed64Field (Out, 1, Time);
        }

        if ( (p = Find (Value, "severityNumber")) )
        {
            if (false == p->IsInt())
                return Fail ("\"severityNumber\" is not an integer");

            AddVarintField (Out, 2, static_cast<uint64_t> (static_cast<int64_t> (p->GetInt())));
        }

        if ( (p = Find (Value, "severityText")) )
        {
            if (false == CheckString (*p, "severityText"))
                return false;

            AddBytesField (Out, 3, p->GetString(), p->GetStringLength());
        }

        if ( (p = Find (Value, "body")) )
        {
            std::string Any;

            if (false == ConvertAnyValue (*p, "body", Any))
                return false;

            AddBytesField (Out, 5, Any);
        }

        if ( (p = Find (Value, "attributes")) )
        {
            if (false == ConvertAttributes (*p, 6, Out))
                return false;
        }

        if ( (p = Find (Value, "observedTimeUnixNano")) )
        {
            if (false == GetUint64 (*p, "observedTimeUnixNano", Time))
                return false;

            AddFixed64Field (Out, 11, Time);
        }

        return true;
    }


    bool ConvertScope (const JsonValue& Value, std::string& Out)
    {
        if (false == CheckObject (Value, "scope", { "name", "version" }))
            return false;

        const JsonValue *p;

        if ( (p = Find (Value, "name")) )
        {
            if (false == CheckString (*p, "name"))
                return false;

            AddBytesField (Out, 1, p->GetString(), p->GetStringLength());
        }

        if ( (p = Find (Value, "version")) )
        {
            if (false == CheckString (*p, "version"))
                return false;

            AddBytesField (Out, 2, p->GetString(), p->GetStringLength());
        }

        return true;
    }


    bool ConvertScopeLogs (const JsonValue& Value, std::string& Out)
    {
        if (false == CheckObject (Value, "scopeLogs", { "scope", "logRecords" }))
            return false;

        const JsonValue *p;

        if ( (p = Find (Value, "scope")) )
        {
            std::string Scope;

            if (false == ConvertScope (*p, Scope))
                return false;

            AddBytesField (Out, 1, Scope);
        }

        if ( (p = Find (Value, "logRecords")) )
        {
            if (false == CheckArray (*p, "logRecords"))
                return false;

            for (JsonValue::ConstValueIterator It = p->Begin(); It != p->End(); ++It)
            {
                std::string LogRecord;

                if (false == ConvertLogRecord (*It, LogRecord))
                    return false;

                AddBytesField (Out, 2, LogRecord);
            }
        }

        return true;
    }


    bool ConvertResource (const JsonValue& Value, std::string& Out)
    {
        if (false == CheckObject (Value, "resource", { "attributes" }))
            return false;

        const JsonValue *p = Find (Value, "attributes");

        if (p)
            return ConvertAttributes (*p, 1, Out);

        return true;
    }


    bool ConvertResourceLogs (const JsonValue& Value, std::string& Out)
    {
        if (false == CheckObject (Value, "resourceLogs", { "resource", "scopeLogs" }))
            return false;

        const JsonValue *p;

        if ( (p = Find (Value, "resource")) )
        {
            std::string Resource;

            if (false == ConvertResource (*p, Resource))
                return false;

            AddBytesField (Out, 1, Resource);
        }

        if ( (p = Find (Value, "scopeLogs")) )
        {
            if (false == CheckArray (*p, "scopeLogs"))
                return false;

            for (JsonValue::ConstValueIterator It = p->Begin(); It != p->End(); ++It)
            {
                std::string ScopeLogs;

                if (false == ConvertScopeLogs (*It, ScopeLogs))
                    return false;

                AddBytesField (Out, 2, ScopeLogs);
            }
        }

        return true;
    }


    bool ConvertRequest (const char *pszJson, size_t JsonLen, std::string& Out)
    {
        if (NULL == pszJson)
            return Fail ("no input");

        rapidjson::Document Doc;

        Doc.Parse (pszJson, JsonLen);

        if (Doc.HasParseError())
            return Fail (std::string ("not valid JSON: ") + rapidjson::GetParseError_En (Doc.GetParseError()) +
                         " (offset " + std::to_string (Doc.GetErrorOffset()) + ")");

        if (false == CheckObject (Doc, "request", { "resourceLogs" }))
            return false;

        const JsonValue *p = Find (Doc, "resourceLogs");

        if (NULL == p)
            return true;

        if (false == CheckArray (*p, "resourceLogs"))
            return false;

        for (JsonValue::ConstValueIterator It = p->Begin(); It != p->End(); ++It)
        {
            std::string ResourceLogs;

            if (false == ConvertResourceLogs (*It, ResourceLogs))
                return false;

            AddBytesField (Out, 1, ResourceLogs);
        }

        return true;
    }
};


/* Converts one OTLP/HTTP JSON request to the protobuf of the same request. Returns false with a text in Error if it can not.
   Out is replaced, and it is empty after an error */
inline bool ConvertOtlpJsonToProtobuf (const char *pszJson, size_t JsonLen, std::string& Out, std::string& Error)
{
    OtlpJsonToProtobuf Converter;

    return Converter.Convert (pszJson, JsonLen, Out, Error);
}
