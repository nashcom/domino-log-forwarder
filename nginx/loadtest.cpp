/*
   loadtest - load test of the whole path

       loadtest --> NGINX (access log JSON) --> syslog socket --> otelfwd --> OTLP --> otel-sink

   The program generates the requests and checks what arrives in the sink:

     1. POST /test/reset to the sink: expected number of threads and requests
     2. every sender thread sends GET /otelfwd-test/<thread>/<count>/<sent_ns> to NGINX (answered with 404 and logged),
        one request in flight per thread, over its own keep-alive connection. A request is never sent twice: a request which
        got no answer counts as failed, its outcome is unknown, and it must not look like a duplicate of the forwarder.
     3. wait until every event has arrived in the sink, or nothing arrives anymore, or the wait time is over
     4. GET /test/stats from the sink and print the result: missing, duplicates, latency, throughput, and what otelfwd reports

   PASS: nothing missing, nothing unexpected or malformed, no conflicting time stamps, and every request was sent and answered
   with 404. Duplicates are allowed (WAL replay is at least once delivery) and reported.

   Build:   make loadtest        (needs the rapidjson headers)
   Help:    loadtest --help
*/

#include <algorithm>
#include <atomic>
#include <chrono>
#include <csignal>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <thread>
#include <vector>

#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>

#include <rapidjson/document.h>

using Clock = std::chrono::steady_clock;

static std::atomic<int>  g_Abort {0};
static std::atomic<bool> g_Go    {false};


static void OnSignal (int)
{
    g_Abort = 1;
}


static int64_t NowUnixNs()
{
    return std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::system_clock::now().time_since_epoch()).count();
}


static double SecondsSince (Clock::time_point Start)
{
    return std::chrono::duration<double>(Clock::now() - Start).count();
}


static void SleepMs (int Ms)
{
    std::this_thread::sleep_for (std::chrono::milliseconds (Ms));
}


struct Config
{
    std::string NginxUrl   = "http://127.0.0.1:18080";
    std::string SinkUrl    = "http://127.0.0.1:4318";
    std::string Token;
    std::string PromFile;
    unsigned    Threads    = 8;
    uint64_t    Requests   = 10000;         /* per thread */
    double      Rate       = 0;             /* requests per second and thread, 0: as fast as the answers come */
    int         TimeoutSec = 10;            /* one HTTP request */
    int         WaitSec    = 60;            /* how long to wait for the last events after the last request */
    int         StallSec   = 15;            /* stop waiting when nothing new arrived for this long */
    int         FailForSec = 0;             /* WAL test: the sink answers 503 for this many seconds */
    bool        bVerbose   = false;
    bool        bExpectFailover = false;    /* the primary endpoint of otelfwd fails: everything has to arrive through the backup */
    bool        bExpectReject   = false;    /* the primary endpoint refuses the data with 400: nothing arrives, nothing goes to the backup */
};


/* ---- Minimal HTTP/1.1 client: plain http, keep-alive, Content-Length ---- */

struct Endpoint
{
    std::string Host;
    std::string Port = "80";
};


static bool ParseUrl (const std::string& Url, Endpoint& retEndpoint)
{
    std::string Rest;
    size_t Slash = 0;
    size_t Colon = 0;

    if (0 != Url.compare (0, 7, "http://"))
        return false;

    Rest  = Url.substr (7);
    Slash = Rest.find ('/');

    if (std::string::npos != Slash)
        Rest = Rest.substr (0, Slash);

    Colon = Rest.rfind (':');

    if (std::string::npos != Colon)
    {
        retEndpoint.Host = Rest.substr (0, Colon);
        retEndpoint.Port = Rest.substr (Colon + 1);
    }
    else
    {
        retEndpoint.Host = Rest;
    }

    return (false == retEndpoint.Host.empty());
}


class HttpConn
{
public:

    HttpConn (const Endpoint& Target, int TimeoutSec) : m_Target (Target), m_TimeoutSec (TimeoutSec) {}

    ~HttpConn()
    {
        Close();
    }

    HttpConn (const HttpConn&) = delete;
    HttpConn& operator= (const HttpConn&) = delete;

    bool IsOpen() const
    {
        return (m_Fd >= 0);
    }

    void Close()
    {
        if (m_Fd >= 0)
            close (m_Fd);

        m_Fd = -1;
        m_Buffer.clear();
    }

    bool Connect()
    {
        struct addrinfo Hints {};
        struct addrinfo *pList = NULL;
        struct timeval  Tv {};
        int opt = 1;

        Close();

        Hints.ai_family   = AF_UNSPEC;
        Hints.ai_socktype = SOCK_STREAM;

        if (0 != getaddrinfo (m_Target.Host.c_str(), m_Target.Port.c_str(), &Hints, &pList))
            return false;

        for (struct addrinfo *p = pList; p; p = p->ai_next)
        {
            int fd = socket (p->ai_family, p->ai_socktype | SOCK_CLOEXEC, p->ai_protocol);

            if (fd < 0)
                continue;

            Tv.tv_sec = m_TimeoutSec;
            setsockopt (fd, SOL_SOCKET, SO_RCVTIMEO, &Tv, sizeof (Tv));
            setsockopt (fd, SOL_SOCKET, SO_SNDTIMEO, &Tv, sizeof (Tv));
            setsockopt (fd, IPPROTO_TCP, TCP_NODELAY, &opt, sizeof (opt));

            if (0 == connect (fd, p->ai_addr, p->ai_addrlen))
            {
                m_Fd = fd;
                break;
            }

            close (fd);
        }

        freeaddrinfo (pList);

        return (m_Fd >= 0);
    }

    /* One request and its answer. retServerCloses: the connection is closed after this answer.
       Returns false if the request could not be sent or the answer is incomplete. The connection is closed then. */
    bool Request (const std::string& Method, const std::string& Path, const std::string& Body, const std::string& Token,
                  int& retStatus, std::string& retBody, bool& retServerCloses)
    {
        std::string Req = Method + " " + Path + " HTTP/1.1\r\nHost: " + m_Target.Host + ":" + m_Target.Port + "\r\n" +
                          "User-Agent: otelfwd-loadtest\r\nAccept: */*\r\n";
        size_t HeaderEnd = 0;
        size_t ContentLength = 0;
        bool   bHaveLength = false;
        std::string Head;

        retServerCloses = false;
        retBody.clear();

        if (false == Token.empty())
            Req += "Authorization: Bearer " + Token + "\r\n";

        if ("POST" == Method)
            Req += "Content-Type: application/json\r\nContent-Length: " + std::to_string (Body.size()) + "\r\n";

        Req += "\r\n";
        Req += Body;

        if (false == SendAll (Req))
        {
            Close();
            return false;
        }

        while (std::string::npos == (HeaderEnd = m_Buffer.find ("\r\n\r\n")))
        {
            if (false == ReadMore())
            {
                Close();
                return false;
            }
        }

        Head = m_Buffer.substr (0, HeaderEnd);
        m_Buffer.erase (0, HeaderEnd + 4);

        if ( (Head.size() < 12) || (0 != Head.compare (0, 5, "HTTP/")) )
        {
            Close();
            return false;
        }

        retStatus = atoi (Head.c_str() + 9);

        {
            std::string Lower = Head;

            for (char& c : Lower)
                c = static_cast<char>(tolower (static_cast<unsigned char>(c)));

            size_t Pos = Lower.find ("\r\ncontent-length:");

            if (std::string::npos != Pos)
            {
                ContentLength = static_cast<size_t>(strtoull (Lower.c_str() + Pos + 17, NULL, 10));
                bHaveLength   = true;
            }

            retServerCloses = (std::string::npos != Lower.find ("\r\nconnection: close")) || (0 == Lower.compare (0, 8, "http/1.0"));

            if (std::string::npos != Lower.find ("\r\ntransfer-encoding:"))
            {
                Close();
                return false;
            }
        }

        if (false == bHaveLength)
        {
            /* Without a length the answer ends when the server closes the connection */
            while (ReadMore())
                ;

            retBody         = m_Buffer;
            m_Buffer.clear();
            retServerCloses = true;
        }
        else
        {
            while (m_Buffer.size() < ContentLength)
            {
                if (false == ReadMore())
                {
                    Close();
                    return false;
                }
            }

            retBody = m_Buffer.substr (0, ContentLength);
            m_Buffer.erase (0, ContentLength);
        }

        if (retServerCloses)
            Close();

        return true;
    }

private:

    bool SendAll (const std::string& Data)
    {
        size_t Sent = 0;

        while (Sent < Data.size())
        {
            ssize_t n = send (m_Fd, Data.data() + Sent, Data.size() - Sent, MSG_NOSIGNAL);

            if (n <= 0)
                return false;

            Sent += static_cast<size_t>(n);
        }

        return true;
    }

    bool ReadMore()
    {
        char Chunk[16384];
        ssize_t n = recv (m_Fd, Chunk, sizeof (Chunk), 0);

        if (n <= 0)
            return false;

        m_Buffer.append (Chunk, static_cast<size_t>(n));
        return true;
    }

    Endpoint    m_Target;
    int         m_TimeoutSec;
    int         m_Fd = -1;
    std::string m_Buffer;
};


/* A request to the sink: its own connection, the sink closes it after every answer */
static bool SinkRequest (const Endpoint& Sink, const Config& Cfg, const std::string& Method, const std::string& Path, const std::string& Body,
                         int& retStatus, std::string& retBody)
{
    HttpConn Conn (Sink, Cfg.TimeoutSec);
    bool bClose = false;

    if (false == Conn.Connect())
        return false;

    return Conn.Request (Method, Path, Body, Cfg.Token, retStatus, retBody, bClose);
}


/* ---- Sender threads ---- */

struct ThreadResult
{
    std::atomic<uint64_t> Done {0};     /* for the progress line: requests finished, read by the main thread */
    uint64_t Sent      = 0;             /* requests which got an answer */
    uint64_t Failed    = 0;             /* no answer: connect, send, timeout. Outcome unknown, never repeated */
    uint64_t BadStatus = 0;             /* answered with something else than 404 */
    double   RtSum     = 0;
    int64_t  RtMinNs   = 0;
    int64_t  RtMaxNs   = 0;
};


static void SenderThread (const Config *pCfg, const Endpoint *pNginx, unsigned ThreadId, ThreadResult *pResult)
{
    HttpConn Conn (*pNginx, pCfg->TimeoutSec);
    Clock::time_point Start;
    std::string Body;

    while (false == g_Go.load())
        SleepMs (1);

    Start = Clock::now();

    for (uint64_t Count = 1; (Count <= pCfg->Requests) && (0 == g_Abort.load()); Count++)
    {
        int  Status  = 0;
        bool bClose  = false;
        int64_t SentNs = 0;
        Clock::time_point T0;
        std::string Path;

        pResult->Done.store (Count - 1, std::memory_order_relaxed);

        /* Pacing: request number Count is due Count / Rate seconds after the start */
        if (pCfg->Rate > 0)
        {
            double Due = static_cast<double>(Count - 1) / pCfg->Rate;
            double Now = SecondsSince (Start);

            if (Due > Now)
                std::this_thread::sleep_for (std::chrono::duration<double>(Due - Now));
        }

        if (false == Conn.IsOpen())
        {
            if (false == Conn.Connect())
            {
                /* No request was sent. This count is lost: it is never sent again */
                pResult->Failed++;
                SleepMs (10);
                continue;
            }
        }

        /* The time is taken right before the request is sent */
        SentNs = NowUnixNs();
        T0     = Clock::now();
        Path   = "/otelfwd-test/" + std::to_string (ThreadId) + "/" + std::to_string (Count) + "/" + std::to_string (SentNs);

        if (false == Conn.Request ("GET", Path, "", "", Status, Body, bClose))
        {
            pResult->Failed++;
            continue;
        }

        {
            int64_t RtNs = std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - T0).count();

            if ( (0 == pResult->Sent) || (RtNs < pResult->RtMinNs) )
                pResult->RtMinNs = RtNs;

            if (RtNs > pResult->RtMaxNs)
                pResult->RtMaxNs = RtNs;

            pResult->RtSum += static_cast<double>(RtNs);
        }

        pResult->Sent++;

        if (404 != Status)
            pResult->BadStatus++;
    }

    /* Done counts the requests which are finished. The last one is only counted here, not at the start of its turn */
    if (0 == g_Abort.load())
        pResult->Done.store (pCfg->Requests, std::memory_order_relaxed);
}


/* ---- otelfwd metrics file (Prometheus text format) ---- */

struct PromValues
{
    bool   bValid = false;
    double LastUpdate = 0;
    std::map<std::string, double> Values;
};


static bool ReadProm (const std::string& File, PromValues& retProm)
{
    std::ifstream In (File);
    std::string Line;

    retProm = PromValues();

    if (false == In.is_open())
        return false;

    while (std::getline (In, Line))
    {
        size_t Space = Line.rfind (' ');

        if ( Line.empty() || ('#' == Line[0]) || (std::string::npos == Space) )
            continue;

        retProm.Values[Line.substr (0, Space)] = atof (Line.c_str() + Space + 1);
    }

    retProm.LastUpdate = retProm.Values.count ("otelfwd_lastupdate_timestamp_seconds") ? retProm.Values["otelfwd_lastupdate_timestamp_seconds"] : 0;
    retProm.bValid     = (retProm.LastUpdate > 0);

    return retProm.bValid;
}


/* otelfwd writes the file every 10 seconds, and its time stamp has a resolution of one second. After is a second which has not begun yet:
   a file with that time stamp was written after the events which are to be counted */
static bool ReadFreshProm (const std::string& File, PromValues& retProm, time_t After, int MaxSec)
{
    for (int i = 0; (i < (MaxSec * 2)) && (0 == g_Abort.load()); i++)
    {
        if (ReadProm (File, retProm) && (retProm.LastUpdate >= static_cast<double>(After)))
            return true;

        SleepMs (500);
    }

    return false;
}


static double Metric (const PromValues& Prom, const char *pszName)
{
    auto it = Prom.Values.find (pszName);

    return (it == Prom.Values.end()) ? 0 : it->second;
}


/* ---- Output ---- */

static std::string Ms (int64_t Ns)
{
    char szText[32] = {0};

    snprintf (szText, sizeof (szText), "%.1f ms", static_cast<double>(Ns) / 1e6);
    return szText;
}


static uint64_t GetU64 (const rapidjson::Value& Obj, const char *pszName)
{
    return (Obj.HasMember (pszName) && Obj[pszName].IsUint64()) ? Obj[pszName].GetUint64() : 0;
}


static int64_t GetI64 (const rapidjson::Value& Obj, const char *pszName)
{
    return (Obj.HasMember (pszName) && Obj[pszName].IsInt64()) ? Obj[pszName].GetInt64() : 0;
}


static std::string LatencyLine (const rapidjson::Value& Lat)
{
    char szText[256] = {0};

    if ( (false == Lat.IsObject()) || (0 == GetU64 (Lat, "count")) )
        return "no data";

    snprintf (szText, sizeof (szText), "min %s  avg %s  p50 %s  p95 %s  p99 %s  max %s",
              Ms (GetI64 (Lat, "min_ns")).c_str(), Ms (GetI64 (Lat, "avg_ns")).c_str(), Ms (GetI64 (Lat, "p50_ns")).c_str(),
              Ms (GetI64 (Lat, "p95_ns")).c_str(), Ms (GetI64 (Lat, "p99_ns")).c_str(), Ms (GetI64 (Lat, "max_ns")).c_str());
    return szText;
}


static void PrintHelp()
{
    printf ("loadtest - load test of NGINX -> otelfwd -> otel-sink\n\n"
            "  --nginx URL          test NGINX             (default http://127.0.0.1:18080)\n"
            "  --sink URL           otel-sink              (default http://127.0.0.1:4318)\n"
            "  --token TOKEN        bearer token of the sink, if it needs one (OTEL_SINK_TOKEN)\n"
            "  --threads N          sender threads         (default 8)\n"
            "  --requests N         requests per thread    (default 10000)\n"
            "  --rate N             requests per second and thread, 0 = as fast as possible (default 0)\n"
            "  --timeout SEC        timeout of one HTTP request (default 10)\n"
            "  --wait SEC           how long to wait for the last events after the last request (default 60)\n"
            "  --stall SEC          stop waiting when nothing new arrived for this long (default 15)\n"
            "  --otelfwd-prom FILE  metrics file of otelfwd (<data dir>/domino/stats/otelfwd.prom): shows where events were lost\n"
            "  --fail-for SEC       WAL test: the sink answers 503 for this many seconds. otelfwd retries from its WAL after that.\n"
            "                       Its WAL retry waits up to 2 minutes, so use --wait 300 or more\n"
            "  --expect-failover    the primary endpoint of otelfwd fails (503) and there is a backup endpoint. The test passes if every event\n"
            "                       arrived through the backup, and otelfwd made a failover. Needs --otelfwd-prom. run_loadtest.sh --backup sets it up\n"
            "  --expect-reject      the primary endpoint refuses the data with 400 and there is a backup endpoint. The test passes if NOTHING arrived:\n"
            "                       the data is dropped, not kept for a retry, and not sent to the backup. Needs --otelfwd-prom. run_loadtest.sh --reject sets it up\n"
            "  --verbose            list every thread\n\n"
            "Exit code: 0 PASS, 1 FAIL, 2 the test could not run\n");
}


static bool ParseArgs (int argc, char *argv[], Config& Cfg)
{
    for (int i = 1; i < argc; i++)
    {
        std::string Arg = argv[i];
        auto Next = [&] () -> const char * { return (i + 1 < argc) ? argv[++i] : NULL; };
        const char *pValue = NULL;

        if ( ("--help" == Arg) || ("-h" == Arg) )
        {
            PrintHelp();
            exit (0);
        }
        else if ("--verbose" == Arg)
        {
            Cfg.bVerbose = true;
        }
        else if ("--expect-failover" == Arg)
        {
            Cfg.bExpectFailover = true;
        }
        else if ("--expect-reject" == Arg)
        {
            Cfg.bExpectReject = true;
        }
        else if ( ("--nginx" == Arg) || ("--sink" == Arg) || ("--token" == Arg) || ("--otelfwd-prom" == Arg) || ("--threads" == Arg) ||
                  ("--requests" == Arg) || ("--rate" == Arg) || ("--timeout" == Arg) || ("--wait" == Arg) || ("--stall" == Arg) || ("--fail-for" == Arg) )
        {
            pValue = Next();

            if (NULL == pValue)
            {
                fprintf (stderr, "%s needs a value\n", Arg.c_str());
                return false;
            }

            if ("--nginx" == Arg)               Cfg.NginxUrl   = pValue;
            else if ("--sink" == Arg)           Cfg.SinkUrl    = pValue;
            else if ("--token" == Arg)          Cfg.Token      = pValue;
            else if ("--otelfwd-prom" == Arg)   Cfg.PromFile   = pValue;
            else if ("--threads" == Arg)        Cfg.Threads    = static_cast<unsigned>(strtoul (pValue, NULL, 10));
            else if ("--requests" == Arg)       Cfg.Requests   = strtoull (pValue, NULL, 10);
            else if ("--rate" == Arg)           Cfg.Rate       = atof (pValue);
            else if ("--timeout" == Arg)        Cfg.TimeoutSec = atoi (pValue);
            else if ("--wait" == Arg)           Cfg.WaitSec    = atoi (pValue);
            else if ("--stall" == Arg)          Cfg.StallSec   = atoi (pValue);
            else if ("--fail-for" == Arg)       Cfg.FailForSec = atoi (pValue);
        }
        else
        {
            fprintf (stderr, "unknown option %s (see --help)\n", Arg.c_str());
            return false;
        }
    }

    if ( (0 == Cfg.Threads) || (0 == Cfg.Requests) || (Cfg.TimeoutSec < 1) || (Cfg.WaitSec < 0) || (Cfg.StallSec < 1) || (Cfg.Rate < 0) || (Cfg.FailForSec < 0) )
    {
        fprintf (stderr, "invalid value. See --help\n");
        return false;
    }

    if (Cfg.bExpectFailover && Cfg.bExpectReject)
    {
        fprintf (stderr, "--expect-failover and --expect-reject cannot be used together\n");
        return false;
    }

    if ( (Cfg.bExpectFailover || Cfg.bExpectReject) && Cfg.PromFile.empty() )
    {
        fprintf (stderr, "--expect-failover and --expect-reject need --otelfwd-prom: the result is in the metrics of otelfwd\n");
        return false;
    }

    return true;
}


int main (int argc, char *argv[])
{
    Config Cfg;
    Endpoint Nginx;
    Endpoint Sink;
    std::vector<ThreadResult> Results;
    std::vector<std::thread> Senders;
    PromValues PromBefore;
    PromValues PromAfter;
    bool bProm       = false;
    bool bFailSet    = false;
    int  Status      = 0;
    std::string Reply;
    uint64_t Expected = 0;
    double SendSec   = 0;
    Clock::time_point TStart;
    Clock::time_point TSendEnd;
    int64_t SendEndUnix = 0;
    rapidjson::Document Stats;
    std::vector<std::string> Reasons;
    std::thread FailTimer;
    std::atomic<bool> bFailTimerStop {false};

    if (false == ParseArgs (argc, argv, Cfg))
        return 2;

    if ( (false == ParseUrl (Cfg.NginxUrl, Nginx)) || (false == ParseUrl (Cfg.SinkUrl, Sink)) )
    {
        fprintf (stderr, "URLs have to look like http://host:port\n");
        return 2;
    }

    signal (SIGINT, OnSignal);
    signal (SIGTERM, OnSignal);
    signal (SIGPIPE, SIG_IGN);

    Expected = static_cast<uint64_t>(Cfg.Threads) * Cfg.Requests;

    printf ("otelfwd load test: %u threads x %llu requests = %llu events%s\n", Cfg.Threads, static_cast<unsigned long long>(Cfg.Requests),
            static_cast<unsigned long long>(Expected), (Cfg.Rate > 0) ? "" : ", no rate limit");
    printf ("  NGINX %s, sink %s\n", Cfg.NginxUrl.c_str(), Cfg.SinkUrl.c_str());

    /* Baseline of the otelfwd counters. They only ever grow, the result is the difference */
    if (false == Cfg.PromFile.empty())
    {
        printf ("  reading the metrics of otelfwd (written every 10 seconds) ...\n");
        bProm = ReadFreshProm (Cfg.PromFile, PromBefore, time (NULL) + 1, 15);

        if (false == bProm)
            printf ("  Warning: %s is missing or not updated. The otelfwd part of the result is left out.\n", Cfg.PromFile.c_str());
    }

    /* 1. Reset the sink */
    {
        std::string Body = "{\"threads\":" + std::to_string (Cfg.Threads) + ",\"events_per_thread\":" + std::to_string (Cfg.Requests) + "}";

        if ( (false == SinkRequest (Sink, Cfg, "POST", "/test/reset", Body, Status, Reply)) || (200 != Status) )
        {
            fprintf (stderr, "The sink did not accept /test/reset (status %d): %s\n", Status, Reply.c_str());
            fprintf (stderr, "Is the sink running at %s, and is it a version with the load test ledger?\n", Cfg.SinkUrl.c_str());
            return 2;
        }
    }

    /* WAL test: the sink fails for a while. A timer thread ends it, also if the test is aborted */
    if (Cfg.FailForSec > 0)
    {
        if ( (false == SinkRequest (Sink, Cfg, "POST", "/test/fail", "{\"status\":503}", Status, Reply)) || (200 != Status) )
        {
            fprintf (stderr, "The sink did not accept /test/fail (status %d)\n", Status);
            return 2;
        }

        bFailSet = true;
        printf ("  the sink answers 503 for %d seconds (WAL test)\n", Cfg.FailForSec);

        FailTimer = std::thread ([&] ()
        {
            for (int i = 0; (i < (Cfg.FailForSec * 10)) && (false == bFailTimerStop.load()); i++)
                SleepMs (100);

            SinkRequest (Sink, Cfg, "POST", "/test/fail", "{\"status\":0}", Status, Reply);
        });
    }

    /* 2. Send */
    {
        std::vector<ThreadResult> Fresh (Cfg.Threads);      /* atomics: not movable, so no resize */

        Results.swap (Fresh);
    }

    for (unsigned t = 0; t < Cfg.Threads; t++)
        Senders.emplace_back (SenderThread, &Cfg, &Nginx, t + 1, &Results[t]);

    TStart = Clock::now();
    g_Go   = true;

    {
        Clock::time_point Progress = Clock::now();

        /* Progress every 5 seconds while the threads run */
        while (0 == g_Abort.load())
        {
            uint64_t Done = 0;

            for (const ThreadResult& R : Results)
                Done += R.Done.load (std::memory_order_relaxed);

            if (Done >= Expected)
                break;

            if (SecondsSince (Progress) >= 5)
            {
                printf ("  sent about %llu of %llu after %.0f s\n", static_cast<unsigned long long>(Done), static_cast<unsigned long long>(Expected), SecondsSince (TStart));
                fflush (stdout);
                Progress = Clock::now();
            }

            SleepMs (100);
        }

        for (std::thread& Th : Senders)
            Th.join();
    }

    TSendEnd    = Clock::now();
    SendEndUnix = NowUnixNs();
    SendSec     = std::chrono::duration<double>(TSendEnd - TStart).count();

    ThreadResult Sum;
    double RtSumAll = 0;

    for (const ThreadResult& R : Results)
    {
        Sum.Sent      += R.Sent;
        Sum.Failed    += R.Failed;
        Sum.BadStatus += R.BadStatus;
        RtSumAll      += R.RtSum;
        Sum.RtMaxNs    = std::max (Sum.RtMaxNs, R.RtMaxNs);

        if ( (R.Sent > 0) && ((0 == Sum.RtMinNs) || (R.RtMinNs < Sum.RtMinNs)) )
            Sum.RtMinNs = R.RtMinNs;
    }

    printf ("  all requests sent after %.1f s: %llu answered, %llu failed\n", SendSec, static_cast<unsigned long long>(Sum.Sent), static_cast<unsigned long long>(Sum.Failed));

    /* 3. Wait for the events to arrive */
    {
        uint64_t LastUnique   = 0;
        uint64_t LastDeliver  = 0;
        Clock::time_point LastChange = Clock::now();
        Clock::time_point WaitStart  = Clock::now();

        while (0 == g_Abort.load())
        {
            Stats.SetNull();

            if ( SinkRequest (Sink, Cfg, "GET", "/test/stats", "", Status, Reply) && (200 == Status) )
            {
                Stats.Parse (Reply.c_str());

                if (Stats.IsObject())
                {
                    uint64_t Unique   = GetU64 (Stats, "unique");
                    uint64_t Deliver  = GetU64 (Stats, "deliveries");

                    if ( (Unique != LastUnique) || (Deliver != LastDeliver) )
                    {
                        LastUnique  = Unique;
                        LastDeliver = Deliver;
                        LastChange  = Clock::now();
                    }

                    if (Unique >= Expected)
                        break;
                }
            }

            if (SecondsSince (WaitStart) >= Cfg.WaitSec)
            {
                printf ("  wait time of %d s is over\n", Cfg.WaitSec);
                break;
            }

            if ( (SecondsSince (LastChange) >= Cfg.StallSec) && (SecondsSince (WaitStart) >= 2) )
            {
                printf ("  nothing new arrived for %d s: stop waiting\n", Cfg.StallSec);
                break;
            }

            SleepMs (1000);
        }
    }

    /* Never leave the sink in failing mode */
    bFailTimerStop = true;

    if (FailTimer.joinable())
        FailTimer.join();

    if (bFailSet)
        SinkRequest (Sink, Cfg, "POST", "/test/fail", "{\"status\":0}", Status, Reply);

    /* Final numbers from the sink */
    Stats.SetNull();

    if ( (false == SinkRequest (Sink, Cfg, "GET", "/test/stats", "", Status, Reply)) || (200 != Status) || (Stats.Parse (Reply.c_str()).HasParseError()) || (false == Stats.IsObject()) )
    {
        fprintf (stderr, "Cannot read /test/stats from the sink\n");
        return 2;
    }

    if (bProm)
        bProm = ReadFreshProm (Cfg.PromFile, PromAfter, time (NULL) + 1, 15);

    /* 4. Report */
    {
        uint64_t Unique     = GetU64 (Stats, "unique");
        uint64_t Missing    = GetU64 (Stats, "missing");
        uint64_t Deliveries = GetU64 (Stats, "deliveries");
        uint64_t Duplicates = GetU64 (Stats, "duplicates");
        uint64_t Unexpected = GetU64 (Stats, "unexpected");
        uint64_t Malformed  = GetU64 (Stats, "malformed");
        uint64_t Conflicts  = GetU64 (Stats, "timestamp_conflicts");
        int64_t  First      = GetI64 (Stats, "first_arrival_ns");
        int64_t  Last       = GetI64 (Stats, "last_arrival_ns");
        double   Forward    = (Last > First) ? (static_cast<double>(Unique) / (static_cast<double>(Last - First) / 1e9)) : 0;

        printf ("\n== generator ==\n");
        printf ("  requests answered            : %llu of %llu (%llu failed, %llu not answered with 404)\n", static_cast<unsigned long long>(Sum.Sent),
                static_cast<unsigned long long>(Expected), static_cast<unsigned long long>(Sum.Failed), static_cast<unsigned long long>(Sum.BadStatus));
        printf ("  send time / rate             : %.1f s, %.0f requests/s\n", SendSec, (SendSec > 0) ? (static_cast<double>(Sum.Sent) / SendSec) : 0.0);
        printf ("  HTTP answer time             : min %s  avg %s  max %s\n", Ms (Sum.RtMinNs).c_str(),
                Ms (Sum.Sent ? static_cast<int64_t>(RtSumAll / static_cast<double>(Sum.Sent)) : 0).c_str(), Ms (Sum.RtMaxNs).c_str());

        printf ("\n== sink ==\n");
        printf ("  expected / unique            : %llu / %llu\n", static_cast<unsigned long long>(GetU64 (Stats, "expected")), static_cast<unsigned long long>(Unique));
        printf ("  deliveries / duplicates      : %llu / %llu\n", static_cast<unsigned long long>(Deliveries), static_cast<unsigned long long>(Duplicates));
        printf ("  missing                      : %llu\n", static_cast<unsigned long long>(Missing));
        printf ("  unexpected / malformed       : %llu / %llu\n", static_cast<unsigned long long>(Unexpected), static_cast<unsigned long long>(Malformed));
        printf ("  mixed batches / conflicts    : %llu / %llu\n", static_cast<unsigned long long>(GetU64 (Stats, "mixed_batches")), static_cast<unsigned long long>(Conflicts));
        printf ("  other records (no test event): %llu\n", static_cast<unsigned long long>(GetU64 (Stats, "other_records")));
        printf ("  named as failed by NGINX     : %llu events, %llu of them missing\n", static_cast<unsigned long long>(GetU64 (Stats, "nginx_reported_lost")), static_cast<unsigned long long>(GetU64 (Stats, "missing_reported")));

        if (Stats.HasMember ("other_sample") && Stats["other_sample"].IsArray())
        {
            for (const auto& Line : Stats["other_sample"].GetArray())
            {
                if (Line.IsString())
                    printf ("      e.g. %s\n", Line.GetString());
            }
        }

        printf ("  end to end latency           : %s\n", LatencyLine (Stats["latency"]).c_str());

        if (Stats.HasMember ("stages") && Stats["stages"].IsObject())
        {
            printf ("    generator -> NGINX log     : %s\n", LatencyLine (Stats["stages"]["generator_to_nginx"]).c_str());
            printf ("    NGINX log -> sink          : %s\n", LatencyLine (Stats["stages"]["nginx_to_sink"]).c_str());
        }

        printf ("  forwarding rate              : %.0f events/s (first to last arrival)\n", Forward);
        printf ("  last event after last request: %.1f s\n", (Last > SendEndUnix) ? (static_cast<double>(Last - SendEndUnix) / 1e9) : 0.0);

        if (Duplicates > 0)
            printf ("  longest duplicate delay      : %s (last delivery after the first one)\n", Ms (GetI64 (Stats, "max_duplicate_delay_ns")).c_str());

        if (bProm)
        {
            double Accepted = Metric (PromAfter, "otelfwd_socket_lines_total{source=\"syslog\",result=\"accepted\"}") - Metric (PromBefore, "otelfwd_socket_lines_total{source=\"syslog\",result=\"accepted\"}");
            double Dropped  = Metric (PromAfter, "otelfwd_socket_lines_total{source=\"syslog\",result=\"dropped\"}") - Metric (PromBefore, "otelfwd_socket_lines_total{source=\"syslog\",result=\"dropped\"}");
            double Invalid  = Metric (PromAfter, "otelfwd_socket_lines_total{source=\"syslog\",result=\"invalid\"}") - Metric (PromBefore, "otelfwd_socket_lines_total{source=\"syslog\",result=\"invalid\"}");
            double PushOk   = Metric (PromAfter, "otelfwd_push_total{result=\"success\"}") - Metric (PromBefore, "otelfwd_push_total{result=\"success\"}");
            double PushErr  = Metric (PromAfter, "otelfwd_push_total{result=\"error\"}") - Metric (PromBefore, "otelfwd_push_total{result=\"error\"}");
            double RetryOk  = Metric (PromAfter, "otelfwd_push_retry_total{result=\"success\"}") - Metric (PromBefore, "otelfwd_push_retry_total{result=\"success\"}");
            double RetryErr = Metric (PromAfter, "otelfwd_push_retry_total{result=\"error\"}") - Metric (PromBefore, "otelfwd_push_retry_total{result=\"error\"}");

            /* Lines of the syslog socket which are not test events (NGINX error log lines) were accepted as well */
            double OtherAccepted = static_cast<double>(GetU64 (Stats, "other_records"));

            printf ("\n== otelfwd (change during the test) ==\n");
            printf ("  syslog lines accepted        : %.0f  (the generator got %llu answers; %.0f of the lines were not test events)\n", Accepted,
                    static_cast<unsigned long long>(Sum.Sent), OtherAccepted);
            printf ("  syslog lines dropped/invalid : %.0f / %.0f\n", Dropped, Invalid);
            printf ("  lines pushed ok / failed     : %.0f / %.0f\n", PushOk, PushErr);
            printf ("  WAL requests replayed ok/fail: %.0f / %.0f\n", RetryOk, RetryErr);

            /* The endpoints of otelfwd: there is a backup endpoint if otelfwd writes its endpoint metrics */
            auto   Delta      = [&] (const char *pszName) { return Metric (PromAfter, pszName) - Metric (PromBefore, pszName); };
            bool   bEndpoints = (PromAfter.Values.count ("otelfwd_push_endpoint_active") > 0);
            double Rejected   = Delta ("otelfwd_push_rejected_total");
            double Failovers  = Delta ("otelfwd_push_failovers_total");
            double Failbacks  = Delta ("otelfwd_push_failbacks_total");
            double PrimaryOk  = Delta ("otelfwd_push_endpoint_requests_total{endpoint=\"primary\",result=\"accepted\"}");
            double PrimaryRet = Delta ("otelfwd_push_endpoint_requests_total{endpoint=\"primary\",result=\"retry\"}");
            double PrimaryRej = Delta ("otelfwd_push_endpoint_requests_total{endpoint=\"primary\",result=\"rejected\"}");
            double BackupOk   = Delta ("otelfwd_push_endpoint_requests_total{endpoint=\"backup\",result=\"accepted\"}");
            double BackupRet  = Delta ("otelfwd_push_endpoint_requests_total{endpoint=\"backup\",result=\"retry\"}");
            double BackupRej  = Delta ("otelfwd_push_endpoint_requests_total{endpoint=\"backup\",result=\"rejected\"}");

            if (bEndpoints || (Rejected > 0))
            {
                printf ("\n== otelfwd endpoints (change during the test) ==\n");

                if (bEndpoints)
                {
                    printf ("  requests to the primary      : %.0f accepted / %.0f not accepted / %.0f refused as bad data (400)\n", PrimaryOk, PrimaryRet, PrimaryRej);
                    printf ("  requests to the backup       : %.0f accepted / %.0f not accepted / %.0f refused as bad data (400)\n", BackupOk, BackupRet, BackupRej);
                    printf ("  failovers / failbacks        : %.0f / %.0f\n", Failovers, Failbacks);
                    printf ("  endpoint in use at the end   : %s\n", (Metric (PromAfter, "otelfwd_push_endpoint_active") >= 1) ? "backup" : "primary");
                }

                printf ("  push requests refused (400)  : %.0f  (dropped, not kept in the WAL)\n", Rejected);
            }

            /* --expect-failover: the primary fails, so the events have to arrive through the backup */
            if (Cfg.bExpectFailover)
            {
                if (false == bEndpoints)
                {
                    Reasons.push_back ("otelfwd writes no endpoint metrics: it has no backup endpoint (--expect-failover)");
                }
                else
                {
                    if (Failovers < 1)
                        Reasons.push_back ("otelfwd made no failover to the backup endpoint (--expect-failover)");

                    if (BackupOk < 1)
                        Reasons.push_back ("the backup endpoint accepted no request (--expect-failover)");
                }
            }

            /* --expect-reject: the primary refuses the data with 400. It is dropped: not delivered, not kept, not sent to the backup */
            if (Cfg.bExpectReject)
            {
                if (Rejected < 1)
                    Reasons.push_back ("the primary endpoint refused nothing: otelfwd_push_rejected_total did not grow (--expect-reject)");

                if (PushOk > 0)
                    Reasons.push_back (std::to_string (static_cast<long long>(PushOk)) + " lines were pushed although the data is refused (--expect-reject)");

                if ( (PushErr > 0) || (RetryOk > 0) || (RetryErr > 0) )
                    Reasons.push_back ("refused data was kept for a retry in the WAL (--expect-reject)");

                if ( bEndpoints && ((Failovers > 0) || ((BackupOk + BackupRet + BackupRej) > 0)) )
                    Reasons.push_back ("refused data was sent to the backup endpoint (--expect-reject)");
            }

            if ( (Missing > 0) && (false == Cfg.bExpectReject) )
            {
                /* What otelfwd saw at its syslog socket for test events: accepted into its queue, dropped because the queue was
                   full, or invalid. Lines which are not test events do not count: they take the place of a lost event */
                double Seen        = (Accepted - OtherAccepted) + Dropped + Invalid;
                double LostBefore  = static_cast<double>(Sum.Sent) - Seen;
                bool   bAnyCause   = false;

                /* Every cause which applies is named with its own number: both can happen in one test. A few lines of difference are
                   noise (a line of NGINX which was itself dropped changes the sum) */
                if (LostBefore >= 1)
                {
                    uint64_t Reported = GetU64 (Stats, "missing_reported");

                    if ( (Missing > 0) && (Reported >= Missing) )
                        printf ("  HINT: PROVEN: all %llu missing events are named in a line of NGINX itself (\"send() to syslog failed while logging\n"
                                "        request ... request: GET /otelfwd-test/...\"). NGINX could not write them to the syslog socket, and they never\n"
                                "        reached otelfwd. That socket has no backpressure: at most net.unix.max_dgram_qlen (512) datagrams wait, and the\n"
                                "        send buffer of NGINX holds few lines. Try --rate, or read the README (Limits).\n", static_cast<unsigned long long>(Reported));
                    else
                        printf ("  HINT: about %.0f test events were lost BEFORE otelfwd, between NGINX and its syslog socket (%llu of the %llu missing are named\n"
                                "        in a \"send() to syslog failed\" line of NGINX). Syslog over a datagram socket has no backpressure: at most\n"
                                "        net.unix.max_dgram_qlen (512) datagrams wait, and NGINX drops the line when that is full. Try --rate.\n",
                                LostBefore, static_cast<unsigned long long>(Reported), static_cast<unsigned long long>(Missing));

                    bAnyCause = true;
                }

                if (Dropped > 0)
                {
                    printf ("  HINT: otelfwd dropped %.0f lines because its queue was full (OTELFWD_SOCKET_QUEUE_MAX). Its push was too slow.\n", Dropped);
                    bAnyCause = true;
                }

                if (Invalid > 0)
                {
                    printf ("  HINT: otelfwd could not decode %.0f syslog lines (invalid).\n", Invalid);
                    bAnyCause = true;
                }

                if (false == bAnyCause)
                    printf ("  HINT: otelfwd accepted everything. The events are lost after it: check the push errors above, its WAL, or the sink.\n");
            }
        }
        else if (Cfg.bExpectFailover || Cfg.bExpectReject)
        {
            Reasons.push_back ("the metrics file of otelfwd could not be read: the expectation of --expect-failover or --expect-reject cannot be checked");
        }

        if (Stats.HasMember ("missing_sample") && Stats["missing_sample"].IsArray() && (Stats["missing_sample"].Size() > 0))
        {
            printf ("\n  first missing events (thread/count):");

            for (const auto& M : Stats["missing_sample"].GetArray())
                printf (" %llu/%llu", static_cast<unsigned long long>(GetU64 (M, "thread")), static_cast<unsigned long long>(GetU64 (M, "count")));

            printf ("\n");
        }

        if (Stats.HasMember ("threads") && Stats["threads"].IsArray())
        {
            bool bHeader = false;

            for (const auto& T : Stats["threads"].GetArray())
            {
                if ( (false == Cfg.bVerbose) && (0 == GetU64 (T, "missing")) && (0 == GetU64 (T, "duplicates")) )
                    continue;

                if (false == bHeader)
                {
                    printf ("\n== threads%s ==\n", Cfg.bVerbose ? "" : " with missing events or duplicates");
                    printf ("  thread  expected    unique deliveries duplicates   missing\n");
                    bHeader = true;
                }

                printf ("  %6llu %9llu %9llu %10llu %10llu %9llu\n", static_cast<unsigned long long>(GetU64 (T, "thread")), static_cast<unsigned long long>(GetU64 (T, "expected")),
                        static_cast<unsigned long long>(GetU64 (T, "unique")), static_cast<unsigned long long>(GetU64 (T, "deliveries")),
                        static_cast<unsigned long long>(GetU64 (T, "duplicates")), static_cast<unsigned long long>(GetU64 (T, "missing")));
            }
        }

        if (g_Abort.load())                Reasons.push_back ("the test was aborted");
        if ( (Missing > 0) && (false == Cfg.bExpectReject) )
            Reasons.push_back (std::to_string (Missing) + " events missing");

        if ( Cfg.bExpectReject && (Unique > 0) )
            Reasons.push_back (std::to_string (Unique) + " events arrived at the sink although the primary endpoint refuses them: refused data must not be sent to the backup (--expect-reject)");

        if (Unexpected > 0)                Reasons.push_back (std::to_string (Unexpected) + " unexpected events");
        if (Malformed > 0)                 Reasons.push_back (std::to_string (Malformed) + " malformed test paths");
        if (Conflicts > 0)                 Reasons.push_back (std::to_string (Conflicts) + " events with conflicting time stamps (records of an earlier run in the WAL?)");
        if (Sum.Failed > 0)                Reasons.push_back (std::to_string (Sum.Failed) + " requests without an answer");
        if (Sum.BadStatus > 0)             Reasons.push_back (std::to_string (Sum.BadStatus) + " requests not answered with 404 (is the /otelfwd-test/ location in the NGINX configuration?)");
    }

    printf ("\n");

    if (Reasons.empty())
    {
        if (Cfg.bExpectReject)
        {
            printf ("RESULT: PASS  (the primary endpoint refused the data with 400: it was dropped, not kept for a retry and not sent to the backup)\n");
            return 0;
        }

        printf ("RESULT: PASS  (every event arrived%s%s)\n", GetU64 (Stats, "duplicates") ? ", some more than once: at least once delivery" : " exactly once",
                Cfg.bExpectFailover ? ", through the backup endpoint after a failover" : "");
        return 0;
    }

    printf ("RESULT: FAIL  (");

    for (size_t i = 0; i < Reasons.size(); i++)
        printf ("%s%s", (i ? "; " : ""), Reasons[i].c_str());

    printf (")\n");
    return 1;
}
