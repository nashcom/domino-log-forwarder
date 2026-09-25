
/* Unit test of the pieces of otelfwd which need no network and no other program: the failover between two endpoints, the converter
   from JSON to protobuf, and the format of the log lines.

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

   3. The console output of otelfwd (log_line.hpp): the time stamp in UTC, the two blanks, the process name, the level. The times
      are what the date command says for the same number of seconds, also when the host time zone is far from UTC.

   4. The file input (file_input.hpp): the names of the severities, the names of the state files, and how the push thread hands over the
      position up to which lines were delivered to the file thread. The names of two paths are written down in the test: a name which changes between versions
      loses the position of every file. The file itself is read by the FileReader, which has a test of its own (filereader/).

   5. The label of the metrics (prom_label.hpp): the instance label which is added to every metric name, and its removal. The load test
      uses the removal to find a metric by its name with or without the label.

   Build and run:  make otelfwd_unit_test && ./otelfwd_unit_test        (or: make test)

   Every check prints [PASS] or [FAIL]. The end of the output has a section "Failed checks" (only if there are any) and a
   section "Result". The exit code is 0 if every check passed and 1 if any check failed. */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <atomic>
#include <functional>
#include <string>
#include <thread>
#include <vector>

#include "push_failover.hpp"
#include "otlp_protobuf.hpp"
#include "log_line.hpp"
#include "file_input.hpp"
#include "prom_label.hpp"


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


/* Returns what the action writes to stderr. The action must not print a check: it would be captured too */
static std::string CaptureStderr (const std::function<void()>& Action)
{
    char szPath[] = "/tmp/otelfwd_unit_test_XXXXXX";
    std::string Result;
    char   szBuffer[512];
    size_t Read = 0;

    int fdNew = ::mkstemp (szPath);

    if (fdNew < 0)
        return Result;

    fflush (stderr);

    int fdOld = ::dup (2);

    ::dup2 (fdNew, 2);
    ::close (fdNew);

    Action();

    fflush (stderr);
    ::dup2 (fdOld, 2);
    ::close (fdOld);

    FILE *fp = fopen (szPath, "r");

    if (fp)
    {
        while ((Read = fread (szBuffer, 1, sizeof (szBuffer), fp)) > 0)
            Result.append (szBuffer, Read);

        fclose (fp);
    }

    ::unlink (szPath);

    return Result;
}


/* ============================== Log lines ============================== */


/* The start of the text is "dddd-dd-ddTdd:dd:ddZ" and two blanks, with digits in place of the d. A check of the shape, which does
   not use the code which makes it */
static bool StartsWithTimestamp (const std::string& Text)
{
    static const char szShape[] = "dddd-dd-ddTdd:dd:ddZ  ";
    size_t Len = sizeof (szShape) - 1;

    if (Text.size() < Len)
        return false;

    for (size_t i = 0; i < Len; i++)
    {
        if ('d' == szShape[i])
        {
            if ( (Text[i] < '0') || (Text[i] > '9') )
                return false;
        }
        else if (Text[i] != szShape[i])
        {
            return false;
        }
    }

    return true;
}


/* The times are what the date command says for the same number of seconds: date -u -d @NUMBER */
static void TestLogTime ()
{
    /* The time is UTC whatever the time zone of the host is. A zone 12 hours away shows a local time as a difference:
       the POSIX form NZST-12 needs no time zone database on the host */
    const char *pszOldZone = getenv ("TZ");
    std::string OldZone    = pszOldZone ? pszOldZone : "";

    ::setenv ("TZ", "NZST-12", 1);
    ::tzset();

    Check ("1970-01-01T00:00:00Z" == FormatLogTime (0),            "log time: 0 is the start of 1970 in UTC, also when the host time zone is 12 hours ahead");
    Check ("2000-02-29T00:00:00Z" == FormatLogTime (951782400),    "log time: a leap day");
    Check ("2026-09-19T21:07:09Z" == FormatLogTime (1789852029),   "log time: a time of today (the metrics timestamp of the WAL retry test)");
    Check ("2030-01-01T00:00:00Z" == FormatLogTime (1893456000),   "log time: the start of a year");
    Check ("2038-01-19T03:14:08Z" == FormatLogTime (2147483648LL), "log time: the second after the 32 bit limit");

    if (pszOldZone)
        ::setenv ("TZ", OldZone.c_str(), 1);
    else
        ::unsetenv ("TZ");

    ::tzset();
}


static void TestLogLine ()
{
    Check ("1970-01-01T00:00:00Z  otelfwd: [Error] message: text\n" == FormatLogLine (0, "otelfwd", "Error", "message", "text"),
           "log line: time, two blanks, process name, level, message, text");

    Check ("1970-01-01T00:00:00Z  [Error] message: text\n" == FormatLogLine (0, NULL, "Error", "message", "text"),
           "log line: without a process name (-nostdin) there is nothing before the level");

    Check ("1970-01-01T00:00:00Z  [Error] message: text\n" == FormatLogLine (0, "", "Error", "message", "text"),
           "log line: an empty process name is the same as none");

    Check ("1970-01-01T00:00:00Z  otelfwd: message: text\n" == FormatLogLine (0, "otelfwd", NULL, "message", "text"),
           "log line: without a level");

    Check ("1970-01-01T00:00:00Z  otelfwd: [Warning] message\n" == FormatLogLine (0, "otelfwd", "Warning", "message", NULL),
           "log line: without a text after the message");

    Check ("1970-01-01T00:00:00Z  otelfwd: [Warning] message\n" == FormatLogLine (0, "otelfwd", "Warning", "message", ""),
           "log line: an empty text is the same as none, there is no dangling colon");

    Check ("1970-01-01T00:00:00Z  message\n" == FormatLogLine (0, NULL, NULL, "message", NULL),
           "log line: only a message: time, two blanks, message");

    Check ("1970-01-01T00:00:00Z  \n" == FormatLogLine (0, NULL, NULL, NULL, NULL),
           "log line: no message at all does not crash");
}


/* What goes to stderr really: the time is now, and the line is one piece */
static void TestLogOutput ()
{
    time_t Before = time (NULL);

    SetLogPrefix ("otelfwd");

    std::string Out = CaptureStderr ([] { WriteLogLine ("Error", "test message", "test text"); });

    time_t After = time (NULL);

    Check (StartsWithTimestamp (Out), "log output: the line starts with a time stamp and two blanks");
    Check (Out.size() > 22 && "otelfwd: [Error] test message: test text\n" == Out.substr (22), "log output: the process name, the level, the message and the text follow");

    struct tm Tm {};
    bool bParsed = (NULL != ::strptime (Out.c_str(), "%Y-%m-%dT%H:%M:%SZ", &Tm));
    time_t Logged = bParsed ? ::timegm (&Tm) : 0;

    Check (bParsed && (Logged >= Before - 1) && (Logged <= After + 1), "log output: the time is the time of now, in UTC");

    SetLogPrefix (NULL);

    Out = CaptureStderr ([] { WriteLogLine (NULL, "plain message"); });
    Check (StartsWithTimestamp (Out) && Out.size() > 22 && "plain message\n" == Out.substr (22), "log output: without a process name (-nostdin) the message follows the time stamp");

    Out = CaptureStderr ([] { errno = ENOENT; WriteLogErrno ("Cannot open the file"); });
    Check (Out.size() > 22 && "[Error] Cannot open the file: No such file or directory\n" == Out.substr (22), "log output: an error with errno has the text of the error, in the place of perror");
}


/* The names of OTELFWD_FILE_SEVERITY. The numbers are those of the OpenTelemetry log data model */
static void TestFileSeverity ()
{
    struct Case
    {
        const char *pszName;
        int         Number;
        const char *pszText;
    };

    const Case Cases[] =
    {
        { "trace", 1, "TRACE" }, { "debug", 5, "DEBUG" }, { "info", 9, "INFO" }, { "warn", 13, "WARN" }, { "warning", 13, "WARN" },
        { "error", 17, "ERROR" }, { "fatal", 21, "FATAL" }, { "off", 0, "" }, { "none", 0, "" }, { "unspecified", 0, "" },
        { "INFO", 9, "INFO" }, { "Warning", 13, "WARN" }, { "ERROR", 17, "ERROR" }, { "OFF", 0, "" }
    };

    for (const Case& Test : Cases)
    {
        int         Number = -1;
        std::string Text   = "unchanged";
        std::string Name   = std::string ("file severity: ") + Test.pszName + " is number " + std::to_string (Test.Number) + (*Test.pszText ? std::string (" and the text ") + Test.pszText : std::string (" and has no text"));

        Check (FileSeverity::Parse (Test.pszName, Number, Text) && (Test.Number == Number) && (Test.pszText == Text), Name.c_str());
    }

    int         Number = 9;
    std::string Text   = "INFO";

    Check (false == FileSeverity::Parse ("verbose", Number, Text), "file severity: an unknown name is refused");
    Check ( (9 == Number) && ("INFO" == Text), "file severity: and nothing is changed");
    Check (false == FileSeverity::Parse ("", Number, Text), "file severity: an empty text is refused");
    Check (false == FileSeverity::Parse (NULL, Number, Text), "file severity: NULL is refused");
    Check (false == FileSeverity::Parse (" info", Number, Text), "file severity: a blank before the name is refused");
    Check (false == FileSeverity::Parse ("info ", Number, Text), "file severity: a blank behind the name is refused");
    Check (false == FileSeverity::Parse ("9", Number, Text), "file severity: a number is refused, the names are the levels");
}


/* The names of the state files. Two of them are written down: they have to stay the same between versions, or every file is read
   again from the start after an update. The hash is FNV-1a of the path, its low 32 bits as 8 hex digits */
static void TestFileStateName ()
{
    std::string Suffix = ".otelfwd-state";

    Check ("/data/app.log.abd36f6a.otelfwd-state" == FileStateName::Make ("/data", "/var/log/app.log"), "state file name: <directory>/<name>.<hash of the path>.otelfwd-state");
    Check ("/data/app.log.49cd793a.otelfwd-state" == FileStateName::Make ("/data", "/srv/other/app.log"), "state file name: the same name in another directory has another hash");
    Check (FileStateName::Make ("/data", "/var/log/app.log") == FileStateName::Make ("/data", "/var/log/app.log"), "state file name: the same path gives the same name every time");
    Check (FileStateName::Make ("/data", "/var/log/app.log") != FileStateName::Make ("/data", "/var/log/app.log2"), "state file name: a path which differs in one character has another name");
    Check ("/data/app.log.abd36f6a.otelfwd-state" == FileStateName::Make ("/data/", "/var/log/app.log"), "state file name: a slash at the end of the directory is not doubled");
    Check ("/var/log/app.log.otelfwd-state" == FileStateName::Make ("", "/var/log/app.log"), "state file name: without a directory it is next to the file: <path>.otelfwd-state");
    Check ("/var/log/my app (1).log.otelfwd-state" == FileStateName::Make ("", "/var/log/my app (1).log"), "state file name: next to the file the name of the file is kept as it is, and no hash is needed");
    Check (FileStateName::Make ("", "/var/log/a.log") != FileStateName::Make ("", "/var/log/b.log"), "state file name: two files in one directory have two state files");

    /* What is not safe in a file name is replaced. The hash is of the real path, so two names which look the same after that stay apart */
    std::string Odd    = FileStateName::Make ("/d", "/var/log/my app (1).log");
    std::string Prefix = "/d/my_app__1_.log.";

    Check ( (Odd.compare (0, Prefix.size(), Prefix) == 0) && (Odd.size() == Prefix.size() + 8 + Suffix.size()), "state file name: blanks and brackets in the file name become underscores");
    Check (FileStateName::Make ("/d", "/x/a b.log") != FileStateName::Make ("/d", "/x/a_b.log"), "state file name: two names which look the same after that are still two names");

    std::string Utf8   = FileStateName::Make ("/d", "/var/log/caf\xc3\xa9.log");
    bool        bAscii = true;

    for (char c : Utf8)
    {
        if (static_cast<unsigned char> (c) >= 0x80)
            bAscii = false;
    }

    Check (bAscii, "state file name: bytes above 127 are replaced");

    std::string Long   = FileStateName::Make ("/d", "/var/log/" + std::string (300, 'x') + ".log");

    Check ( (Long.size() == 3 + 100 + 1 + 8 + Suffix.size()) && (Long.compare (3, 100, std::string (100, 'x')) == 0), "state file name: a long file name is cut at 100 characters");

    std::string Empty  = FileStateName::Make ("/d", "/var/log/");

    Check (Empty.compare (0, 8, "/d/file.") == 0, "state file name: a path without a file name gives \"file\"");
    Check (FileStateName::Make ("/d", "app.log").compare (0, 11, "/d/app.log.") == 0, "state file name: a path without a directory works");
}


/* What the push thread hands over to the file thread */
static void TestFileCommitSlot ()
{
    FileCommitSlot Slot;
    uint64_t       Generation = 0;
    uint64_t       EndOffset  = 0;

    Check (false == Slot.Take (Generation, EndOffset), "commit slot: empty at the start");

    Slot.Add (1, 100);
    Check (Slot.Take (Generation, EndOffset) && (1 == Generation) && (100 == EndOffset), "commit slot: what was added is taken");
    Check (false == Slot.Take (Generation, EndOffset), "commit slot: and it is gone after that");

    Slot.Add (1, 100);
    Slot.Add (1, 250);
    Slot.Add (1, 180);
    Check (Slot.Take (Generation, EndOffset) && (1 == Generation) && (250 == EndOffset), "commit slot: the highest position of a generation is kept");

    Slot.Add (1, 500);
    Slot.Add (2, 10);
    Check (Slot.Take (Generation, EndOffset) && (2 == Generation) && (10 == EndOffset), "commit slot: a newer generation replaces an older one, also with a lower position");

    Slot.Add (2, 20);
    Slot.Add (1, 900);
    Check (Slot.Take (Generation, EndOffset) && (2 == Generation) && (20 == EndOffset), "commit slot: an older generation than the current one is ignored");

    Slot.Add (2, 20);
    Slot.Add (2, 15);
    Check (false == Slot.Take (Generation, EndOffset), "commit slot: a position which was taken already, or a lower one, is ignored after the take too");

    Slot.Add (2, 30);
    Check (Slot.Take (Generation, EndOffset) && (2 == Generation) && (30 == EndOffset), "commit slot: a higher position after a take is taken");

    /* Several threads add, and the file thread takes: the highest position wins, and nothing is torn */
    FileCommitSlot Shared;
    std::atomic<bool> bStop {false};
    uint64_t          Highest = 0;
    bool              bAscending = true;

    std::thread Taker ([&]
    {
        uint64_t G = 0, O = 0, Last = 0;

        while (false == bStop.load())
        {
            if (Shared.Take (G, O))
            {
                if ( (1 != G) || (O < Last) )
                    bAscending = false;

                Last = O;
            }
        }

        if (Shared.Take (G, O))
        {
            if ( (1 != G) || (O < Last) )
                bAscending = false;

            Last = O;
        }

        Highest = Last;
    });

    std::vector<std::thread> Adders;

    for (uint64_t t = 0; t < 4; t++)
    {
        Adders.emplace_back ([&Shared, t]
        {
            for (uint64_t i = 1; i <= 10000; i++)
                Shared.Add (1, i * 4 + t);
        });
    }

    for (std::thread& Adder : Adders)
        Adder.join();

    bStop = true;
    Taker.join();

    Check (bAscending && (40003 == Highest), "commit slot: four threads adding while one takes: what is taken never goes back, and the highest position arrives");
}


/* The label which tells two instances apart: added to the name of every metric, in front of the labels which are there.
   The Prometheus text format: the value is in double quotes, and a backslash, a double quote and a new line are escaped */
static void TestPromLabel ()
{
    Check ("otelfwd_health{otelfwd_instance=\"mail\"}" == PromAddLabel ("otelfwd_health", "otelfwd_instance", "mail"),
           "prom label: a metric without labels gets the label in braces");

    Check ("otelfwd_push_total{otelfwd_instance=\"mail\",result=\"success\"}" == PromAddLabel ("otelfwd_push_total{result=\"success\"}", "otelfwd_instance", "mail"),
           "prom label: a metric with a label gets the new one in front of it, separated by a comma");

    Check ("otelfwd_x{otelfwd_instance=\"mail\",a=\"1\",b=\"2\"}" == PromAddLabel ("otelfwd_x{a=\"1\",b=\"2\"}", "otelfwd_instance", "mail"),
           "prom label: more than one label which is there is kept, in the same order");

    Check ("otelfwd_x{otelfwd_instance=\"mail\"}" == PromAddLabel ("otelfwd_x{}", "otelfwd_instance", "mail"),
           "prom label: empty braces are filled, not doubled");

    Check ("otelfwd_x{otelfwd_instance=\"mail\"}" == PromAddLabel ("otelfwd_x{", "otelfwd_instance", "mail"),
           "prom label: a name with an open brace and nothing else is closed");

    Check ("otelfwd_health" == PromAddLabel ("otelfwd_health", "otelfwd_instance", ""),
           "prom label: without a value the name is not changed");

    Check ("otelfwd_health" == PromAddLabel ("otelfwd_health", "", "mail"),
           "prom label: without a label name the name is not changed");

    Check ("otelfwd_health{otelfwd_instance=\"CN=earth/O=NotesLab\"}" == PromAddLabel ("otelfwd_health", "otelfwd_instance", "CN=earth/O=NotesLab"),
           "prom label: the distinguished name of a Domino server is a value like any other, = and / are not escaped");

    Check ("a\\\"b\\\\c\\nd" == PromEscapeLabelValue ("a\"b\\c\nd"),
           "prom label: a double quote, a backslash and a new line in a value are escaped");

    Check ("otelfwd_health{otelfwd_instance=\"a\\\"b\"}" == PromAddLabel ("otelfwd_health", "otelfwd_instance", "a\"b"),
           "prom label: the value in the name is escaped, so a quote in it does not end the value");

    /* Removing it again, for a reader which finds a metric by its name and its labels: the load test */
    Check ("otelfwd_health" == PromRemoveLabel ("otelfwd_health{otelfwd_instance=\"mail\"}", "otelfwd_instance"),
           "prom label removed: the only label goes, and the braces with it");

    Check ("otelfwd_push_total{result=\"success\"}" == PromRemoveLabel ("otelfwd_push_total{otelfwd_instance=\"mail\",result=\"success\"}", "otelfwd_instance"),
           "prom label removed: the label in front of another one goes with its comma");

    Check ("otelfwd_x{a=\"1\",b=\"2\"}" == PromRemoveLabel ("otelfwd_x{a=\"1\",otelfwd_instance=\"mail\",b=\"2\"}", "otelfwd_instance"),
           "prom label removed: a label in the middle goes with one comma");

    Check ("otelfwd_x{a=\"1\"}" == PromRemoveLabel ("otelfwd_x{a=\"1\",otelfwd_instance=\"mail\"}", "otelfwd_instance"),
           "prom label removed: the last label goes with the comma before it");

    Check ("otelfwd_health" == PromRemoveLabel ("otelfwd_health", "otelfwd_instance"),
           "prom label removed: a name without labels is not changed");

    Check ("otelfwd_x{result=\"a\"}" == PromRemoveLabel ("otelfwd_x{result=\"a\"}", "otelfwd_instance"),
           "prom label removed: a name without this label is not changed");

    Check ("otelfwd_x{my_otelfwd_instance=\"a\"}" == PromRemoveLabel ("otelfwd_x{my_otelfwd_instance=\"a\"}", "otelfwd_instance"),
           "prom label removed: the end of the name of another label is not the label");

    Check ("otelfwd_health" == PromRemoveLabel ("otelfwd_health{otelfwd_instance=\"a\\\"b,c\"}", "otelfwd_instance"),
           "prom label removed: a value with an escaped quote and a comma in it is removed as a whole");

    /* Adding it and removing it again gives the name back. Empty braces are the one exception: the braces are gone with the label */
    struct RoundTrip { const char *pszName; const char *pszBack; };

    const RoundTrip Names[] = { { "otelfwd_health", "otelfwd_health" },
                                { "otelfwd_push_total{result=\"success\"}", "otelfwd_push_total{result=\"success\"}" },
                                { "otelfwd_x{a=\"1\",b=\"2\"}", "otelfwd_x{a=\"1\",b=\"2\"}" },
                                { "otelfwd_x{}", "otelfwd_x" } };

    for (const RoundTrip& Case : Names)
        Check (std::string (Case.pszBack) == PromRemoveLabel (PromAddLabel (Case.pszName, "otelfwd_instance", "CN=earth/O=NotesLab"), "otelfwd_instance"),
               (std::string ("prom label: adding it and removing it again gives the name back: ") + Case.pszName).c_str());
}


int main ()
{
    setvbuf (stdout, NULL, _IOLBF, 0);

    Group ("otelfwd unit test: failover, converter, log lines");
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

    Group ("Log lines: time stamp and format");
    TestLogTime();
    TestLogLine();
    TestLogOutput();

    Group ("Metrics: the label which tells two instances apart");
    TestPromLabel();

    Group ("File input: the severity, the names of the state files, and the hand over of the position");
    TestFileSeverity();
    TestFileStateName();
    TestFileCommitSlot();

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
