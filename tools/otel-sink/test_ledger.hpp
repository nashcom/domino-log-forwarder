/*
   test_ledger.hpp - the load test ledger of otel-sink.

   The load test program (nginx/loadtest) sends requests like

       GET /otelfwd-test/<thread>/<count>/<sent_ns>

   to NGINX. NGINX logs them, otelfwd forwards the log records as OTLP, and the sink receives them here. The url.path attribute of
   an access log record carries the three numbers. The ledger remembers every {thread, count} it has seen, when it was sent,
   when it arrived and how often, so the sink can tell whether events are missing, delivered twice or slow.

   The ledger lives in memory only. OTLP requests with test records are not written to files.

   Control (through the normal HTTP interface of the sink):
       POST /test/reset   {"threads": 32, "events_per_thread": 100000}   clears everything, sets the expected range
       GET  /test/stats                                                   counters, latency, per thread numbers
       POST /test/fail    {"status": 503}                                 answer OTLP requests with this status (0: stop failing)

   Threads and counts start at 1. The slot of an event is (thread - 1) * events_per_thread + (count - 1), so a lookup needs no hash.
   The sink runs one thread per connection: everything which touches the ledger holds its mutex, for the duration of one batch.

   Latency: end to end is arrival - sent. It is split at the event time NGINX put into the log record ($msec, milliseconds):
       generator_to_nginx = event time - sent      nginx_to_sink = arrival - event time
   All of it is only valid if the load test program and the sink use the same clock. The event time is cut off to whole milliseconds:
   the middle of that millisecond is used, and stage values below zero are cut to zero.
   The two stages are only exact to about half a millisecond. Percentiles come from a histogram (8 steps per doubling): about 10 % exact.
*/

#pragma once

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

namespace sinktest
{

constexpr std::string_view TEST_PREFIX      = "/otelfwd-test/";
constexpr uint64_t         MAX_THREADS      = 4096;
constexpr uint64_t         MAX_EVENTS       = 20000000;     /* 40 bytes per event: 800 MB */
constexpr int64_t          SUMMARY_EVERY_NS = 5000000000LL;


inline int64_t NowNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}


/* Digits only, no sign, no overflow */
inline bool ParseUnsigned (std::string_view Text, uint64_t& retValue)
{
    uint64_t Value = 0;

    if (Text.empty())
        return false;

    for (char c : Text)
    {
        uint64_t Digit = 0;

        if ( (c < '0') || (c > '9') )
            return false;

        Digit = static_cast<uint64_t>(c - '0');

        if (Value > ((UINT64_MAX - Digit) / 10))
            return false;

        Value = (Value * 10) + Digit;
    }

    retValue = Value;
    return true;
}


/* /otelfwd-test/<thread>/<count>/<sent_ns> with exactly three numbers */
inline bool ParseTestPath (std::string_view Path, uint64_t& retThread, uint64_t& retCount, int64_t& retSentNs)
{
    std::string_view Rest;
    size_t Slash1 = 0;
    size_t Slash2 = 0;
    uint64_t SentNs = 0;

    if (Path.substr (0, TEST_PREFIX.size()) != TEST_PREFIX)
        return false;

    Rest   = Path.substr (TEST_PREFIX.size());
    Slash1 = Rest.find ('/');

    if (std::string_view::npos == Slash1)
        return false;

    Slash2 = Rest.find ('/', Slash1 + 1);

    if (std::string_view::npos == Slash2)
        return false;

    if ( (false == ParseUnsigned (Rest.substr (0, Slash1), retThread)) ||
         (false == ParseUnsigned (Rest.substr (Slash1 + 1, Slash2 - Slash1 - 1), retCount)) ||
         (false == ParseUnsigned (Rest.substr (Slash2 + 1), SentNs)) ||
         (SentNs > static_cast<uint64_t>(INT64_MAX)) )
        return false;

    retSentNs = static_cast<int64_t>(SentNs);
    return true;
}


/* Latency numbers of one kind: minimum, average, maximum and a log scale histogram for the percentiles */
class LatencyStats
{
public:

    void Add (int64_t Ns)
    {
        if ( (0 == m_Count) || (Ns < m_Min) )
            m_Min = Ns;

        if ( (0 == m_Count) || (Ns > m_Max) )
            m_Max = Ns;

        m_Sum += static_cast<double>(Ns);
        m_Count++;
        m_Buckets[BucketIndex (Ns)]++;
    }

    void ToJson (rapidjson::Writer<rapidjson::StringBuffer>& Writer) const
    {
        Writer.StartObject();
        Writer.Key ("count");  Writer.Uint64 (m_Count);
        Writer.Key ("min_ns"); Writer.Int64 (m_Min);
        Writer.Key ("avg_ns"); Writer.Int64 (m_Count ? static_cast<int64_t>(m_Sum / static_cast<double>(m_Count)) : 0);
        Writer.Key ("p50_ns"); Writer.Int64 (Percentile (0.50));
        Writer.Key ("p95_ns"); Writer.Int64 (Percentile (0.95));
        Writer.Key ("p99_ns"); Writer.Int64 (Percentile (0.99));
        Writer.Key ("max_ns"); Writer.Int64 (m_Max);
        Writer.EndObject();
    }

private:

    static constexpr size_t BUCKETS = 400;

    /* Microseconds on a log scale with 8 steps per doubling. Bucket 0 is everything below one microsecond (also negative values) */
    static size_t BucketIndex (int64_t Ns)
    {
        uint64_t Us  = (Ns <= 0) ? 0 : static_cast<uint64_t>(Ns) / 1000;
        unsigned Oct = 0;
        unsigned Sub = 0;
        size_t   Idx = 0;

        if (0 == Us)
            return 0;

        Oct = static_cast<unsigned>(63 - __builtin_clzll (Us));

        if (Oct < 3)
            Sub = static_cast<unsigned>((Us - (1ULL << Oct)) << (3 - Oct));
        else
            Sub = static_cast<unsigned>((Us >> (Oct - 3)) & 7);

        Idx = 1 + (Oct * 8) + Sub;

        return (Idx < BUCKETS) ? Idx : (BUCKETS - 1);
    }

    static uint64_t UpperUs (size_t Idx)
    {
        unsigned Oct = static_cast<unsigned>((Idx - 1) / 8);
        unsigned Sub = static_cast<unsigned>((Idx - 1) % 8);

        if (0 == Idx)
            return 1;

        if (Oct >= 3)
            return (1ULL << Oct) + ((Sub + 1ULL) << (Oct - 3));

        return (1ULL << Oct) + (Sub >> (3 - Oct)) + 1;
    }

    int64_t Percentile (double P) const
    {
        uint64_t Target = static_cast<uint64_t>(P * static_cast<double>(m_Count));
        uint64_t Seen   = 0;

        if (0 == m_Count)
            return 0;

        if (Target < 1)
            Target = 1;

        for (size_t i = 0; i < BUCKETS; i++)
        {
            Seen += m_Buckets[i];

            if (Seen >= Target)
                return std::min (static_cast<int64_t>(UpperUs (i) * 1000ULL), m_Max);
        }

        return m_Max;
    }

    uint64_t m_Buckets[BUCKETS] = {0};
    uint64_t m_Count = 0;
    int64_t  m_Min   = 0;
    int64_t  m_Max   = 0;
    double   m_Sum   = 0;
};


class Ledger
{
public:

    struct Result
    {
        size_t Records     = 0;     /* log records in the request */
        size_t TestRecords = 0;     /* records with an /otelfwd-test/ url.path */
    };

    /* POST /test/reset: clears everything and sets the range of valid events */
    bool Reset (uint64_t Threads, uint64_t EventsPerThread, std::string& retError)
    {
        if ( (0 == Threads) || (Threads > MAX_THREADS) )
        {
            retError = "threads must be between 1 and " + std::to_string (MAX_THREADS);
            return false;
        }

        if ( (0 == EventsPerThread) || (EventsPerThread > (MAX_EVENTS / Threads)) )
        {
            retError = "threads * events_per_thread must be between 1 and " + std::to_string (MAX_EVENTS);
            return false;
        }

        std::lock_guard<std::mutex> Lock (m_Mutex);

        m_Slots.assign (static_cast<size_t>(Threads * EventsPerThread), Slot());
        m_UniquePerThread.assign (static_cast<size_t>(Threads), 0);
        m_DeliveriesPerThread.assign (static_cast<size_t>(Threads), 0);

        m_Threads      = Threads;
        m_PerThread    = EventsPerThread;
        m_bActive      = true;
        m_Unique       = 0;
        m_Deliveries   = 0;
        m_Unexpected   = 0;
        m_Malformed    = 0;
        m_TestBatches  = 0;
        m_MixedBatches = 0;
        m_Conflicts    = 0;
        m_OtherRecords = 0;
        m_ReportedLost = 0;
        m_ReportedMissing = 0;
        m_OtherSample.clear();
        m_MaxDupDelay  = 0;
        m_ResetNs      = NowNs();
        m_FirstArrival = 0;
        m_LastArrival  = 0;
        m_LastPrint    = m_ResetNs;
        m_EndToEnd     = LatencyStats();
        m_GenToNginx   = LatencyStats();
        m_NginxToSink  = LatencyStats();
        m_Failed.store (0);

        return true;
    }

    /* Looks at every log record of an OTLP JSON request. Test records are stored, all others are only counted.
       retSummary is filled with a line for the console about every five seconds while test records arrive. */
    Result ProcessOtlp (const rapidjson::Document& Doc, int64_t ArrivalNs, std::string& retSummary)
    {
        struct Parsed { uint64_t Thread; uint64_t Count; int64_t SentNs; int64_t EventNs; };

        Result Res;
        std::vector<Parsed> Events;
        size_t Malformed = 0;
        size_t Others = 0;
        std::vector<std::string> OtherSample;
        std::vector<Parsed> Reports;

        if ( (false == Doc.IsObject()) || (false == Doc.HasMember ("resourceLogs")) || (false == Doc["resourceLogs"].IsArray()) )
            return Res;

        for (const auto& Resource : Doc["resourceLogs"].GetArray())
        {
            if ( (false == Resource.IsObject()) || (false == Resource.HasMember ("scopeLogs")) || (false == Resource["scopeLogs"].IsArray()) )
                continue;

            for (const auto& Scope : Resource["scopeLogs"].GetArray())
            {
                if ( (false == Scope.IsObject()) || (false == Scope.HasMember ("logRecords")) || (false == Scope["logRecords"].IsArray()) )
                    continue;

                for (const auto& Record : Scope["logRecords"].GetArray())
                {
                    std::string_view Path;
                    Parsed Ev {0, 0, 0, 0};
                    uint64_t EventNs = 0;

                    Res.Records++;

                    if ( (false == Record.IsObject()) || (false == GetUrlPath (Record, Path)) || (Path.substr (0, TEST_PREFIX.size()) != TEST_PREFIX) )
                    {
                        /* Not a test event, for example an NGINX error log line. Counted, and the first few are kept as samples */
                        Others++;

                        if ( (OtherSample.size() < 3) && Record.IsObject() && Record.HasMember ("body") && Record["body"].IsObject() &&
                             Record["body"].HasMember ("stringValue") && Record["body"]["stringValue"].IsString() )
                            OtherSample.push_back (std::string (Record["body"]["stringValue"].GetString(), std::min<size_t>(Record["body"]["stringValue"].GetStringLength(), 200)));

                        /* A line of NGINX about a request which it could not log names that request:
                           ... send() to syslog failed while logging request, ... request: "GET /otelfwd-test/23/14559/1789... HTTP/1.1" ...
                           The event of that request is lost, and the line proves why */
                        if ( Record.IsObject() && Record.HasMember ("body") && Record["body"].IsObject() &&
                             Record["body"].HasMember ("stringValue") && Record["body"]["stringValue"].IsString() )
                        {
                            std::string_view Text (Record["body"]["stringValue"].GetString(), Record["body"]["stringValue"].GetStringLength());
                            size_t Pos = Text.find (TEST_PREFIX);

                            if (std::string_view::npos != Pos)
                            {
                                size_t End = Text.find_first_of (" \"", Pos);
                                Parsed R {0, 0, 0, 0};

                                if (ParseTestPath (Text.substr (Pos, (std::string_view::npos == End) ? std::string_view::npos : (End - Pos)), R.Thread, R.Count, R.SentNs))
                                    Reports.push_back (R);
                            }
                        }

                        continue;
                    }

                    Res.TestRecords++;

                    if (false == ParseTestPath (Path, Ev.Thread, Ev.Count, Ev.SentNs))
                    {
                        Malformed++;
                        continue;
                    }

                    if (Record.HasMember ("timeUnixNano") && Record["timeUnixNano"].IsString())
                    {
                        ParseUnsigned (std::string_view (Record["timeUnixNano"].GetString(), Record["timeUnixNano"].GetStringLength()), EventNs);
                        Ev.EventNs = (EventNs <= static_cast<uint64_t>(INT64_MAX)) ? static_cast<int64_t>(EventNs) : 0;
                    }

                    Events.push_back (Ev);
                }
            }
        }

        if ( (0 == Res.TestRecords) && (0 == Others) )
            return Res;

        std::lock_guard<std::mutex> Lock (m_Mutex);

        /* Records which are not test events, counted while a test is active. NGINX writes its own problems to the same syslog
           socket, for example "send() failed" when the socket is full: an event lost before otelfwd is replaced by such a line */
        if (m_bActive && (Others > 0))
        {
            m_OtherRecords += Others;

            for (const std::string& Sample : OtherSample)
            {
                if (m_OtherSample.size() < 5)
                    m_OtherSample.push_back (Sample);
            }

            for (const Parsed& R : Reports)
            {
                if ( (R.Thread < 1) || (R.Thread > m_Threads) || (R.Count < 1) || (R.Count > m_PerThread) )
                    continue;

                Slot& S = m_Slots[static_cast<size_t>(((R.Thread - 1) * m_PerThread) + (R.Count - 1))];

                if (0 == S.Reported)
                {
                    S.Reported = 1;
                    m_ReportedLost++;

                    if (0 == S.Received)
                        m_ReportedMissing++;
                }
            }
        }

        if (0 == Res.TestRecords)
            return Res;

        m_TestBatches++;
        m_Malformed += Malformed;

        if (Res.TestRecords < Res.Records)
            m_MixedBatches++;

        for (const Parsed& Ev : Events)
        {
            if ( (false == m_bActive) || (Ev.Thread < 1) || (Ev.Thread > m_Threads) || (Ev.Count < 1) || (Ev.Count > m_PerThread) )
            {
                m_Unexpected++;
                continue;
            }

            Slot& S = m_Slots[static_cast<size_t>(((Ev.Thread - 1) * m_PerThread) + (Ev.Count - 1))];

            m_Deliveries++;
            m_DeliveriesPerThread[static_cast<size_t>(Ev.Thread - 1)]++;

            if (0 == m_FirstArrival)
                m_FirstArrival = ArrivalNs;

            m_LastArrival = ArrivalNs;

            if (0 == S.Received)
            {
                S.SentNs   = Ev.SentNs;
                S.FirstNs  = ArrivalNs;
                S.LastNs   = ArrivalNs;
                S.EventNs  = Ev.EventNs;
                S.Received = 1;

                if (S.Reported && (m_ReportedMissing > 0))
                    m_ReportedMissing--;

                m_Unique++;
                m_UniquePerThread[static_cast<size_t>(Ev.Thread - 1)]++;
                m_EndToEnd.Add (ArrivalNs - Ev.SentNs);

                if (Ev.EventNs > 0)
                {
                    /* NGINX logs the time in whole milliseconds (cut off). The middle of that millisecond is the best estimate of the real time.
                       Values below zero are cut to zero: they are only the rounding. */
                    int64_t EventMid = Ev.EventNs + 500000;

                    m_GenToNginx.Add (std::max<int64_t> (0, EventMid - Ev.SentNs));
                    m_NginxToSink.Add (std::max<int64_t> (0, ArrivalNs - EventMid));
                }
            }
            else
            {
                S.LastNs = ArrivalNs;
                S.Received++;

                if (S.SentNs != Ev.SentNs)
                    m_Conflicts++;

                m_MaxDupDelay = std::max (m_MaxDupDelay, ArrivalNs - S.FirstNs);
            }
        }

        if ( (ArrivalNs - m_LastPrint) >= SUMMARY_EVERY_NS )
        {
            char szLine[256] = {0};

            m_LastPrint = ArrivalNs;

            snprintf (szLine, sizeof (szLine), "test: %llu of %llu unique events, %llu deliveries, %llu unexpected, %llu malformed",
                      static_cast<unsigned long long>(m_Unique), static_cast<unsigned long long>(m_Threads * m_PerThread),
                      static_cast<unsigned long long>(m_Deliveries), static_cast<unsigned long long>(m_Unexpected),
                      static_cast<unsigned long long>(m_Malformed));
            retSummary = szLine;
        }

        return Res;
    }

    /* GET /test/stats */
    std::string StatsJson()
    {
        rapidjson::StringBuffer Buffer;
        rapidjson::Writer<rapidjson::StringBuffer> W (Buffer);
        std::lock_guard<std::mutex> Lock (m_Mutex);
        uint64_t Expected = m_Threads * m_PerThread;

        W.StartObject();
        W.Key ("active");              W.Bool (m_bActive);
        W.Key ("expected");            W.Uint64 (Expected);
        W.Key ("unique");              W.Uint64 (m_Unique);
        W.Key ("deliveries");          W.Uint64 (m_Deliveries);
        W.Key ("duplicates");          W.Uint64 (m_Deliveries - m_Unique);
        W.Key ("missing");             W.Uint64 (Expected - m_Unique);
        W.Key ("unexpected");          W.Uint64 (m_Unexpected);
        W.Key ("malformed");           W.Uint64 (m_Malformed);
        W.Key ("mixed_batches");       W.Uint64 (m_MixedBatches);
        W.Key ("test_batches");        W.Uint64 (m_TestBatches);
        W.Key ("timestamp_conflicts"); W.Uint64 (m_Conflicts);
        W.Key ("other_records");       W.Uint64 (m_OtherRecords);
        W.Key ("nginx_reported_lost"); W.Uint64 (m_ReportedLost);
        W.Key ("missing_reported");    W.Uint64 (m_ReportedMissing);
        W.Key ("other_sample");
        W.StartArray();

        for (const std::string& Sample : m_OtherSample)
            W.String (Sample.c_str(), static_cast<rapidjson::SizeType>(Sample.size()));

        W.EndArray();
        W.Key ("failed_requests");     W.Uint64 (m_Failed.load());
        W.Key ("fail_status");         W.Int (m_FailStatus.load());
        W.Key ("reset_ns");            W.Int64 (m_ResetNs);
        W.Key ("first_arrival_ns");    W.Int64 (m_FirstArrival);
        W.Key ("last_arrival_ns");     W.Int64 (m_LastArrival);
        W.Key ("max_duplicate_delay_ns"); W.Int64 (m_MaxDupDelay);

        /* Flat names of the end to end latency, and the two stages */
        W.Key ("latency");             m_EndToEnd.ToJson (W);
        W.Key ("stages");
        W.StartObject();
        W.Key ("generator_to_nginx");  m_GenToNginx.ToJson (W);
        W.Key ("nginx_to_sink");       m_NginxToSink.ToJson (W);
        W.EndObject();

        /* The first missing events: a run of consecutive counts of one thread points to a burst which was lost */
        W.Key ("missing_sample");
        W.StartArray();

        if (Expected > m_Unique)
        {
            int Found = 0;

            for (size_t i = 0; (i < m_Slots.size()) && (Found < 10); i++)
            {
                if (0 == m_Slots[i].Received)
                {
                    W.StartObject();
                    W.Key ("thread"); W.Uint64 ((i / m_PerThread) + 1);
                    W.Key ("count");  W.Uint64 ((i % m_PerThread) + 1);
                    W.EndObject();
                    Found++;
                }
            }
        }

        W.EndArray();

        W.Key ("threads");
        W.StartArray();

        for (uint64_t t = 0; t < m_Threads; t++)
        {
            W.StartObject();
            W.Key ("thread");     W.Uint64 (t + 1);
            W.Key ("expected");   W.Uint64 (m_PerThread);
            W.Key ("unique");     W.Uint64 (m_UniquePerThread[static_cast<size_t>(t)]);
            W.Key ("deliveries"); W.Uint64 (m_DeliveriesPerThread[static_cast<size_t>(t)]);
            W.Key ("duplicates"); W.Uint64 (m_DeliveriesPerThread[static_cast<size_t>(t)] - m_UniquePerThread[static_cast<size_t>(t)]);
            W.Key ("missing");    W.Uint64 (m_PerThread - m_UniquePerThread[static_cast<size_t>(t)]);
            W.EndObject();
        }

        W.EndArray();
        W.EndObject();

        return std::string (Buffer.GetString(), Buffer.GetSize());
    }

    /* POST /test/fail: OTLP requests are answered with this status and are not processed. 0 stops it */
    void SetFailStatus (int Status)
    {
        m_FailStatus.store (Status);
    }

    int FailStatus() const
    {
        return m_FailStatus.load();
    }

    void CountFailedRequest()
    {
        m_Failed.fetch_add (1);
    }

private:

    struct Slot
    {
        int64_t  SentNs   = 0;
        int64_t  FirstNs  = 0;
        int64_t  LastNs   = 0;
        int64_t  EventNs  = 0;
        uint32_t Received = 0;      /* 0: not received, 1: delivered once, more: duplicate or WAL replay */
        uint8_t  Reported = 0;      /* NGINX wrote a "send() to syslog failed" line which names this event */
    };

    /* The url.path attribute of a log record: attributes is an array of {"key": "...", "value": {"stringValue": "..."}} */
    static bool GetUrlPath (const rapidjson::Value& Record, std::string_view& retPath)
    {
        if ( (false == Record.HasMember ("attributes")) || (false == Record["attributes"].IsArray()) )
            return false;

        for (const auto& Attr : Record["attributes"].GetArray())
        {
            if ( (false == Attr.IsObject()) || (false == Attr.HasMember ("key")) || (false == Attr["key"].IsString()) )
                continue;

            if (0 != strcmp (Attr["key"].GetString(), "url.path"))
                continue;

            if ( Attr.HasMember ("value") && Attr["value"].IsObject() && Attr["value"].HasMember ("stringValue") && Attr["value"]["stringValue"].IsString() )
            {
                retPath = std::string_view (Attr["value"]["stringValue"].GetString(), Attr["value"]["stringValue"].GetStringLength());
                return true;
            }

            return false;
        }

        return false;
    }

    std::mutex            m_Mutex;
    std::vector<Slot>     m_Slots;
    std::vector<uint64_t> m_UniquePerThread;
    std::vector<uint64_t> m_DeliveriesPerThread;
    uint64_t              m_Threads      = 0;
    uint64_t              m_PerThread    = 0;
    bool                  m_bActive      = false;
    uint64_t              m_Unique       = 0;
    uint64_t              m_Deliveries   = 0;
    uint64_t              m_Unexpected   = 0;
    uint64_t              m_Malformed    = 0;
    uint64_t              m_TestBatches  = 0;
    uint64_t              m_MixedBatches = 0;
    uint64_t              m_Conflicts    = 0;
    uint64_t              m_OtherRecords = 0;
    uint64_t              m_ReportedLost = 0;       /* events which NGINX names in a send() failure */
    uint64_t              m_ReportedMissing = 0;    /* of them, the ones which did not arrive */
    std::vector<std::string> m_OtherSample;
    int64_t               m_MaxDupDelay  = 0;
    int64_t               m_ResetNs      = 0;
    int64_t               m_FirstArrival = 0;
    int64_t               m_LastArrival  = 0;
    int64_t               m_LastPrint    = 0;
    LatencyStats          m_EndToEnd;
    LatencyStats          m_GenToNginx;
    LatencyStats          m_NginxToSink;
    std::atomic<uint64_t> m_Failed {0};
    std::atomic<int>      m_FailStatus {0};
};

} /* namespace sinktest */
