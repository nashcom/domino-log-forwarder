/*
   otel-sink - the receiving end of the OTLP test container.

   NGINX terminates HTTP/HTTPS, checks the bearer token and answers with the configured error codes.
   It forwards every accepted request over a UNIX socket to this program (proxy_pass). This program
   reads that request and writes the body of every POST to its own file:

       000001_20260918T201530.123Z_v1-logs.json

   * A JSON body is pretty printed. --raw keeps the bytes as received.
   * A body which is not JSON (for example protobuf) is written with the extension .bin.
   * A gzip compressed body (Content-Encoding: gzip) is decompressed first.
   * The request is answered with 200 and an empty OTLP response ({}).

   NGINX always sends HTTP/1.0 with a Content-Length and closes the connection after the response
   (proxy_request_buffering is on), so the HTTP handling here is intentionally minimal.
   The sink only receives and stores data. It has no authentication: only NGINX can reach the socket.

   Build:   make otel-sink                (rapidjson and zlib headers needed)
   Run:     otel-sink --socket /tmp/otel-sink/sink.sock --dir /data/received
*/

#include <atomic>
#include <cerrno>
#include <chrono>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <map>
#include <string>
#include <thread>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/un.h>
#include <unistd.h>
#include <zlib.h>

#include <rapidjson/document.h>
#include <rapidjson/encodedstream.h>
#include <rapidjson/memorystream.h>
#include <rapidjson/prettywriter.h>
#include <rapidjson/stringbuffer.h>

#include "test_ledger.hpp"

#define SINK_MAX_HEADER_BYTES  (64 * 1024)
#define SINK_MAX_BODY_BYTES    (64 * 1024 * 1024)
#define SINK_MAX_INFLATED_BYTES (256 * 1024 * 1024)
#define SINK_READ_TIMEOUT_SEC  10

struct SinkConfig
{
    std::string Socket = "/tmp/otel-sink/sink.sock";
    std::string Dir    = "otel-received";
    bool        bRaw   = false;
};

/* Shared between threads: atomic. g_Stop is set by the signal handler, which can run in any thread,
   so it has to be lock free as well. */
static_assert (std::atomic<int>::is_always_lock_free, "std::atomic<int> has to be lock free to be used in a signal handler");

SinkConfig            g_Config;                 /* set at startup, read only afterwards */
std::atomic<unsigned> g_Seq {0};                /* last file number, incremented by the connection threads */
std::atomic<int>      g_Active {0};             /* number of running connection threads */
std::atomic<int>      g_Stop {0};               /* set by SIGINT/SIGTERM */
sinktest::Ledger      g_Test;                   /* load test ledger, has its own mutex (see test_ledger.hpp) */


static void OnSignal (int)
{
    g_Stop = 1;
}


static void PrintLine (const std::string& Text)
{
    fprintf (stdout, "%s\n", Text.c_str());
    fflush (stdout);
}


static std::string ToLower (std::string Text)
{
    for (char& c : Text)
        c = static_cast<char>(tolower (static_cast<unsigned char>(c)));

    return Text;
}


static bool SendAll (int Fd, const std::string& Data)
{
    size_t Sent = 0;

    while (Sent < Data.size())
    {
        ssize_t n = send (Fd, Data.data() + Sent, Data.size() - Sent, MSG_NOSIGNAL);

        if (n <= 0)
            return false;

        Sent += static_cast<size_t>(n);
    }

    return true;
}


static const char *ReasonPhrase (int Status)
{
    switch (Status)
    {
        case 200: return "OK";
        case 400: return "Bad Request";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 411: return "Length Required";
        case 413: return "Payload Too Large";
        case 503: return "Service Unavailable";
        default:  return "Internal Server Error";
    }
}


static void Reply (int Fd, int Status, const std::string& Body = "{}")
{
    std::string Response = "HTTP/1.0 " + std::to_string (Status) + " " + ReasonPhrase (Status) + "\r\n" +
                           "Content-Type: application/json\r\n" +
                           "Content-Length: " + std::to_string (Body.size()) + "\r\n" +
                           "Connection: close\r\n\r\n" + Body;

    SendAll (Fd, Response);
}


/* 000001_20260918T201530.123Z_v1-logs.json - the path of the request is part of the name, reduced to letters and digits */
static std::string BuildFileName (unsigned Seq, const std::string& Target, const char *pszExtension)
{
    std::string Path = Target.substr (0, Target.find ('?'));
    std::string Part;
    char szTime[32] = {0};
    char szName[64] = {0};
    struct tm Tm {};

    auto Now = std::chrono::system_clock::now();
    time_t T = std::chrono::system_clock::to_time_t (Now);
    long Millis = static_cast<long>(std::chrono::duration_cast<std::chrono::milliseconds>(Now.time_since_epoch()).count() % 1000);

    gmtime_r (&T, &Tm);
    strftime (szTime, sizeof (szTime), "%Y%m%dT%H%M%S", &Tm);
    snprintf (szName, sizeof (szName), "%06u_%s.%03ldZ_", Seq, szTime, Millis);

    for (char c : Path)
    {
        if (isalnum (static_cast<unsigned char>(c)))
            Part += c;
        else if ( (false == Part.empty()) && ('-' != Part.back()) )
            Part += '-';
    }

    while ( (false == Part.empty()) && ('-' == Part.back()) )
        Part.pop_back();

    if (Part.empty())
        Part = "root";

    if (Part.size() > 60)
        Part.resize (60);

    return std::string (szName) + Part + "." + pszExtension;
}


static bool ParseJson (const std::string& Body, rapidjson::Document& retDoc)
{
    rapidjson::MemoryStream Ms (Body.data(), Body.size());
    rapidjson::EncodedInputStream<rapidjson::UTF8<>, rapidjson::MemoryStream> Is (Ms);

    retDoc.ParseStream<rapidjson::kParseValidateEncodingFlag> (Is);

    return (false == retDoc.HasParseError());
}


/* Short description of an OTLP JSON document: number of log records, spans, metrics */
static std::string Summarize (const rapidjson::Document& Doc)
{
    struct Signal { const char *pszResource; const char *pszScope; const char *pszItems; };
    static const Signal Signals[] = { {"resourceLogs", "scopeLogs", "logRecords"},
                                      {"resourceSpans", "scopeSpans", "spans"},
                                      {"resourceMetrics", "scopeMetrics", "metrics"} };
    std::string Result;

    if (false == Doc.IsObject())
        return "no OTLP content";

    for (const Signal& S : Signals)
    {
        size_t Count = 0;

        if ( (false == Doc.HasMember (S.pszResource)) || (false == Doc[S.pszResource].IsArray()) )
            continue;

        for (const auto& Resource : Doc[S.pszResource].GetArray())
        {
            if ( (false == Resource.IsObject()) || (false == Resource.HasMember (S.pszScope)) || (false == Resource[S.pszScope].IsArray()) )
                continue;

            for (const auto& Scope : Resource[S.pszScope].GetArray())
            {
                if (Scope.IsObject() && Scope.HasMember (S.pszItems) && Scope[S.pszItems].IsArray())
                    Count += Scope[S.pszItems].Size();
            }
        }

        if (false == Result.empty())
            Result += ", ";

        Result += std::to_string (Count) + " " + S.pszItems;
    }

    return Result.empty() ? "no OTLP content" : Result;
}


/* Decompresses a gzip stream. Limits the result to protect against a compression bomb */
static bool Gunzip (const std::string& In, std::string& retOut)
{
    z_stream Zs {};
    char Buffer[65536];
    int rc = Z_OK;

    if (Z_OK != inflateInit2 (&Zs, 15 + 16))
        return false;

    Zs.next_in  = reinterpret_cast<Bytef *>(const_cast<char *>(In.data()));
    Zs.avail_in = static_cast<uInt>(In.size());

    do
    {
        Zs.next_out  = reinterpret_cast<Bytef *>(Buffer);
        Zs.avail_out = sizeof (Buffer);

        rc = inflate (&Zs, Z_NO_FLUSH);

        if ( (Z_OK != rc) && (Z_STREAM_END != rc) )
        {
            inflateEnd (&Zs);
            return false;
        }

        retOut.append (Buffer, sizeof (Buffer) - Zs.avail_out);

        if (retOut.size() > SINK_MAX_INFLATED_BYTES)
        {
            inflateEnd (&Zs);
            return false;
        }

    } while (Z_STREAM_END != rc);

    inflateEnd (&Zs);
    return true;
}


/* Writes to a temporary file and renames it, so a reader never sees a partial file */
static bool WriteFileAtomic (const std::filesystem::path& Target, const std::string& Data)
{
    std::filesystem::path Temp = Target;
    std::error_code ec;
    FILE *fp = NULL;
    bool bOk = false;

    Temp += ".tmp";

    fp = fopen (Temp.c_str(), "wb");

    if (NULL == fp)
        return false;

    bOk = (Data.size() == fwrite (Data.data(), 1, Data.size(), fp));
    bOk = (0 == fclose (fp)) && bOk;

    if (false == bOk)
    {
        std::filesystem::remove (Temp, ec);
        return false;
    }

    std::filesystem::rename (Temp, Target, ec);

    return (!ec);
}


/* Reads the request header. Returns false on a malformed or oversized header */
static bool ReadHeader (int Fd, std::string& retRaw, size_t& retHeaderEnd)
{
    char Buffer[8192];

    while (std::string::npos == (retHeaderEnd = retRaw.find ("\r\n\r\n")))
    {
        ssize_t n = 0;

        if (retRaw.size() > SINK_MAX_HEADER_BYTES)
            return false;

        n = recv (Fd, Buffer, sizeof (Buffer), 0);

        if (n <= 0)
            return false;

        retRaw.append (Buffer, static_cast<size_t>(n));
    }

    return true;
}


/* POST /test/reset and POST /test/fail. The load test ledger is in test_ledger.hpp */
static void HandleTestCommand (int Fd, const std::string& Path, const std::string& Body)
{
    rapidjson::Document Doc;
    std::string Error;

    if ("/test/reset" == Path)
    {
        if ( (false == ParseJson (Body, Doc)) || (false == Doc.IsObject()) ||
             (false == Doc.HasMember ("threads")) || (false == Doc["threads"].IsUint64()) ||
             (false == Doc.HasMember ("events_per_thread")) || (false == Doc["events_per_thread"].IsUint64()) )
        {
            Reply (Fd, 400, "{\"error\":\"body must be {\\\"threads\\\": n, \\\"events_per_thread\\\": n}\"}");
            return;
        }

        uint64_t Threads   = Doc["threads"].GetUint64();
        uint64_t PerThread = Doc["events_per_thread"].GetUint64();

        if (false == g_Test.Reset (Threads, PerThread, Error))
        {
            Reply (Fd, 400, "{\"error\":\"" + Error + "\"}");
            return;
        }

        PrintLine ("test reset: " + std::to_string (Threads) + " threads x " + std::to_string (PerThread) + " events");
        Reply (Fd, 200, "{\"status\":\"reset\",\"expected\":" + std::to_string (Threads * PerThread) + "}");
        return;
    }

    if ("/test/fail" == Path)
    {
        if ( (false == ParseJson (Body, Doc)) || (false == Doc.IsObject()) || (false == Doc.HasMember ("status")) || (false == Doc["status"].IsInt()) ||
             ( (0 != Doc["status"].GetInt()) && ((Doc["status"].GetInt() < 400) || (Doc["status"].GetInt() > 599)) ) )
        {
            Reply (Fd, 400, "{\"error\":\"body must be {\\\"status\\\": 0 or 400 to 599}\"}");
            return;
        }

        g_Test.SetFailStatus (Doc["status"].GetInt());
        PrintLine ("test fail status: " + std::to_string (Doc["status"].GetInt()));
        Reply (Fd, 200, "{\"fail_status\":" + std::to_string (Doc["status"].GetInt()) + "}");
        return;
    }

    Reply (Fd, 404, "{\"error\":\"unknown test command\"}");
}


static void HandleConnection (int Fd)
{
    std::string Raw;
    std::string Method;
    std::string Target;
    std::string Body;
    std::string Note;
    std::string Name;
    std::string Data;
    std::map<std::string, std::string> Headers;
    rapidjson::Document Doc;
    size_t HeaderEnd = 0;
    size_t Length    = 0;
    bool bJson       = false;
    unsigned Seq     = 0;
    struct timeval Tv {};

    Tv.tv_sec = SINK_READ_TIMEOUT_SEC;
    setsockopt (Fd, SOL_SOCKET, SO_RCVTIMEO, &Tv, sizeof (Tv));

    if (false == ReadHeader (Fd, Raw, HeaderEnd))
    {
        Reply (Fd, 400, "{\"error\":\"bad request\"}");
        return;
    }

    /* Request line and headers */
    {
        size_t LineEnd = Raw.find ("\r\n");
        std::string Line = Raw.substr (0, LineEnd);
        size_t Sp1 = Line.find (' ');
        size_t Sp2 = (std::string::npos == Sp1) ? std::string::npos : Line.find (' ', Sp1 + 1);
        size_t Pos = LineEnd + 2;

        if ( (std::string::npos == Sp1) || (std::string::npos == Sp2) )
        {
            Reply (Fd, 400, "{\"error\":\"bad request line\"}");
            return;
        }

        Method = Line.substr (0, Sp1);
        Target = Line.substr (Sp1 + 1, Sp2 - Sp1 - 1);

        while (Pos < HeaderEnd)
        {
            size_t End   = Raw.find ("\r\n", Pos);
            size_t Colon = Raw.find (':', Pos);

            if ( (std::string::npos != Colon) && (Colon < End) )
            {
                std::string Value = Raw.substr (Colon + 1, End - Colon - 1);
                size_t First = Value.find_first_not_of (" \t");

                Headers[ToLower (Raw.substr (Pos, Colon - Pos))] = (std::string::npos == First) ? "" : Value.substr (First);
            }

            Pos = End + 2;
        }
    }

    if ("GET" == Method)
    {
        if ("/test/stats" == Target.substr (0, Target.find ('?')))
            Reply (Fd, 200, g_Test.StatsJson());
        else
            Reply (Fd, 200, "{\"status\":\"otel-sink\"}");

        return;
    }

    if ("POST" != Method)
    {
        Reply (Fd, 405, "{\"error\":\"method not allowed\"}");
        return;
    }

    if (Headers.count ("transfer-encoding") || (0 == Headers.count ("content-length")))
    {
        Reply (Fd, 411, "{\"error\":\"content-length required\"}");
        return;
    }

    Length = static_cast<size_t>(strtoull (Headers["content-length"].c_str(), NULL, 10));

    if (Length > SINK_MAX_BODY_BYTES)
    {
        Reply (Fd, 413, "{\"error\":\"request too large\"}");
        return;
    }

    /* Body: the part which arrived with the header, then the rest */
    Body = Raw.substr (HeaderEnd + 4);

    while (Body.size() < Length)
    {
        char Buffer[65536];
        ssize_t n = recv (Fd, Buffer, sizeof (Buffer), 0);

        if (n <= 0)
        {
            Reply (Fd, 400, "{\"error\":\"incomplete body\"}");
            return;
        }

        Body.append (Buffer, static_cast<size_t>(n));
    }

    Body.resize (Length);

    /* Load test control commands, not OTLP data */
    if (0 == Target.compare (0, 6, "/test/"))
    {
        HandleTestCommand (Fd, Target.substr (0, Target.find ('?')), Body);
        return;
    }

    /* Failure switch of the load test: answer like a failing receiver, so otelfwd has to use its WAL */
    if (g_Test.FailStatus() > 0)
    {
        g_Test.CountFailedRequest();
        Reply (Fd, g_Test.FailStatus(), "{\"error\":\"simulated failure (test)\"}");
        return;
    }

    {
        std::string Encoding = ToLower (Headers["content-encoding"]);

        if ("gzip" == Encoding)
        {
            std::string Inflated;

            if (false == Gunzip (Body, Inflated))
            {
                Reply (Fd, 400, "{\"error\":\"invalid gzip body\"}");
                return;
            }

            Body = Inflated;
            Note = ", was gzip";
        }
        else if ( (false == Encoding.empty()) && ("identity" != Encoding) )
        {
            Note = ", " + Encoding + " not decoded";
        }
    }

    bJson = Note.empty() || (0 == Note.compare (0, 10, ", was gzip"));
    bJson = bJson && ParseJson (Body, Doc);

    /* Records of the load test (url.path /otelfwd-test/...) go to the ledger in memory. Such a request is not written to a file
       and not printed for every request, so disk and console do not slow the test down. */
    if (bJson)
    {
        std::string Summary;
        sinktest::Ledger::Result Res = g_Test.ProcessOtlp (Doc, sinktest::NowNs(), Summary);

        if (Res.TestRecords > 0)
        {
            if (false == Summary.empty())
                PrintLine (Summary);

            Reply (Fd, 200);
            return;
        }
    }

    if (bJson && (false == g_Config.bRaw))
    {
        rapidjson::StringBuffer Buffer;
        rapidjson::PrettyWriter<rapidjson::StringBuffer> Writer (Buffer);

        Writer.SetIndent (' ', 2);
        Doc.Accept (Writer);

        Data.assign (Buffer.GetString(), Buffer.GetSize());
        Data += '\n';
    }
    else
    {
        Data = Body;
    }

    Seq  = ++g_Seq;
    Name = BuildFileName (Seq, Target, bJson ? "json" : "bin");

    if (false == WriteFileAtomic (std::filesystem::path (g_Config.Dir) / Name, Data))
    {
        PrintLine ("       cannot write " + Name);
        Reply (Fd, 500, "{\"error\":\"cannot write file\"}");
        return;
    }

    {
        std::string Auth = (0 == Headers["authorization"].compare (0, 7, "Bearer ")) ? "Bearer set" : "none";
        char szSeq[16] = {0};

        snprintf (szSeq, sizeof (szSeq), "%06u", Seq);

        PrintLine (std::string ("#") + szSeq + " " + Method + " " + Target + "  " + (bJson ? Summarize (Doc) : std::string ("not JSON")) + ", " +
                   std::to_string (Body.size()) + " bytes" + Note + "  (auth: " + Auth + ")  -> " + Name);
    }

    Reply (Fd, 200);
}


static void ConnectionThread (int Fd)
{
    HandleConnection (Fd);
    close (Fd);
    g_Active--;
}


/* Highest sequence number of the files already in the directory, so a restart does not reuse numbers */
static unsigned LastSequence (const std::string& Dir)
{
    unsigned Last = 0;
    std::error_code ec;

    for (const auto& Entry : std::filesystem::directory_iterator (Dir, ec))
    {
        std::string Name = Entry.path().filename().string();

        if ( (Name.size() > 7) && ('_' == Name[6]) && isdigit (static_cast<unsigned char>(Name[0])) )
        {
            unsigned Number = static_cast<unsigned>(strtoul (Name.substr (0, 6).c_str(), NULL, 10));

            if (Number > Last)
                Last = Number;
        }
    }

    return Last;
}


/* Creates the listening UNIX socket. A stale socket file of an earlier run is replaced. */
static int CreateListener (const std::string& Path)
{
    struct sockaddr_un Addr {};
    struct stat St {};
    int Fd = -1;
    int rc = 0;

    if (Path.size() >= sizeof (Addr.sun_path))
    {
        fprintf (stderr, "otel-sink: socket path is too long\n");
        return -1;
    }

    Addr.sun_family = AF_UNIX;
    snprintf (Addr.sun_path, sizeof (Addr.sun_path), "%s", Path.c_str());

    if (0 == lstat (Path.c_str(), &St))
    {
        int Probe = -1;

        if (false == S_ISSOCK (St.st_mode))
        {
            fprintf (stderr, "otel-sink: %s exists and is not a socket\n", Path.c_str());
            return -1;
        }

        Probe = socket (AF_UNIX, SOCK_STREAM, 0);

        if ( (Probe >= 0) && (0 == connect (Probe, reinterpret_cast<struct sockaddr *>(&Addr), sizeof (Addr))) )
        {
            close (Probe);
            fprintf (stderr, "otel-sink: %s is in use by another process\n", Path.c_str());
            return -1;
        }

        if (Probe >= 0)
            close (Probe);

        unlink (Path.c_str());
    }

    Fd = socket (AF_UNIX, SOCK_STREAM, 0);

    if (Fd < 0)
        return -1;

    umask (0117);       /* owner and group only */
    rc = bind (Fd, reinterpret_cast<struct sockaddr *>(&Addr), sizeof (Addr));

    if ( (0 != rc) || (0 != listen (Fd, 64)) )
    {
        fprintf (stderr, "otel-sink: cannot listen on %s: %s\n", Path.c_str(), strerror (errno));
        close (Fd);
        return -1;
    }

    return Fd;
}


static void PrintUsage()
{
    fprintf (stderr,
             "otel-sink - receiving end of the OTLP test container. Writes every POST to its own file.\n"
             "\n"
             "  --socket PATH    UNIX socket NGINX forwards to (default: /tmp/otel-sink/sink.sock)\n"
             "  --dir DIRECTORY  output directory (default: ./otel-received)\n"
             "  --raw            write the body as received, do not pretty print JSON\n"
             "  --help           this text\n");
}


int main (int argc, char *argv[])
{
    std::error_code ec;
    int Listener = -1;

    for (int i = 1; i < argc; i++)
    {
        std::string Arg = argv[i];

        if ( ("--help" == Arg) || ("-h" == Arg) )
        {
            PrintUsage();
            return 0;
        }
        else if ("--raw" == Arg)
        {
            g_Config.bRaw = true;
        }
        else if ( (("--socket" == Arg) || ("--dir" == Arg)) && ((i + 1) < argc) )
        {
            (("--socket" == Arg) ? g_Config.Socket : g_Config.Dir) = argv[++i];
        }
        else
        {
            fprintf (stderr, "otel-sink: unknown or incomplete parameter %s\n\n", argv[i]);
            PrintUsage();
            return 2;
        }
    }

    std::filesystem::create_directories (g_Config.Dir, ec);

    if (ec)
    {
        fprintf (stderr, "otel-sink: cannot create %s: %s\n", g_Config.Dir.c_str(), ec.message().c_str());
        return 1;
    }

    std::filesystem::create_directories (std::filesystem::path (g_Config.Socket).parent_path(), ec);

    Listener = CreateListener (g_Config.Socket);

    if (Listener < 0)
        return 1;

    g_Seq = LastSequence (g_Config.Dir);

    signal (SIGINT,  OnSignal);
    signal (SIGTERM, OnSignal);
    signal (SIGPIPE, SIG_IGN);

    PrintLine ("otel-sink: " + g_Config.Socket + "  ->  " + std::filesystem::absolute (g_Config.Dir).string() +
               "  (next file number " + std::to_string (g_Seq + 1) + ")");

    while (0 == g_Stop)
    {
        struct pollfd Pfd {};
        int Fd = -1;

        Pfd.fd     = Listener;
        Pfd.events = POLLIN;

        if (poll (&Pfd, 1, 500) <= 0)
            continue;

        Fd = accept (Listener, NULL, NULL);

        if (Fd < 0)
            continue;

        g_Active++;
        std::thread (ConnectionThread, Fd).detach();
    }

    close (Listener);
    unlink (g_Config.Socket.c_str());

    for (int Wait = 0; (g_Active > 0) && (Wait < 40); Wait++)
        usleep (50000);

    PrintLine ("otel-sink: stopped");

    return 0;
}
