
/* Unit test of what happens to a push request before and while it is sent. There is no network in any of it.

   1. The decisions in push_failover.hpp: which endpoint a push request goes to, when the backup is used, and when the primary
      is tried again. The test gives the failover a function which sends nothing and answers what the test says, and it writes
      down which endpoint was called. Time is a parameter, so nothing has to wait.

      In the notes of the calls: P is the primary, p is the primary tried again while the backup is in use (a probe), B is the backup

   2. The converter in otlp_protobuf.hpp: OTLP/HTTP JSON (what otelfwd builds and stores in the WAL) to OTLP/HTTP protobuf (what a
      receiver like VictoriaLogs wants). The test gives the converter a JSON text and compares the bytes it returns with the bytes
      which were worked out by hand from the OTLP protobuf definition.

      The expected bytes are written in hex, with the structure in the spaces. The field numbers are from the OTLP definition:
      ExportLogsServiceRequest 1 resource_logs. ResourceLogs 1 resource, 2 scope_logs. Resource 1 attributes. ScopeLogs 1 scope,
      2 log_records. InstrumentationScope 1 name, 2 version. LogRecord 1 time_unix_nano, 2 severity_number, 3 severity_text,
      5 body, 6 attributes, 11 observed_time_unix_nano. AnyValue 1 string, 2 bool, 3 int, 4 double. KeyValue 1 key, 2 value.
      A tag is (field number << 3) | wire type: 0A is field 1 with a length, 12 is field 2 with a length, 09 is field 1 with 8 fixed
      bytes. A length is a varint: 7 bits at a time, the top bit says that another byte follows

   Build and run:  make push_failover_test && ./push_failover_test        (or: make test)

   Every check prints [PASS] or [FAIL]. The end of the output has a section "Failed checks" (only if there are any) and a
   section "Result". The exit code is 0 if every check passed and 1 if any check failed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <atomic>
#include <string>
#include <thread>
#include <vector>

#include "push_failover.hpp"
#include "otlp_protobuf.hpp"


static int g_Total  = 0;
static int g_Failed = 0;
static std::vector<std::string> g_FailedNames;


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


/* A header: the title between two lines of 80 characters, with an empty line before and after it */
static void Group (const char *pszTitle)
{
    std::string Line (80, '-');

    printf ("\n%s\n%s\n%s\n\n", Line.c_str(), pszTitle, Line.c_str());
}


/* What the two endpoints answer now, and which of them were called */
struct Script
{
    PushAction  Primary = PUSH_ACCEPTED;
    PushAction  Backup  = PUSH_ACCEPTED;
    std::string Calls;

    PushFailover::Sender GetSender()
    {
        return [this] (PushEndpoint Endpoint, bool bProbe)
        {
            if (PUSH_PRIMARY == Endpoint)
            {
                Calls += bProbe ? "p" : "P";
                return Primary;
            }

            Calls += "B";
            return Backup;
        };
    }

    /* Sends one request. Returns the calls of this request only */
    std::string Send (PushFailover& Failover, time_t Now, PushAction *retAction = NULL, int *retSwitchedTo = NULL)
    {
        int Switched = -1;

        Calls.clear();
        PushAction Action = Failover.Send (GetSender(), Now, &Switched);

        if (retAction)
            *retAction = Action;

        if (retSwitchedTo)
            *retSwitchedTo = Switched;

        return Calls;
    }
};


/* Without a backup endpoint the request goes to the primary, and nothing else happens */
static void TestNoBackup ()
{
    PushFailover Failover;
    Script S;
    PushAction Action = PUSH_ACCEPTED;
    int Switched = 0;

    Failover.Configure (false, 60);

    S.Primary = PUSH_ACCEPTED;
    Check ("P" == S.Send (Failover, 1000, &Action, &Switched) && PUSH_ACCEPTED == Action, "no backup: delivered by the primary");

    S.Primary = PUSH_RETRY;
    Check ("P" == S.Send (Failover, 1001, &Action, &Switched) && PUSH_RETRY == Action, "no backup: a failing primary gives retry, nothing else is called");

    S.Primary = PUSH_REJECTED;
    Check ("P" == S.Send (Failover, 1002, &Action, &Switched) && PUSH_REJECTED == Action, "no backup: rejected data is passed on");

    Check (-1 == Switched && PUSH_PRIMARY == Failover.GetActive() && 0 == Failover.GetFailovers(), "no backup: nothing is switched");
    Check (0 == Failover.GetRequests (PUSH_BACKUP, PUSH_ACCEPTED) + Failover.GetRequests (PUSH_BACKUP, PUSH_RETRY) + Failover.GetRequests (PUSH_BACKUP, PUSH_REJECTED),
          "no backup: the backup is never called");
}


/* The primary works: the backup is not used */
static void TestPrimaryWorks ()
{
    PushFailover Failover;
    Script S;

    Failover.Configure (true, 60);

    std::string Calls;

    for (int i = 0; i < 5; i++)
        Calls += S.Send (Failover, 1000 + i);

    Check ("PPPPP" == Calls, "primary works: five requests, only the primary is called");

    Check (PUSH_PRIMARY == Failover.GetActive() && 0 == Failover.GetFailovers(), "primary works: the primary stays in use");
}


/* The primary fails: the same request goes to the backup, and the backup stays in use */
static void TestFailover ()
{
    PushFailover Failover;
    Script S;
    PushAction Action = PUSH_RETRY;
    int Switched = -1;

    Failover.Configure (true, 60);

    S.Primary = PUSH_RETRY;
    Check ("PB" == S.Send (Failover, 1000, &Action, &Switched), "failover: the request goes to the primary, and when that fails to the backup");
    Check (PUSH_ACCEPTED == Action, "failover: it is delivered by the backup");
    Check (PUSH_BACKUP == Switched && PUSH_BACKUP == Failover.GetActive() && 1 == Failover.GetFailovers(), "failover: the backup is in use from now on, the switch is reported once");

    Check ("B" == S.Send (Failover, 1001, &Action, &Switched), "failover: the next request goes to the backup, the failing primary is not tried");
    Check (-1 == Switched && PUSH_ACCEPTED == Action, "failover: no further switch is reported");
    Check ("B" == S.Send (Failover, 1059), "failover: the primary is not tried before the failback time is over");
}


/* Both endpoints fail: retry, so that the caller keeps the request in the WAL */
static void TestBothFail ()
{
    PushFailover Failover;
    Script S;
    PushAction Action = PUSH_ACCEPTED;
    int Switched = 0;

    Failover.Configure (true, 60);

    S.Primary = PUSH_RETRY;
    S.Backup  = PUSH_RETRY;

    Check ("PB" == S.Send (Failover, 1000, &Action, &Switched), "both fail: both are tried");
    Check (PUSH_RETRY == Action, "both fail: the result is retry (the request is kept in the WAL)");
    Check (-1 == Switched && PUSH_PRIMARY == Failover.GetActive() && 0 == Failover.GetFailovers(), "both fail: nothing is switched, the primary stays in use");
}


/* Bad data is the same at both endpoints: the try ends, and nothing is switched */
static void TestRejected ()
{
    PushFailover Failover;
    Script S;
    PushAction Action = PUSH_ACCEPTED;
    int Switched = 0;

    Failover.Configure (true, 60);

    S.Primary = PUSH_REJECTED;
    Check ("P" == S.Send (Failover, 1000, &Action, &Switched) && PUSH_REJECTED == Action, "rejected by the primary: the backup is not tried");

    S.Primary = PUSH_RETRY;
    S.Backup  = PUSH_REJECTED;
    Check ("PB" == S.Send (Failover, 1001, &Action, &Switched) && PUSH_REJECTED == Action, "primary down, rejected by the backup: rejected");
    Check (-1 == Switched && PUSH_PRIMARY == Failover.GetActive(), "rejected by the backup: it does not become the endpoint in use");
}


/* While the backup is in use, the primary is tried again every FailbackSec seconds, and used again when it delivers */
static void TestFailback ()
{
    PushFailover Failover;
    Script S;
    PushAction Action = PUSH_RETRY;
    int Switched = -1;

    Failover.Configure (true, 60);

    S.Primary = PUSH_RETRY;
    S.Send (Failover, 1000);                       /* fails over to the backup at 1000. The next probe is at 1060 */

    Check ("B" == S.Send (Failover, 1059),  "failback: 1 second before the time is over there is no probe");
    Check ("pB" == S.Send (Failover, 1060, &Action), "failback: after 60 seconds the primary is tried (p), it still fails, the backup delivers");
    Check (PUSH_ACCEPTED == Action, "failback: the request is delivered although the probe failed");
    Check ("B" == S.Send (Failover, 1119),  "failback: the next probe is 60 seconds after the last one, not before");
    Check ("pB" == S.Send (Failover, 1120), "failback: and it is made then");
    Check (PUSH_BACKUP == Failover.GetActive() && 0 == Failover.GetFailbacks(), "failback: the backup stays in use while the primary fails");

    S.Primary = PUSH_ACCEPTED;                     /* the primary is back */
    Check ("B" == S.Send (Failover, 1179), "failback: before the probe time it is not noticed, the backup is used");
    Check ("p" == S.Send (Failover, 1180, &Action, &Switched), "failback: at the probe the primary delivers, the backup is not tried");
    Check (PUSH_ACCEPTED == Action && PUSH_PRIMARY == Switched && PUSH_PRIMARY == Failover.GetActive() && 1 == Failover.GetFailbacks(),
          "failback: the primary is in use again, the switch is reported once");
    Check ("P" == S.Send (Failover, 1181), "failback: the next request goes to the primary");
}


/* The interval is the configured one */
static void TestFailbackInterval ()
{
    PushFailover Failover;
    Script S;

    Failover.Configure (true, 5);

    S.Primary = PUSH_RETRY;
    S.Send (Failover, 100);

    Check ("B" == S.Send (Failover, 104) && "pB" == S.Send (Failover, 105) && "B" == S.Send (Failover, 109) && "pB" == S.Send (Failover, 110),
          "interval: with 5 seconds the probes are at 105 and 110");
}


/* The backup fails while it is in use: the primary is tried at once, without waiting for the probe time */
static void TestBackupFails ()
{
    PushFailover Failover;
    Script S;
    PushAction Action = PUSH_RETRY;
    int Switched = -1;

    Failover.Configure (true, 60);

    S.Primary = PUSH_RETRY;
    S.Send (Failover, 1000);                       /* the backup is in use */

    S.Primary = PUSH_ACCEPTED;                     /* the primary is back, but the probe time is not there yet */
    S.Backup  = PUSH_RETRY;
    Check ("BP" == S.Send (Failover, 1010, &Action, &Switched), "backup fails: the primary is tried at once");
    Check (PUSH_ACCEPTED == Action && PUSH_PRIMARY == Switched && PUSH_PRIMARY == Failover.GetActive(), "backup fails: the primary delivers and is in use again");

    /* Both fail while the backup is in use */
    PushFailover Failover2;
    Script S2;

    Failover2.Configure (true, 60);
    S2.Primary = PUSH_RETRY;
    S2.Send (Failover2, 1000);

    S2.Backup = PUSH_RETRY;
    Check ("BP" == S2.Send (Failover2, 1010, &Action, &Switched) && PUSH_RETRY == Action, "backup fails, primary fails: retry, the request is kept in the WAL");
    Check (PUSH_BACKUP == Failover2.GetActive() && -1 == Switched, "backup fails, primary fails: the endpoint in use does not change");
}


/* The primary answers the probe with rejected data: the request ends there, and there is no switch */
static void TestProbeRejected ()
{
    PushFailover Failover;
    Script S;
    PushAction Action = PUSH_ACCEPTED;
    int Switched = 0;

    Failover.Configure (true, 60);

    S.Primary = PUSH_RETRY;
    S.Send (Failover, 1000);

    S.Primary = PUSH_REJECTED;
    Check ("p" == S.Send (Failover, 1060, &Action, &Switched) && PUSH_REJECTED == Action, "probe: rejected data ends the request, the backup is not tried");
    Check (-1 == Switched && PUSH_BACKUP == Failover.GetActive(), "probe: no switch");
}


/* The statistics count every call by endpoint and result */
static void TestStatistics ()
{
    PushFailover Failover;
    Script S;

    Failover.Configure (true, 60);

    S.Primary = PUSH_RETRY;
    S.Send (Failover, 1000);                       /* P retry, B accepted */
    S.Send (Failover, 1001);                       /* B accepted */
    S.Backup = PUSH_REJECTED;
    S.Send (Failover, 1002);                       /* B rejected */
    S.Primary = PUSH_ACCEPTED;
    S.Backup  = PUSH_ACCEPTED;
    S.Send (Failover, 1060);                       /* p accepted: failback */

    Check (1 == Failover.GetRequests (PUSH_PRIMARY, PUSH_RETRY) && 1 == Failover.GetRequests (PUSH_PRIMARY, PUSH_ACCEPTED) &&
           0 == Failover.GetRequests (PUSH_PRIMARY, PUSH_REJECTED), "statistics: the requests to the primary by result");
    Check (2 == Failover.GetRequests (PUSH_BACKUP, PUSH_ACCEPTED) && 1 == Failover.GetRequests (PUSH_BACKUP, PUSH_REJECTED) &&
           0 == Failover.GetRequests (PUSH_BACKUP, PUSH_RETRY), "statistics: the requests to the backup by result");
    Check (1 == Failover.GetFailovers() && 1 == Failover.GetFailbacks(), "statistics: one switch to the backup and one back");
}


/* Two threads must not both make the probe. Many threads send at the same second, when the probe is due: exactly one probes */
static void TestOneProbe ()
{
    PushFailover Failover;
    const int Threads = 8;

    Failover.Configure (true, 60);

    {
        Script S;
        S.Primary = PUSH_RETRY;
        S.Send (Failover, 1000);                   /* the backup is in use, the probe is due at 1060 */
    }

    std::atomic<int>  Probes {0};
    std::atomic<int>  Delivered {0};
    std::atomic<bool> bGo {false};
    std::vector<std::thread> Workers;

    for (int t = 0; t < Threads; t++)
    {
        Workers.emplace_back ([&]
        {
            while (false == bGo.load())
                ;

            PushAction Action = Failover.Send ([&] (PushEndpoint Endpoint, bool bProbe)
            {
                if (bProbe)
                    Probes++;

                return (PUSH_BACKUP == Endpoint) ? PUSH_ACCEPTED : PUSH_RETRY;
            }, 1060);

            if (PUSH_ACCEPTED == Action)
                Delivered++;
        });
    }

    bGo = true;

    for (std::thread& Worker : Workers)
        Worker.join();

    Check (1 == Probes.load(), "threads: at the time of the probe, exactly one of 8 threads probes the primary");
    Check (Threads == Delivered.load(), "threads: every request is delivered by the backup");
}


/* Many threads, mixed answers: the statistics add up and nothing crashes */
static void TestManyThreads ()
{
    PushFailover Failover;
    const int Threads = 4;
    const int PerThread = 20000;

    Failover.Configure (true, 1);

    std::atomic<std::int64_t> Calls {0};
    std::vector<std::thread> Workers;

    for (int t = 0; t < Threads; t++)
    {
        Workers.emplace_back ([&, t]
        {
            unsigned Seed = 12345u + static_cast<unsigned> (t);

            for (int i = 0; i < PerThread; i++)
            {
                Failover.Send ([&] (PushEndpoint, bool)
                {
                    Calls++;
                    return static_cast<PushAction> (rand_r (&Seed) % 3);
                }, static_cast<time_t> (i / 100));
            }
        });
    }

    for (std::thread& Worker : Workers)
        Worker.join();

    std::int64_t Sum = 0;

    for (int e = 0; e < 2; e++)
    {
        for (int a = 0; a < 3; a++)
            Sum += Failover.GetRequests (static_cast<PushEndpoint> (e), static_cast<PushAction> (a));
    }

    Check (Sum == Calls.load(), "many threads: the statistics count every call which was made");
}


/* "0A 24 6B" to the three bytes. White space between the bytes is for the reader of the test */
static std::string FromHex (const char *pszHex)
{
    std::string Bytes;
    int         Nibbles = 0;
    int         Value   = 0;

    for (const char *p = pszHex; *p; p++)
    {
        int Digit;

        if ( (*p >= '0') && (*p <= '9') )
            Digit = *p - '0';
        else if ( (*p >= 'A') && (*p <= 'F') )
            Digit = *p - 'A' + 10;
        else
            continue;

        Value = (Value << 4) | Digit;

        if (2 == ++Nibbles)
        {
            Bytes.push_back (static_cast<char> (Value));
            Nibbles = 0;
            Value   = 0;
        }
    }

    return Bytes;
}


static std::string ToHex (const std::string& Bytes)
{
    std::string Hex;
    char        szByte[4];

    for (size_t i = 0; i < Bytes.size(); i++)
    {
        snprintf (szByte, sizeof (szByte), "%02X", static_cast<unsigned char> (Bytes[i]));

        if (i)
            Hex += ' ';

        Hex += szByte;
    }

    return Hex;
}


/* The JSON must convert to exactly these bytes. The output starts with text, so a converter which appends instead of replacing
   would fail every check */
static void Expect (const char *pszName, const std::string& Json, const std::string& Expected)
{
    std::string Out = "junk";
    std::string Error;

    bool bOk = ConvertOtlpJsonToProtobuf (Json.data(), Json.size(), Out, Error);

    Check (bOk && (Out == Expected), pszName);

    if (bOk && (Out == Expected))
        return;

    printf ("        expected: %s\n", ToHex (Expected).c_str());
    printf ("        got:      %s\n", bOk ? ToHex (Out).c_str() : "(the conversion failed)");

    if (false == bOk)
        printf ("        error:    %s\n", Error.c_str());
}


/* The JSON must be refused. The error text must name the problem (pszWord, if not NULL), because it goes to the log, and
   no bytes may be returned: half of a request must never be sent */
static void ExpectFailure (const char *pszName, const std::string& Json, const char *pszWord)
{
    std::string Out = "junk";
    std::string Error;

    bool bOk = ConvertOtlpJsonToProtobuf (Json.data(), Json.size(), Out, Error);

    bool bWordFound = (NULL == pszWord) || (std::string::npos != Error.find (pszWord));

    Check ( (false == bOk) && Out.empty() && (false == Error.empty()) && bWordFound, pszName);

    if ( (false == bOk) && Out.empty() && (false == Error.empty()) && bWordFound)
        return;

    printf ("        conversion %s, %zu bytes returned, error: \"%s\"", bOk ? "worked" : "failed", Out.size(), Error.c_str());

    if (pszWord)
        printf (", the error should contain \"%s\"", pszWord);

    printf ("\n");
}


/* A log record with the same content, in the way the JSON of otelfwd has it */
static void TestOneRecord ()
{
    /* Request 38 bytes: ResourceLogs 36 = resource 12 (attribute k=v) + scope_logs 24 (scope 5, one record of 15 bytes) */
    Expect ("a record with resource, scope, time and body",
        R"({"resourceLogs":[{"resource":{"attributes":[{"key":"k","value":{"stringValue":"v"}}]},)"
        R"("scopeLogs":[{"scope":{"name":"s"},"logRecords":[{"timeUnixNano":"1","body":{"stringValue":"hi"}}]}]}]})",
        FromHex ("0A 24"
                 "  0A 0A  0A 08  0A 01 6B  12 03 0A 01 76"
                 "  12 16  0A 03 0A 01 73"
                 "         12 0F  09 01 00 00 00 00 00 00 00  2A 04 0A 02 68 69"));

    /* The same content with the members of every object in a different order: the bytes are the same */
    Expect ("the order of the members in the JSON does not change the bytes",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"body":{"stringValue":"hi"},"timeUnixNano":"1"}],"scope":{"name":"s"}}],)"
        R"("resource":{"attributes":[{"value":{"stringValue":"v"},"key":"k"}]}}]})",
        FromHex ("0A 24"
                 "  0A 0A  0A 08  0A 01 6B  12 03 0A 01 76"
                 "  12 16  0A 03 0A 01 73"
                 "         12 0F  09 01 00 00 00 00 00 00 00  2A 04 0A 02 68 69"));

    /* Every field of the log record and every type of value. Times are 8 bytes, least significant byte first:
       0x0102030405060708 is 72623859790382856. Severity 17 is 0x11. 1.5 as a double is 0x3FF8000000000000.
       The int -1 is 10 bytes: FF nine times and 01. Request 89 bytes: ResourceLogs 87 = scope 8 + log record 77 (75 bytes) */
    Expect ("all fields of a record: version, severity, observed time, and the four types of values",
        R"({"resourceLogs":[{"scopeLogs":[{"scope":{"name":"s","version":"1"},"logRecords":[{)"
        R"("timeUnixNano":"72623859790382856","observedTimeUnixNano":"72623859790382857",)"
        R"("severityNumber":17,"severityText":"ERROR","body":{"stringValue":"x"},"attributes":[)"
        R"({"key":"i","value":{"intValue":"-1"}},{"key":"d","value":{"doubleValue":1.5}},{"key":"b","value":{"boolValue":true}}]}]}]}]})",
        FromHex ("0A 57"
                 "  12 55  0A 06  0A 01 73  12 01 31"
                 "         12 4B  09 08 07 06 05 04 03 02 01"
                 "                10 11"
                 "                1A 05 45 52 52 4F 52"
                 "                2A 03 0A 01 78"
                 "                32 10 0A 01 69  12 0B 18 FF FF FF FF FF FF FF FF FF 01"
                 "                32 0E 0A 01 64  12 09 21 00 00 00 00 00 00 F8 3F"
                 "                32 07 0A 01 62  12 02 10 01"
                 "                59 09 07 06 05 04 03 02 01"));

    Expect ("a bool which is false is written, it is a set value and not a default",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"attributes":[{"key":"b","value":{"boolValue":false}}]}]}]}]})",
        FromHex ("0A 0D  12 0B  12 09  32 07 0A 01 62  12 02 10 00"));
}


static void TestNumbers ()
{
    /* An int64 is a string in OTLP/JSON, but a JSON number is accepted too. Largest is 9 bytes, smallest is 10 bytes */
    Expect ("int values: a number, the largest int64 and the smallest int64",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"attributes":[)"
        R"({"key":"a","value":{"intValue":5}},)"
        R"({"key":"b","value":{"intValue":"9223372036854775807"}},)"
        R"({"key":"c","value":{"intValue":"-9223372036854775808"}}]}]}]}]})",
        FromHex ("0A 30  12 2E  12 2C"
                 "  32 07 0A 01 61  12 02 18 05"
                 "  32 0F 0A 01 62  12 0A 18 FF FF FF FF FF FF FF FF 7F"
                 "  32 10 0A 01 63  12 0B 18 80 80 80 80 80 80 80 80 80 01"));

    Expect ("a time as a JSON number",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"timeUnixNano":2}]}]}]})",
        FromHex ("0A 0D  12 0B  12 09  09 02 00 00 00 00 00 00 00"));

    Expect ("a time above the largest int64 is still a valid time (unsigned)",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"timeUnixNano":"18446744073709551615"}]}]}]})",
        FromHex ("0A 0D  12 0B  12 09  09 FF FF FF FF FF FF FF FF"));
}


static void TestStrings ()
{
    /* \u00e9 is one character, and two bytes in UTF-8: C3 A9 */
    Expect ("a string with a non-ASCII character is written as UTF-8",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"body":{"stringValue":"\u00e9"}}]}]}]})",
        FromHex ("0A 0A  12 08  12 06  2A 04 0A 02 C3 A9"));

    Expect ("an empty string is written, it is a set value",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"body":{"stringValue":""}}]}]}]})",
        FromHex ("0A 08  12 06  12 04  2A 02 0A 00"));

    Expect ("a string with a zero byte inside keeps the zero byte and the length",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"body":{"stringValue":"a\u0000b"}}]}]}]})",
        FromHex ("0A 0B  12 09  12 07  2A 05 0A 03 61 00 62"));

    /* 200 characters: the length of the string is 2 bytes (C8 01), and so are the lengths of everything around it.
       The body is 203 bytes (CB 01), the log record 206 (CE 01), the scope_logs 209 (D1 01), the resource_logs 212 (D4 01) */
    Expect ("lengths above 127 are written as two byte varints on every level",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"body":{"stringValue":")" + std::string (200, 'a') + R"("}}]}]}]})",
        FromHex ("0A D4 01  12 D1 01  12 CE 01  2A CB 01  0A C8 01") + std::string (200, 'a'));
}


static void TestStructure ()
{
    Expect ("records of one scope stay in the order they have",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"body":{"stringValue":"a"}},{"body":{"stringValue":"b"}}]}]}]})",
        FromHex ("0A 10  12 0E  12 05 2A 03 0A 01 61  12 05 2A 03 0A 01 62"));

    Expect ("resourceLogs stay in the order they have",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"body":{"stringValue":"a"}}]}]},)"
        R"({"scopeLogs":[{"logRecords":[{"body":{"stringValue":"b"}}]}]}]})",
        FromHex ("0A 09  12 07  12 05 2A 03 0A 01 61"
                 "  0A 09  12 07  12 05 2A 03 0A 01 62"));

    Expect ("no resourceLogs is an empty request",       R"({"resourceLogs":[]})", "");
    Expect ("an empty object is an empty request",        R"({})", "");
    Expect ("an empty resourceLogs is written",           R"({"resourceLogs":[{}]})", FromHex ("0A 00"));
    Expect ("an empty resource is written, and no empty attributes", R"({"resourceLogs":[{"resource":{"attributes":[]},"scopeLogs":[]}]})", FromHex ("0A 02  0A 00"));
}


/* A WAL record is a block of bytes with a length, not a text with a zero at the end. What is behind the length must not be read */
static void TestLength ()
{
    std::string Json    = R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"timeUnixNano":2}]}]}]})";
    std::string Padded  = Json + R"( garbage which is not JSON)";
    std::string Out;
    std::string Error;

    bool bOk = ConvertOtlpJsonToProtobuf (Padded.data(), Json.size(), Out, Error);

    Check (bOk && (Out == FromHex ("0A 0D  12 0B  12 09  09 02 00 00 00 00 00 00 00")), "only the given length is read, the bytes behind it are not");
}


/* What the converter must refuse. It is strict on purpose: it converts the JSON which otelfwd builds, and a field or value
   which it does not know would be lost in the conversion without a trace, so it is an error which is logged and counted */
static void TestRefused ()
{
    ExpectFailure ("empty input",                          "",                                             NULL);
    ExpectFailure ("text which is not JSON",                "otelfwd is not JSON",                          NULL);
    ExpectFailure ("JSON which stops in the middle",        R"({"resourceLogs":[{"scopeLogs":[)",           NULL);
    ExpectFailure ("more than one JSON value",              R"({"resourceLogs":[]} {})",                    NULL);
    ExpectFailure ("an array instead of the request",       R"([])",                                        NULL);
    ExpectFailure ("resourceLogs which is not an array",    R"({"resourceLogs":{}})",                       "resourceLogs");
    ExpectFailure ("a field of the request which is not known",
        R"({"resourceLogs":[],"partialSuccess":{}})", "partialSuccess");
    ExpectFailure ("a field of the log record which is not known (a trace id would be lost)",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"traceId":"0102"}]}]}]})", "traceId");
    ExpectFailure ("a field of the resource which is not known",
        R"({"resourceLogs":[{"resource":{"droppedAttributesCount":1}}]})", "droppedAttributesCount");
    ExpectFailure ("a type of value which is not known (bytes)",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"attributes":[{"key":"k","value":{"bytesValue":"AAE="}}]}]}]}]})", "bytesValue");
    ExpectFailure ("a type of value which is not known (list)",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"attributes":[{"key":"k","value":{"arrayValue":{}}}]}]}]}]})", "arrayValue");
    ExpectFailure ("a value with two types (which one is it?)",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"attributes":[{"key":"k","value":{"stringValue":"1","intValue":"1"}}]}]}]}]})", "more than one");
    ExpectFailure ("an attribute without a key",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"attributes":[{"value":{"stringValue":"v"}}]}]}]}]})", "key");
    ExpectFailure ("an int which is not a number",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"attributes":[{"key":"k","value":{"intValue":"abc"}}]}]}]}]})", "intValue");
    ExpectFailure ("an int which is too large for 64 bits",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"attributes":[{"key":"k","value":{"intValue":"9223372036854775808"}}]}]}]}]})", "intValue");
    ExpectFailure ("a double which is a string",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"attributes":[{"key":"k","value":{"doubleValue":"1.5"}}]}]}]}]})", "doubleValue");
    ExpectFailure ("a bool which is a string",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"attributes":[{"key":"k","value":{"boolValue":"true"}}]}]}]}]})", "boolValue");
    ExpectFailure ("a time which is not a number",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"timeUnixNano":"yesterday"}]}]}]})", "timeUnixNano");
    ExpectFailure ("a time which is negative",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"timeUnixNano":"-1"}]}]}]})", "timeUnixNano");
    ExpectFailure ("a severity which is not a number",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"severityNumber":"ERROR"}]}]}]})", "severityNumber");
    ExpectFailure ("a body which is a string and not a value",
        R"({"resourceLogs":[{"scopeLogs":[{"logRecords":[{"body":"text"}]}]}]})", "body");
}


int main ()
{
    setvbuf (stdout, NULL, _IOLBF, 0);

    Group ("Push test: failover and converter");
    printf ("Which endpoint gets a push request. P: primary, p: primary tried again (probe), B: backup\n");

    Group ("Without a backup, and with a working primary");
    TestNoBackup();
    TestPrimaryWorks();

    Group ("Failing over to the backup");
    TestFailover();
    TestBothFail();
    TestRejected();

    Group ("Going back to the primary");
    TestFailback();
    TestFailbackInterval();
    TestBackupFails();
    TestProbeRejected();

    Group ("Statistics and threads");
    TestStatistics();
    TestOneProbe();
    TestManyThreads();

    Group ("Converter, JSON to protobuf: a log record");
    printf ("The bytes are worked out by hand from the OTLP definition. See the top of this file\n");
    TestOneRecord();

    Group ("Converter: numbers");
    TestNumbers();

    Group ("Converter: strings and lengths");
    TestStrings();

    Group ("Converter: structure, empty, and the length of the input");
    TestStructure();
    TestLength();

    Group ("Converter: input which is refused");
    TestRefused();

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
