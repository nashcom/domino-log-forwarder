/*
###########################################################################
# otelfwd - Log Forwarder for OpenTelemetry                               #
# Version 2.0.0 18.09.2026                                                #
# (C) Copyright Daniel Nashed/Nash!Com 2026                               #
#                                                                         #
# Licensed under the Apache License, Version 2.0 (the "License");         #
# you may not use this file except in compliance with the License.        #
# You may obtain a copy of the License at                                 #
#                                                                         #
#      http://www.apache.org/licenses/LICENSE-2.0                         #
#                                                                         #
# Unless required by applicable law or agreed to in writing, software     #
# distributed under the License is distributed on an "AS IS" BASIS,       #
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.#
# See the License for the specific language governing permissions and     #
# limitations under the License.                                          #
#                                                                         #
#                                                                         #
###########################################################################
*/

#define OTELFWD_COPYRIGHT  "Copyright Daniel Nashed/Nash!Com 2026"
#define OTELFWD_GITHUB_URL "https://github.com/nashcom/domino-grafana"

#define OTELFWD_VERSION_MAJOR 2
#define OTELFWD_VERSION_MINOR 0
#define OTELFWD_VERSION_PATCH 0

#define OTELFWD_DEFAULT_SHUTDOWN_MAX_WAIT_SEC 30

/* How often the primary OTLP endpoint is tried again while the backup endpoint is in use */
#define OTELFWD_DEFAULT_FAILBACK_SEC          60
#define OTELFWD_MAX_FAILBACK_SEC              86400

/* How long the WAL thread waits after a failed replay before it sends the WAL again */
#define OTELFWD_DEFAULT_WAL_RETRY_SEC         60
#define OTELFWD_MAX_WAL_RETRY_SEC             86400

/* The largest size of the WAL in MB. Pushes which do not fit are dropped. 0 is no limit */
#define OTELFWD_DEFAULT_WAL_MAX_MB            128
#define OTELFWD_MAX_WAL_MAX_MB                1048576

/* A receiver which does not accept the connection in this time is treated as not reachable */
#define OTELFWD_PUSH_CONNECT_TIMEOUT_SEC      3

/* Time for a whole push request. The try of the primary while the backup is in use (a probe) gets less: a primary which does
   not answer must not hold up the request for the whole time */
#define OTELFWD_PUSH_TIMEOUT_SEC              15
#define OTELFWD_PUSH_PROBE_TIMEOUT_SEC        5

/* How much of the answer of the receiver is kept for the log */
#define OTELFWD_PUSH_RESPONSE_MAX             512

/* The format of a push request. The WAL always holds JSON. Protobuf is made from it just before a request is sent */
#define OTELFWD_CONTENT_TYPE_JSON             "application/json"
#define OTELFWD_CONTENT_TYPE_PROTOBUF         "application/x-protobuf"

#define OTELFWD_VERSION_BUILD (OTELFWD_VERSION_MAJOR * 10000 +  OTELFWD_VERSION_MINOR * 100 + OTELFWD_VERSION_PATCH)


#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>
#include <mutex>
#include <queue>
#include <sstream>
#include <string>
#include <system_error>
#include <unordered_map>
#include <vector>


#include <stdint.h>
#include <strings.h>
#include <arpa/inet.h>
#include <curl/curl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/un.h>
#include <sys/utsname.h>
#include <time.h>
#include <unistd.h>

/* Override assertions to ensure we don't terminate for logical errors */
#define RAPIDJSON_ASSERT

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "simple_wal.hpp"
#include "push_status.hpp"
#include "push_failover.hpp"
#include "otlp_protobuf.hpp"
#include "log_line.hpp"
#include "health.hpp"
#include "file_input.hpp"
#include "file_reader.hpp"

/* pid.nbf map definition */
using PidMap = std::unordered_map<pid_t, std::string>;

/* Limits for the number of log lines sent to the OTLP endpoint in one push request.
   The push thread never waits to fill a batch: it takes whatever is queued up to these limits. */
#define OTELFWD_MAX_BATCH_RECORDS 100
#define OTELFWD_MAX_BATCH_BYTES   (512 * 1024)

/* The file input: how often it looks for new lines when there are none, and how many lines it reads before it looks at what the
   push thread delivered */
#define OTELFWD_FILE_POLL_MS             200
#define OTELFWD_FILE_LINES_PER_ROUND     1000

/* Limits for the socket inputs */
#define OTELFWD_MAX_LINE_BYTES           (1024 * 1024)
#define OTELFWD_MAX_CLIENTS_PER_INPUT    32
#define OTELFWD_DEFAULT_SOCKET_QUEUE_MAX 100000

/* Resource and attribute values (OTLP AnyValue subset) */
enum OtelValueType
{
    OTEL_TYPE_STRING = 0,
    OTEL_TYPE_INT,
    OTEL_TYPE_DOUBLE,
    OTEL_TYPE_BOOL
};

struct OtelKV
{
    std::string   Key;
    OtelValueType Type      = OTEL_TYPE_STRING;
    std::string   StrValue;
    int64_t       IntValue  = 0;
    double        DblValue  = 0;
    bool          BoolValue = false;
};

enum LogSource
{
    LOG_SOURCE_STDIN = 0,
    LOG_SOURCE_SOCKET,
    LOG_SOURCE_FILE
};

/* One log line moving through the forwarder.

   stdin lines: the ingestion thread sets TimeNs (observed time) and Line.
                The push thread sets Pid, Process and Attributes (annotation via pid.nbf).

   Records from socket inputs are decoded from the flat record format and use all fields.
   Empty/zero fields select the forwarder's defaults in the OTLP payload.

   Records of the file input have the line as the body and the attribute log.file.path. FileGeneration and FileEndOffset say where in
   the file the line ends: when the push thread is done with the batch, the position is committed (see FileCommitSlot). */
struct LogRecord
{
    LogSource   Source         = LOG_SOURCE_STDIN;
    int64_t     TimeNs         = 0;     /* event time. 0: use ObservedTimeNs */
    int64_t     ObservedTimeNs = 0;     /* observed time. 0: use TimeNs */
    std::string Line;                   /* log line / body */
    pid_t       Pid            = 0;
    std::string Process;
    int         SeverityNumber = 0;     /* 0: unspecified */
    std::string SeverityText;
    std::string ScopeName;              /* empty: forwarder default scope */
    std::string ScopeVersion;
    std::vector<OtelKV> Resource;       /* empty: forwarder default resource */
    std::vector<OtelKV> Attributes;
    uint64_t    FileGeneration = 0;     /* file input only: 0 for every other record */
    uint64_t    FileEndOffset  = 0;
};

/* FIFO class used to hand log records from the ingestion threads to the push thread */
class log_fifo
{

public:

    void push (LogRecord &&Record)
    {
        {
            std::lock_guard<std::mutex> lock (m_mutex);
            m_queue.push (std::move (Record));
        }
        m_cond.notify_one();
    }

    /* Push with a size limit. Returns false and drops the record if the queue is full.
       Used by socket inputs, where dropping is acceptable. stdin must never use it. */
    bool try_push (LogRecord &&Record, size_t MaxSize)
    {
        {
            std::lock_guard<std::mutex> lock (m_mutex);

            if (m_queue.size() >= MaxSize)
                return false;

            m_queue.push (std::move (Record));
        }
        m_cond.notify_one();

        return true;
    }

    /* Blocks until at least one record is queued (or shutdown), then returns everything
       queued up to the given limits without waiting for more.
       Returns false only on shutdown with an empty queue. */
    bool pop_batch (std::vector<LogRecord> &out, size_t MaxRecords, size_t MaxBytes)
    {
        std::unique_lock<std::mutex> lock (m_mutex);
        size_t Bytes = 0;

        out.clear();

        m_cond.wait (lock, [&]
        {
            return !m_queue.empty() || m_bShutdown;
        });

        if (m_queue.empty())
        {
            return false; // shutdown
        }

        while ( (!m_queue.empty()) && (out.size() < MaxRecords) )
        {
            Bytes += m_queue.front().Line.size();

            /* Always take at least one record, even if it is larger than the limit */
            if ( (!out.empty()) && (Bytes > MaxBytes) )
                break;

            out.push_back (std::move (m_queue.front()));
            m_queue.pop();
        }

        return true;
    }

    /* The number of queued records. The file input reads only while the queue has room */
    size_t size()
    {
        std::lock_guard<std::mutex> lock (m_mutex);
        return m_queue.size();
    }

    void shutdown()
    {
        {
            std::lock_guard<std::mutex> lock (m_mutex);
            m_bShutdown = true;
        }
        m_cond.notify_all();
    }

private:

    std::queue<LogRecord> m_queue;
    std::mutex m_mutex;
    std::condition_variable m_cond;
    bool m_bShutdown = false;
};


/* One socket input (Unix socket or TCP). Each input runs in its own thread */
struct IngestSource
{
    const char               *pszName;              /* the name in the metrics (source="unix"). Do not change it */
    const char               *pszTitle;             /* the name in the log messages: Unix, TCP, Syslog */
    bool                      bEnabled    = false;
    int                       fdListen    = -1;
    char                      szPath[1024] = {0};   /* Unix socket path. Removed when the input ends */
    bool                      bDatagram   = false;  /* datagram socket (syslog): no connections */
    std::atomic<size_t>       Running     {0};      /* set by main and by the input thread */
    pthread_t                 Thread      = {0};
    std::atomic<std::int64_t> Accepted    {0};
    std::atomic<std::int64_t> Dropped     {0};
    std::atomic<std::int64_t> Invalid     {0};
    std::atomic<std::int64_t> Connections {0};
    std::atomic<std::int64_t> Rejected    {0};

    IngestSource (const char *pszInputName, const char *pszInputTitle) : pszName (pszInputName), pszTitle (pszInputTitle) {}
};


/* Environment Variables */
char g_szEnvDataDir[]                 = "OTELFWD_DATA_DIR";
char g_szEnvOutputLog[]              = "OTELFWD_OUTPUT_LOG";
char g_szEnvLogLevel[]               = "OTELFWD_LOGLEVEL";
char g_szEnvMirrorToStdout[]         = "OTELFWD_MIRROR_STDOUT";
char g_szEnvPromFile[]               = "OTELFWD_PROM_FILE";
char g_szEnvHostname[]               = "OTELFWD_HOSTNAME";
char g_szEnvShutdownMaxSec[]         = "OTELFWD_SHUTDOWN_MAX_SEC";
char g_szEnvUnixSocket[]             = "OTELFWD_UNIX_SOCKET";
char g_szEnvUnixSocketMode[]         = "OTELFWD_UNIX_SOCKET_MODE";
char g_szEnvSyslogSocket[]           = "OTELFWD_SYSLOG_SOCKET";
char g_szEnvSyslogSocketMode[]       = "OTELFWD_SYSLOG_SOCKET_MODE";
char g_szEnvTcpListen[]              = "OTELFWD_TCP_LISTEN";
char g_szEnvFileInput[]              = "OTELFWD_FILE_INPUT";
char g_szEnvFileStart[]              = "OTELFWD_FILE_START";
char g_szEnvFileStateDir[]           = "OTELFWD_FILE_STATE_DIR";
char g_szEnvFileSeverity[]           = "OTELFWD_FILE_SEVERITY";
char g_szEnvFileService[]            = "OTELFWD_FILE_SERVICE";
char g_szEnvSocketQueueMax[]         = "OTELFWD_SOCKET_QUEUE_MAX";
char g_szEnvOtlpPushApiUrl[]         = "OTLP_PUSH_API_URL";
char g_szEnvOtlpPushApiUrlBackup[]   = "OTLP_PUSH_API_URL_BACKUP";
char g_szEnvOtlpPushFailbackSec[]    = "OTLP_PUSH_FAILBACK_SEC";
char g_szEnvOtlpPushWalRetrySec[]    = "OTLP_PUSH_WAL_RETRY_SEC";
char g_szEnvOtlpPushWalMaxMB[]       = "OTLP_PUSH_WAL_MAX_MB";
char g_szEnvOtlpPushEncoding[]       = "OTLP_PUSH_ENCODING";
char g_szEnvOtlpPushToken[]          = "OTLP_PUSH_TOKEN";
char g_szEnvOtlpCaFile[]             = "OTLP_CA_FILE";
char g_szEnvOtlpServiceName[]        = "OTLP_SERVICE_NAME";
char g_szEnvOtlpServiceNamespace[]   = "OTLP_SERVICE_NAMESPACE";
char g_szEnvOtlpServiceInstanceId[]  = "OTLP_SERVICE_INSTANCE_ID";


/* Globals */
char g_szHostname[1024]          = {0};
char g_szPidNbfFile[2048]        = {0};
char g_szOtlpPushApiURL[1024]    = {0};
char g_szOtlpPushApiURLBackup[1024] = {0};   /* optional second endpoint, used when the primary fails */
size_t g_FailbackSec             = OTELFWD_DEFAULT_FAILBACK_SEC;
size_t g_WalRetrySec             = OTELFWD_DEFAULT_WAL_RETRY_SEC;
size_t g_WalMaxMB                = OTELFWD_DEFAULT_WAL_MAX_MB;
bool   g_bPushProtobuf           = false;   /* the format of push requests: false is JSON, true is protobuf */
char g_szOtlpPushToken[1024]     = {0};
char g_szOtlpCaFile[1024]        = {0};
char g_szWalFile[2048]           = {0};
char g_szOutputLogFile[2048]     = {0};
char g_szMetricsFileName[2048]   = {0};
char g_szUnixSocketPath[1100]    = {0};   /* data dir (1024) plus the default path */
char g_szSyslogSocketPath[1024]  = {0};
char g_szTcpListen[256]          = {0};

/* The file input (OTELFWD_FILE_INPUT): one file. Its path is absolute. The state file is next to it (<file>.otelfwd-state), or in the
   state directory if there is one (OTELFWD_FILE_STATE_DIR: g_szFileStateDir is empty without it) */
char g_szFileInput[2048]         = {0};
char g_szFileStateDir[1024]      = {0};
char g_szFileStateFile[3200]     = {0};
bool g_bFileStartAtEnd           = false;       /* OTELFWD_FILE_START: without a state file the file is read from its end, not from its beginning */
int  g_FileSeverityNumber        = 9;           /* OTELFWD_FILE_SEVERITY: the severity of every line. info by default, 0 is none */
char g_szFileSeverityText[16]    = "INFO";
char g_szFileService[256]        = {0};         /* OTELFWD_FILE_SERVICE: service.name and service.namespace of the lines. Empty: the defaults of the forwarder */
bool g_bFileInputEnabled         = false;       /* the file input is configured and works: it is only set at start */

char g_szDataDir[1024]      = "/local/notesdata";
char g_szOtlpServiceName[1024]   = "domino";
char g_szServiceNamespace[1024]  = "domino";
char g_szServiceInstanceId[1024] = "";

/* Static string definitions */
char g_szVersion[40]           = {0};
char g_szCopyright[]           = OTELFWD_COPYRIGHT;
char g_szGitHubURL[]           = OTELFWD_GITHUB_URL;
char g_szTask[]                = "otelfwd";

char  g_szPromTypeGauge[]      = "gauge";
char  g_szPromTypeCounter[]    = "counter";
char  g_szPromTypeUntyped[]    = "untyped";
char  g_szPromPrefix[]         = "otelfwd";
char  g_szEmpty[]              = "";
char  g_szProcessEmpty[]       = "unknown";

/* Flags shared between threads: written by one thread (or by the signal handler) and read by others.
   They are atomic. The type has to be lock free, because the signal handler sets g_ShutdownRequested. */
static_assert (std::atomic<size_t>::is_always_lock_free, "std::atomic<size_t> has to be lock free to be used in a signal handler");

std::atomic<size_t> g_ShutdownRequested    {0};
std::atomic<size_t> g_ReloadRequested      {0};
std::atomic<size_t> g_PushThreadRunning    {0};
std::atomic<size_t> g_WalThreadRunning     {0};
std::atomic<size_t> g_MetricsThreadRunning {0};
std::atomic<size_t> g_FileThreadRunning    {0};

/* Options to enable functionality. Set once at startup, before any thread is created */
size_t g_LogLevel              = 0;
size_t g_Mirror2Stdout         = 0;
size_t g_NoStdin               = 0;     /* -nostdin: do not read STDIN, only the socket inputs are used */
size_t g_DumpEnvironment       = 1;
size_t g_ShutdownMaxWaitSec    = OTELFWD_DEFAULT_SHUTDOWN_MAX_WAIT_SEC;
size_t g_SocketQueueMax        = OTELFWD_DEFAULT_SOCKET_QUEUE_MAX;
mode_t g_UnixSocketMode        = 0600;
mode_t g_SyslogSocketMode      = 0600;

/* Thread status */
pthread_t g_WalThreadInstance     = {0};
pthread_t g_PushThreadInstance    = {0};
pthread_t g_MetricsThreadInstance = {0};
pthread_t g_FileThreadInstance    = {0};

/* Process start time */
time_t g_tStartTime = time (NULL);

/* Log files */
int g_fdOutputLogFile     = -1;
int g_fdStdOut = fileno (stdout);

/* Stat for pid.nbf update detection */
struct stat g_PidNbfStat {};

/* FIFO instance. stdin and the socket inputs feed it, the push thread drains it */
log_fifo g_LogFifo;

/* WAL implementation. Only used when OTLP push is configured */
SimpleWAL g_Wal;
bool g_bWalOpened = false;      /* the result of opening it. Only used for the summary at start, and for the health */

/* The health state for alerting (see health.hpp). Only the metrics thread updates it */
HealthMonitor g_Health;

/* The file input. g_FileReader belongs to the file thread, and to main before it starts and after it ended: it is not thread safe.
   The push thread does not touch it. It leaves the position of the lines which it delivered in g_FileCommit */
FileReader     g_FileReader;
FileCommitSlot g_FileCommit;

/* Which OTLP endpoint gets a push request: the primary, or the backup while the primary fails. Used by the push thread
   and by the thread which replays the WAL */
PushFailover g_PushFailover;

/* Socket inputs */
IngestSource g_IngestUnix   ("unix",   "Unix");
IngestSource g_IngestTcp    ("tcp",    "TCP");
IngestSource g_IngestSyslog ("syslog", "Syslog");

/* Statistic counters */
std::atomic<std::int64_t> g_Metric_LogLines         {0};
std::atomic<std::int64_t> g_Metric_PushSuccess      {0};
std::atomic<std::int64_t> g_Metric_PushErrors       {0};
std::atomic<std::int64_t> g_Metric_PushRetrySuccess {0};
std::atomic<std::int64_t> g_Metric_PushRetryErrors  {0};
std::atomic<std::int64_t> g_Metric_PushRejected     {0};   /* push requests which are dropped for good: the receiver refused them as bad data (HTTP 400), or they cannot be converted to protobuf */
std::atomic<std::int64_t> g_Metric_FileLines        {0};   /* lines read from the file input and queued */
std::atomic<std::int64_t> g_Metric_FileTruncated    {0};   /* the part of them which were longer than the maximum and were cut */
std::atomic<std::int64_t> g_Metric_FileCommitted    {0};   /* the position in the file up to which the lines were delivered. Written by the file thread */
std::atomic<std::int64_t> g_Metric_FileOpen         {0};   /* 1 if the file is open: 0 if it does not exist (yet) */
std::atomic<std::int64_t> g_Metric_WalRefused       {0};   /* push requests which the WAL did not take (it was full, or a failure). They are lost */
std::atomic<std::int64_t> g_Metric_PushConvertErrors {0};  /* the part of them which could not be converted to protobuf (only with OTLP_PUSH_ENCODING=protobuf) */

/* Helper functions */

bool IsNullStr (const char *pszStr)
{
    if (NULL == pszStr)
        return true;

    if ('\0' == *pszStr)
        return true;

    return false;
}


/* The console output of otelfwd: time, process name (pipe mode only), level, message. See log_line.hpp */
void LogMessage (const char *pszMessage)
{
    if (NULL == pszMessage)
        return;

    WriteLogLine (NULL, pszMessage);
}

void LogInfo (const char *pszMessage)
{
    if (NULL == pszMessage)
        return;

    WriteLogLine (NULL, pszMessage);
}

void LogError (const char *pszMessage)
{
    if (IsNullStr (pszMessage))
        return;

    WriteLogLine ("Error", pszMessage, NULL);
}

void LogError (const char *pszMessage, const char *pszErrorText)
{
    if (IsNullStr (pszMessage))
        return;

    if (IsNullStr (pszErrorText))
    {
        LogError (pszMessage);
    }
    else
    {
        WriteLogLine ("Error", pszMessage, pszErrorText);
    }
}


int IsDirectoryWritable (const char *pszPath)
{
    struct stat st;

    if (IsNullStr (pszPath))
        return 0;

    if (stat (pszPath, &st) != 0)
        return 0;

    if (!S_ISDIR(st.st_mode))
        return 0;

    if (access(pszPath, W_OK))
        return 0;

    return 1;
}


bool MakeDirectoryTree (const char *pszDirectory)
{
    std::error_code ec;

    if (IsNullStr (pszDirectory))
        return false;

    if (std::filesystem::exists(pszDirectory))
        return true;

    std::filesystem::create_directories(pszDirectory, ec);

    if (ec)
        return false;

    return true;
}


bool MakeDirectoryTreeFromFileName (const char *pszFile)
{

    if (std::filesystem::exists (pszFile))
        return true;

    {
        std::filesystem::path file = pszFile;
        std::filesystem::path dir = file.parent_path();

        MakeDirectoryTree (dir.c_str());
    }

    return true;
}


void sleep_ms (unsigned int ms)
{
    struct timespec ts;

    ts.tv_sec  = ms / 1000;
    ts.tv_nsec = (ms % 1000) * 1000000L;

    nanosleep (&ts, NULL);
}


bool IdleDelay (size_t seconds)
{
    while (seconds)
    {
        if (g_ShutdownRequested)
            return true;

        sleep (1);
        seconds--;
    }

    if (g_ShutdownRequested)
        return true;

    return false;
}


bool ParsePid (const std::string& text, pid_t& pid)
{
    errno = 0;
    char* pEnd = nullptr;

    long value = std::strtol(text.c_str(), &pEnd, 10);

    if (errno != 0 || pEnd == text.c_str() || *pEnd != '\0')
        return false;

    if (value < 0)
        return false;

    pid = static_cast<pid_t>(value);
    return true;
}


bool HasFileChanged (const char *pszFile, struct stat *pFileStat)
{
    struct stat CurrentStat {};
    bool bChanged = false;

    if (IsNullStr(pszFile) || pFileStat == NULL)
        return false;

    if (stat(pszFile, &CurrentStat) != 0)
        return false;

    // First observation
    if (pFileStat->st_ino == 0)
    {
        bChanged = true;
    }
    else
    {
        // Inode change → rotation / replacement
        if (CurrentStat.st_ino != pFileStat->st_ino)
            bChanged = true;

        // Size change → append / truncate
        else if (CurrentStat.st_size != pFileStat->st_size)
            bChanged = true;

        // Content modification time bChanged
        else if (CurrentStat.st_mtim.tv_sec  != pFileStat->st_mtim.tv_sec ||
                 CurrentStat.st_mtim.tv_nsec != pFileStat->st_mtim.tv_nsec)
            bChanged = true;

        // Metadata change (rename, truncate, etc.)
        else if (CurrentStat.st_ctim.tv_sec  != pFileStat->st_ctim.tv_sec ||
                 CurrentStat.st_ctim.tv_nsec != pFileStat->st_ctim.tv_nsec)
            bChanged = true;
    }

    *pFileStat = CurrentStat;
    return bChanged;
}


bool LoadPidMap (const char * pszPidFile, PidMap& pidMap)
{
    std::ifstream file (pszPidFile);
    pid_t pid = 0;

    if (IsNullStr (pszPidFile))
        return false;

    if (false == HasFileChanged (pszPidFile, &g_PidNbfStat))
        return false;

    if (!file.is_open())
    {
        LogError ("Cannot open the pid file", pszPidFile);
        return false;
    }

    std::string line;
    while (std::getline (file, line))
    {
        if (line.empty())
            continue;

        std::istringstream iss (line);

        std::string running;
        std::string pidText;
        std::string ppid;
        std::string processName;

        /* We only need the first four fields */
        if (!(iss >> running >> pidText >> ppid >> processName))
            continue;

        if (!ParsePid (pidText, pid))
            continue;

        pidMap[pid] = processName;
    }

    return true;
}


pid_t ExtractProcessId (const std::string& line)
{
    if (line.empty() || line[0] != '[')
        return 0;

    constexpr std::size_t kMaxPrefixLen = 40;
    const std::size_t searchEnd = std::min (line.size(), kMaxPrefixLen);

    pid_t pid = 0;

    /* Start after '[' */
    for (std::size_t i = 1; i < searchEnd; ++i)
    {
        const char c = line[i];

        if (c == ':')
        {
            /* Success only if we saw at least one digit */
            return pid;
        }

        if (c < '0' || c > '9')
        {
            return 0;
        }

        pid = pid * 10 + (c - '0');
    }

    /* No ':' encountered */
    return 0;
}


bool GetProcessNameFromMap (PidMap& pidMap, pid_t pid, std::string& processName)
{
    const auto it = pidMap.find (pid);

    if (it == pidMap.end())
    {
        processName = "";
        return false;
    }

    processName = it->second;
    return true;
}


bool GetProcessName (PidMap& pidMap, pid_t pid, std::string& processName)
{
    if (GetProcessNameFromMap (pidMap, pid, processName))
        return true;

    /* Try to reload */

    if (false == LoadPidMap (g_szPidNbfFile, pidMap))
        return false;

    if (GetProcessNameFromMap (pidMap, pid, processName))
        return true;

    return false;
}


static int64_t GetEpochNanoseconds()
{
    struct timespec ts {};
    clock_gettime (CLOCK_REALTIME, &ts);

    return ((int64_t)ts.tv_sec * 1000000000LL + (int64_t)ts.tv_nsec);
}


/* Replaces invalid UTF-8 sequences with U+FFFD. Log lines are not validated when read, and a payload
   with invalid UTF-8 could be rejected permanently by the endpoint, which would block the WAL replay. */
std::string ToValidUtf8 (const std::string& In)
{
    std::string Out;
    size_t i = 0;
    size_t n = In.size();

    Out.reserve (n);

    while (i < n)
    {
        unsigned char c   = static_cast<unsigned char>(In[i]);
        size_t        len = 0;
        bool          bOk = false;

        if (c < 0x80)
            len = 1;
        else if ( (c >= 0xC2) && (c <= 0xDF) )
            len = 2;
        else if ( (c >= 0xE0) && (c <= 0xEF) )
            len = 3;
        else if ( (c >= 0xF0) && (c <= 0xF4) )
            len = 4;

        bOk = ( (len > 0) && (i + len <= n) );

        for (size_t k = 1; bOk && (k < len); k++)
        {
            if (0x80 != (static_cast<unsigned char>(In[i + k]) & 0xC0))
                bOk = false;
        }

        /* Reject overlong encodings, surrogates and code points above U+10FFFF */
        if (bOk && (3 == len))
        {
            unsigned char c1 = static_cast<unsigned char>(In[i + 1]);

            if ( (0xE0 == c) && (c1 < 0xA0) )
                bOk = false;

            if ( (0xED == c) && (c1 >= 0xA0) )
                bOk = false;
        }

        if (bOk && (4 == len))
        {
            unsigned char c1 = static_cast<unsigned char>(In[i + 1]);

            if ( (0xF0 == c) && (c1 < 0x90) )
                bOk = false;

            if ( (0xF4 == c) && (c1 >= 0x90) )
                bOk = false;
        }

        if (bOk)
        {
            Out.append (In, i, len);
            i += len;
        }
        else
        {
            Out.append ("\xEF\xBF\xBD");
            i++;
        }
    }

    return Out;
}


OtelKV MakeStringKV (const char *pszKey, const std::string& Value)
{
    OtelKV KV;

    KV.Key      = pszKey;
    KV.Type     = OTEL_TYPE_STRING;
    KV.StrValue = Value;

    return KV;
}


OtelKV MakeIntKV (const char *pszKey, int64_t Value)
{
    OtelKV KV;

    KV.Key      = pszKey;
    KV.Type     = OTEL_TYPE_INT;
    KV.IntValue = Value;

    return KV;
}


bool SameKV (const OtelKV& A, const OtelKV& B)
{
    if ( (A.Type != B.Type) || (A.Key != B.Key) )
        return false;

    switch (A.Type)
    {
        case OTEL_TYPE_INT:
            return A.IntValue == B.IntValue;

        case OTEL_TYPE_DOUBLE:
            return A.DblValue == B.DblValue;

        case OTEL_TYPE_BOOL:
            return A.BoolValue == B.BoolValue;

        default:
            return A.StrValue == B.StrValue;
    }
}


bool SameKVs (const std::vector<OtelKV>& A, const std::vector<OtelKV>& B)
{
    if (A.size() != B.size())
        return false;

    for (size_t i = 0; i < A.size(); i++)
    {
        if (false == SameKV (A[i], B[i]))
            return false;
    }

    return true;
}


/* Resource attributes used when a record carries no resource of its own (stdin lines):
   identify the emitting service and instance.
   Some backends turn a default set of resource attributes (e.g. service.name, service.namespace,
   service.instance.id, host.name) into index labels and store all other attributes as structured metadata.
   The choice of attributes is kept in this one function on purpose, because it is likely to change. */
std::vector<OtelKV> GetDefaultResource()
{
    std::vector<OtelKV> Resource;

    Resource.push_back (MakeStringKV ("service.name",        g_szOtlpServiceName));
    Resource.push_back (MakeStringKV ("service.namespace",   g_szServiceNamespace));
    Resource.push_back (MakeStringKV ("service.instance.id", g_szServiceInstanceId));
    Resource.push_back (MakeStringKV ("host.name",           g_szHostname));
    Resource.push_back (MakeStringKV ("os.type",             "linux"));

    return Resource;
}


/* Log record attributes of stdin lines: process id and Domino server task. See GetDefaultResource */
void SetStdinAttributes (LogRecord& Record)
{
    Record.Attributes.clear();

    if (Record.Pid > 0)
        Record.Attributes.push_back (MakeIntKV ("process.pid", static_cast<int64_t>(Record.Pid)));

    Record.Attributes.push_back (MakeStringKV ("domino.task", Record.Process.empty() ? std::string (g_szProcessEmpty) : Record.Process));
}


rapidjson::Value MakeUtf8Value (const std::string& Text, rapidjson::Document::AllocatorType& alloc)
{
    std::string Valid = ToValidUtf8 (Text);

    return rapidjson::Value (Valid.c_str(), static_cast<rapidjson::SizeType>(Valid.size()), alloc);
}


/* 64-bit integers are encoded as decimal strings in OTLP/JSON */
void AddOtlpKV (rapidjson::Value& Attrs, const OtelKV& KV, rapidjson::Document::AllocatorType& alloc)
{
    rapidjson::Value Attr (rapidjson::kObjectType);
    rapidjson::Value Val  (rapidjson::kObjectType);

    switch (KV.Type)
    {
        case OTEL_TYPE_INT:
            Val.AddMember ("intValue", rapidjson::Value (std::to_string (KV.IntValue).c_str(), alloc), alloc);
            break;

        case OTEL_TYPE_DOUBLE:
            Val.AddMember ("doubleValue", rapidjson::Value (KV.DblValue), alloc);
            break;

        case OTEL_TYPE_BOOL:
            Val.AddMember ("boolValue", rapidjson::Value (KV.BoolValue), alloc);
            break;

        default:
            Val.AddMember ("stringValue", MakeUtf8Value (KV.StrValue, alloc), alloc);
            break;
    }

    Attr.AddMember ("key",   MakeUtf8Value (KV.Key, alloc), alloc);
    Attr.AddMember ("value", Val, alloc);

    Attrs.PushBack (Attr, alloc);
}


/* Builds one OTLP/HTTP JSON payload (ExportLogsServiceRequest) for one or more records.
   Records with the same scope and resource share one resourceLogs entry.
   Groups and the records within them keep the arrival order of the records.
   Records without a resource get the default resource, records without a scope the forwarder's scope. */
std::string BuildOtlpPayload (const std::vector<const LogRecord*>& Records)
{
    rapidjson::Document doc;
    doc.SetObject();
    auto& alloc = doc.GetAllocator();

    std::vector<std::vector<const LogRecord*>> Groups;

    for (const LogRecord* pRecord : Records)
    {
        bool bFound = false;

        for (auto& Group : Groups)
        {
            const LogRecord& First = *Group[0];

            if ( (First.ScopeName == pRecord->ScopeName) &&
                 (First.ScopeVersion == pRecord->ScopeVersion) &&
                 SameKVs (First.Resource, pRecord->Resource) )
            {
                Group.push_back (pRecord);
                bFound = true;
                break;
            }
        }

        if (false == bFound)
            Groups.push_back (std::vector<const LogRecord*> (1, pRecord));
    }

    std::vector<OtelKV> DefaultResource = GetDefaultResource();

    rapidjson::Value resourceLogs (rapidjson::kArrayType);

    for (const auto& Group : Groups)
    {
        const LogRecord& First = *Group[0];

        /* ---- resource ---- */
        const std::vector<OtelKV>& Resource = First.Resource.empty() ? DefaultResource : First.Resource;

        rapidjson::Value resourceAttrs (rapidjson::kArrayType);

        for (const OtelKV& KV : Resource)
            AddOtlpKV (resourceAttrs, KV, alloc);

        rapidjson::Value resource (rapidjson::kObjectType);
        resource.AddMember ("attributes", resourceAttrs, alloc);

        /* ---- scope ---- */
        rapidjson::Value scope (rapidjson::kObjectType);

        if (First.ScopeName.empty())
        {
            scope.AddMember ("name",    rapidjson::Value (g_szTask, alloc), alloc);
            scope.AddMember ("version", rapidjson::Value (g_szVersion, alloc), alloc);
        }
        else
        {
            scope.AddMember ("name", MakeUtf8Value (First.ScopeName, alloc), alloc);

            if (false == First.ScopeVersion.empty())
                scope.AddMember ("version", MakeUtf8Value (First.ScopeVersion, alloc), alloc);
        }

        /* ---- log records ---- */
        rapidjson::Value logRecords (rapidjson::kArrayType);

        for (const LogRecord* pRecord : Group)
        {
            rapidjson::Value logRecord (rapidjson::kObjectType);

            int64_t TimeNs     = pRecord->TimeNs ? pRecord->TimeNs : pRecord->ObservedTimeNs;
            int64_t ObservedNs = pRecord->ObservedTimeNs ? pRecord->ObservedTimeNs : pRecord->TimeNs;

            std::string ts       = std::to_string (TimeNs);
            std::string observed = std::to_string (ObservedNs);

            logRecord.AddMember ("timeUnixNano",         rapidjson::Value (ts.c_str(), alloc), alloc);
            logRecord.AddMember ("observedTimeUnixNano", rapidjson::Value (observed.c_str(), alloc), alloc);

            if (pRecord->SeverityNumber > 0)
                logRecord.AddMember ("severityNumber", pRecord->SeverityNumber, alloc);

            if (false == pRecord->SeverityText.empty())
                logRecord.AddMember ("severityText", MakeUtf8Value (pRecord->SeverityText, alloc), alloc);

            rapidjson::Value body (rapidjson::kObjectType);
            body.AddMember ("stringValue", MakeUtf8Value (pRecord->Line, alloc), alloc);
            logRecord.AddMember ("body", body, alloc);

            rapidjson::Value recordAttrs (rapidjson::kArrayType);

            for (const OtelKV& KV : pRecord->Attributes)
                AddOtlpKV (recordAttrs, KV, alloc);

            logRecord.AddMember ("attributes", recordAttrs, alloc);

            logRecords.PushBack (logRecord, alloc);
        }

        rapidjson::Value scopeLog (rapidjson::kObjectType);
        scopeLog.AddMember ("scope",      scope, alloc);
        scopeLog.AddMember ("logRecords", logRecords, alloc);

        rapidjson::Value scopeLogs (rapidjson::kArrayType);
        scopeLogs.PushBack (scopeLog, alloc);

        rapidjson::Value resourceLog (rapidjson::kObjectType);
        resourceLog.AddMember ("resource",  resource, alloc);
        resourceLog.AddMember ("scopeLogs", scopeLogs, alloc);

        resourceLogs.PushBack (resourceLog, alloc);
    }

    doc.AddMember ("resourceLogs", resourceLogs, alloc);

    /* ---- serialize ---- */
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer(buffer);
    doc.Accept (writer);

    return buffer.GetString();
}


/* ---- Flat record format decoder ----
   One JSON object per line:

   { "time_unix_nano": "...", "observed_time_unix_nano": "...", "severity_number": 17, "severity_text": "ERROR",
     "body": "...", "scope": { "name": "...", "version": "..." },
     "resource": { "key": value, ... }, "attributes": { "key": value, ... } }

   All fields are optional. Times can be decimal strings or integers.
   Attribute values: string, integer, floating point, boolean. An object or array is converted to its JSON text. */

std::string JsonToText (const rapidjson::Value& Val)
{
    rapidjson::StringBuffer buffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer (buffer);

    Val.Accept (writer);

    return std::string (buffer.GetString(), buffer.GetSize());
}


bool GetUnixNanoFromJson (const rapidjson::Value& Val, int64_t& retNano)
{
    const uint64_t Max = static_cast<uint64_t>(std::numeric_limits<int64_t>::max());
    uint64_t       n   = 0;

    if (Val.IsString())
    {
        const char *p = Val.GetString();

        if ('\0' == *p)
            return false;

        for (; *p; p++)
        {
            if ( (*p < '0') || (*p > '9') )
                return false;

            if (n > ((std::numeric_limits<uint64_t>::max() - static_cast<uint64_t>(*p - '0')) / 10))
                return false;

            n = (n * 10) + static_cast<uint64_t>(*p - '0');
        }
    }
    else if (Val.IsUint64())
    {
        n = Val.GetUint64();
    }
    else
    {
        return false;
    }

    if (n > Max)
        return false;

    retNano = static_cast<int64_t>(n);
    return true;
}


void AddKVsFromJsonObject (const rapidjson::Value& Obj, std::vector<OtelKV>& retKVs)
{
    if (false == Obj.IsObject())
        return;

    for (rapidjson::Value::ConstMemberIterator it = Obj.MemberBegin(); it != Obj.MemberEnd(); ++it)
    {
        const rapidjson::Value& Val = it->value;
        OtelKV KV;

        KV.Key = std::string (it->name.GetString(), it->name.GetStringLength());

        if (Val.IsNull())
            continue;

        if (Val.IsBool())
        {
            KV.Type      = OTEL_TYPE_BOOL;
            KV.BoolValue = Val.GetBool();
        }
        else if (Val.IsString())
        {
            KV.Type     = OTEL_TYPE_STRING;
            KV.StrValue = std::string (Val.GetString(), Val.GetStringLength());
        }
        else if (Val.IsInt64())
        {
            KV.Type     = OTEL_TYPE_INT;
            KV.IntValue = Val.GetInt64();
        }
        else if (Val.IsUint64())
        {
            /* Does not fit into a signed 64 bit integer: keep the exact value as text */
            KV.Type     = OTEL_TYPE_STRING;
            KV.StrValue = std::to_string (Val.GetUint64());
        }
        else if (Val.IsDouble())
        {
            KV.Type     = OTEL_TYPE_DOUBLE;
            KV.DblValue = Val.GetDouble();
        }
        else
        {
            KV.Type     = OTEL_TYPE_STRING;
            KV.StrValue = JsonToText (Val);
        }

        retKVs.push_back (std::move (KV));
    }
}


bool DecodeFlatRecord (const std::string& Text, int64_t ArrivalNs, LogRecord& retRecord)
{
    rapidjson::Document doc;
    rapidjson::Value::ConstMemberIterator it;
    int64_t TimeNs     = 0;
    int64_t ObservedNs = 0;

    retRecord = LogRecord();

    doc.Parse (Text.c_str());

    if (doc.HasParseError() || (false == doc.IsObject()))
        return false;

    retRecord.Source = LOG_SOURCE_SOCKET;

    it = doc.FindMember ("time_unix_nano");
    if (it != doc.MemberEnd())
        GetUnixNanoFromJson (it->value, TimeNs);

    it = doc.FindMember ("observed_time_unix_nano");
    if (it != doc.MemberEnd())
        GetUnixNanoFromJson (it->value, ObservedNs);

    it = doc.FindMember ("severity_number");
    if ( (it != doc.MemberEnd()) && it->value.IsInt() && (it->value.GetInt() >= 0) && (it->value.GetInt() <= 24) )
        retRecord.SeverityNumber = it->value.GetInt();

    it = doc.FindMember ("severity_text");
    if ( (it != doc.MemberEnd()) && it->value.IsString() )
        retRecord.SeverityText = std::string (it->value.GetString(), it->value.GetStringLength());

    it = doc.FindMember ("body");
    if (it != doc.MemberEnd())
    {
        if (it->value.IsString())
            retRecord.Line = std::string (it->value.GetString(), it->value.GetStringLength());
        else if (false == it->value.IsNull())
            retRecord.Line = JsonToText (it->value);
    }

    it = doc.FindMember ("scope");
    if ( (it != doc.MemberEnd()) && it->value.IsObject() )
    {
        rapidjson::Value::ConstMemberIterator itScope;

        itScope = it->value.FindMember ("name");
        if ( (itScope != it->value.MemberEnd()) && itScope->value.IsString() )
            retRecord.ScopeName = std::string (itScope->value.GetString(), itScope->value.GetStringLength());

        itScope = it->value.FindMember ("version");
        if ( (itScope != it->value.MemberEnd()) && itScope->value.IsString() )
            retRecord.ScopeVersion = std::string (itScope->value.GetString(), itScope->value.GetStringLength());
    }

    it = doc.FindMember ("resource");
    if (it != doc.MemberEnd())
        AddKVsFromJsonObject (it->value, retRecord.Resource);

    it = doc.FindMember ("attributes");
    if (it != doc.MemberEnd())
        AddKVsFromJsonObject (it->value, retRecord.Attributes);

    /* Times: a missing observed time is the arrival time, a missing event time is the observed time */
    retRecord.ObservedTimeNs = ObservedNs ? ObservedNs : ArrivalNs;
    retRecord.TimeNs         = TimeNs     ? TimeNs     : retRecord.ObservedTimeNs;

    return true;
}


bool GetHostname (size_t MaxSize, char * retpszHostname)
{
    if (0 != gethostname (retpszHostname, MaxSize-1))
        *retpszHostname = '\0';
    else
        retpszHostname[MaxSize - 1] = '\0';

    return true;
}


bool SendPayloadToWAL (const std::string& payload)
{
    if (g_Wal.Append (payload.c_str(), payload.size()))
        return true;

    g_Metric_WalRefused.fetch_add (1, std::memory_order_relaxed);
    return false;
}


/* Collects the start of the answer of the receiver, for the log. Without a callback libcurl writes the answer to stdout */
static size_t PushWriteCallback (char *pData, size_t Size, size_t Items, void *pUserData)
{
    std::string *pResponse = static_cast<std::string *> (pUserData);
    size_t Len = Size * Items;
    size_t Max = OTELFWD_PUSH_RESPONSE_MAX;

    if ( pResponse && (pResponse->size() < Max) )
        pResponse->append (pData, std::min (Len, Max - pResponse->size()));

    return Len;
}


/* Logs why a push request was not accepted: the status and the start of the answer. The answer can carry control characters */
static void LogPushStatus (PushAction Action, long HttpCode, const std::string& Response)
{
    static std::atomic<time_t> tLastRejected {0};
    std::string Text = "HTTP status " + std::to_string (HttpCode);
    std::string Answer = Response;

    for (char& c : Answer)
    {
        if ( (static_cast<unsigned char>(c) < 0x20) || (0x7f == static_cast<unsigned char>(c)) )
            c = ' ';
    }

    while ( (false == Answer.empty()) && (' ' == Answer.back()) )
        Answer.pop_back();

    if (false == Answer.empty())
        Text += ": " + Answer;

    if (PUSH_REJECTED == Action)
    {
        /* A receiver which refuses everything would make a message for every request: at most one every 10 seconds */
        time_t tNow  = time (NULL);
        time_t tLast = tLastRejected.load();

        if ( (tNow - tLast < 10) || (false == tLastRejected.compare_exchange_strong (tLast, tNow)) )
            return;

        LogError ("The receiver refused the push request as bad data. The data is dropped", Text.c_str());
        return;
    }

    LogError ("Push request was not accepted", Text.c_str());
}


/* The value of OTLP_PUSH_ENCODING: json or protobuf, in any case. Returns false for any other text */
static bool ParsePushEncoding (const char *pszValue, bool& retProtobuf)
{
    if (0 == strcasecmp (pszValue, "json"))
    {
        retProtobuf = false;
        return true;
    }

    if (0 == strcasecmp (pszValue, "protobuf"))
    {
        retProtobuf = true;
        return true;
    }

    return false;
}


/* The value of OTELFWD_FILE_START: begin or end, in any case. Returns false for any other text */
static bool ParseFileStart (const char *pszValue, bool& retAtEnd)
{
    if (0 == strcasecmp (pszValue, "begin"))
    {
        retAtEnd = false;
        return true;
    }

    if (0 == strcasecmp (pszValue, "end"))
    {
        retAtEnd = true;
        return true;
    }

    return false;
}


/* The path as an absolute path: the same text every time, because the name of the state file is made from it. A relative path is
   relative to the directory the program was started in */
static void MakeAbsolutePath (const char *pszPath, char *pszOut, size_t OutSize)
{
    char szDirectory[1024] = {0};

    if ( ('/' == *pszPath) || (NULL == getcwd (szDirectory, sizeof (szDirectory))) )
        snprintf (pszOut, OutSize, "%s", pszPath);
    else
        snprintf (pszOut, OutSize, "%s/%s", szDirectory, pszPath);
}


/* A number of seconds from 1 to MaxValue, and nothing else after the number. Returns false for any other text */
static bool ParseSecondsSetting (const char *pszValue, long MaxValue, size_t& retSeconds)
{
    char *pEnd  = NULL;
    long  Value = strtol (pszValue, &pEnd, 10);

    if ( (pEnd == pszValue) || ('\0' != *pEnd) || (Value < 1) || (Value > MaxValue) )
        return false;

    retSeconds = static_cast<size_t> (Value);
    return true;
}


/* A number of MB from 0 to MaxValue, and nothing else after the number. 0 is no limit. Returns false for any other text */
static bool ParseMegabytesSetting (const char *pszValue, long MaxValue, size_t& retMegabytes)
{
    char *pEnd  = NULL;
    long  Value = strtol (pszValue, &pEnd, 10);

    if ( (pEnd == pszValue) || ('\0' != *pEnd) || (Value < 0) || (Value > MaxValue) )
        return false;

    retMegabytes = static_cast<size_t> (Value);
    return true;
}


static const char *GetPushEncodingName()
{
    return g_bPushProtobuf ? "protobuf" : "json";
}


/* A request which cannot be converted is dropped. A converter which fails for every request would make a message for every
   request: at most one every 10 seconds. The counter shows how many there were */
static void LogConvertError (const std::string& Error)
{
    static std::atomic<time_t> tLastLogged {0};

    time_t tNow  = time (NULL);
    time_t tLast = tLastLogged.load();

    if ( (tNow - tLast < 10) || (false == tLastLogged.compare_exchange_strong (tLast, tNow)) )
        return;

    LogError ("A push request cannot be converted from JSON to protobuf. The data is dropped", Error.c_str());
}


/* Posts a payload to the OTLP/HTTP logs endpoint, in the format which the content type names.
   What the answer means is decided by GetPushAction (push_status.hpp) */
PushAction SendPushPayload (CURL* pCurl, const char *pszURL, const char *pszPushToken, const char *pszCaFile, const char *pszContentType, const char* pszBuffer, size_t BufferLen, long TimeoutSec)
{
    PushAction Action = PUSH_RETRY;
    char szErrorBuffer[CURL_ERROR_SIZE+10] = {0};
    CURLcode rc = CURLE_OK;
    long HttpCode = 0;
    std::string Response;
    struct curl_slist* pHeaders = nullptr;

    if (IsNullStr (pszBuffer))
        return PUSH_RETRY;

    if (nullptr == pCurl)
    {
        LogError ("No curl handle specified");
        goto Done;
    }

    if (IsNullStr (pszURL))
    {
        LogError ("No Push URL specified");
        goto Done;
    }

    curl_easy_reset (pCurl);

    pHeaders = curl_slist_append (pHeaders, (std::string ("Content-Type: ") + pszContentType).c_str());
    curl_easy_setopt (pCurl, CURLOPT_HTTPHEADER, pHeaders);
    curl_easy_setopt (pCurl, CURLOPT_POST, 1L);
    curl_easy_setopt (pCurl, CURLOPT_TIMEOUT, TimeoutSec);

    /* A receiver which does not answer must not block the push thread for the whole timeout */
    curl_easy_setopt (pCurl, CURLOPT_CONNECTTIMEOUT, static_cast<long> (OTELFWD_PUSH_CONNECT_TIMEOUT_SEC));

    /* No CURLOPT_FAILONERROR: it would throw away the answer of an error status, and the status is judged below */
    curl_easy_setopt (pCurl, CURLOPT_WRITEFUNCTION, PushWriteCallback);
    curl_easy_setopt (pCurl, CURLOPT_WRITEDATA, &Response);

    curl_easy_setopt (pCurl, CURLOPT_ERRORBUFFER, szErrorBuffer);
    curl_easy_setopt (pCurl, CURLOPT_URL, pszURL);
    curl_easy_setopt (pCurl, CURLOPT_POSTFIELDS, pszBuffer);
    curl_easy_setopt (pCurl, CURLOPT_POSTFIELDSIZE, BufferLen);

    if (false == IsNullStr (pszCaFile))
    {
        curl_easy_setopt (pCurl, CURLOPT_CAINFO, pszCaFile);
    }

    if (false == IsNullStr (pszPushToken))
    {
        curl_easy_setopt (pCurl, CURLOPT_XOAUTH2_BEARER, pszPushToken);
        curl_easy_setopt (pCurl, CURLOPT_HTTPAUTH, CURLAUTH_BEARER);
    }

    rc = curl_easy_perform (pCurl);

    /* No answer at all (no connection, timeout ...): the request is tried again later */
    if (CURLE_OK != rc)
    {
        LogError ("Curl operation failed", szErrorBuffer);
        goto Done;
    }

    curl_easy_getinfo (pCurl, CURLINFO_RESPONSE_CODE, &HttpCode);

    Action = GetPushAction (HttpCode);

    if (PUSH_ACCEPTED != Action)
        LogPushStatus (Action, HttpCode, Response);

Done:

    if (pHeaders)
    {
        curl_slist_free_all (pHeaders);
        pHeaders = nullptr;
    }

    return Action;
}


/* Sends one push request to the endpoint which g_PushFailover chooses: the primary, or the backup while the primary fails.
   The token and the CA file are the same for both. A change of the endpoint in use is logged: it is the one message an
   admin needs to see that the traffic moved.

   The buffer is JSON, the format of the WAL. With OTLP_PUSH_ENCODING=protobuf it is converted here, once, before the failover
   logic: both endpoints and the try of the primary get the same bytes. A request which cannot be converted is dropped for good
   (PUSH_REJECTED, like data which the receiver refuses): trying again can not change the result, and it would block the WAL */
PushAction SendPushRequest (CURL* pCurl, const char* pszBuffer, size_t BufferLen)
{
    int  SwitchedTo = -1;
    char szMessage[2200] = {0};

    std::string Converted;
    const char *pszBody        = pszBuffer;
    size_t      BodyLen        = BufferLen;
    const char *pszContentType = OTELFWD_CONTENT_TYPE_JSON;

    if (g_bPushProtobuf)
    {
        std::string Error;

        if (false == ConvertOtlpJsonToProtobuf (pszBuffer, BufferLen, Converted, Error))
        {
            g_Metric_PushConvertErrors.fetch_add (1, std::memory_order_relaxed);
            LogConvertError (Error);
            return PUSH_REJECTED;
        }

        pszBody        = Converted.data();
        BodyLen        = Converted.size();
        pszContentType = OTELFWD_CONTENT_TYPE_PROTOBUF;
    }

    PushAction Action = g_PushFailover.Send ([&] (PushEndpoint Endpoint, bool bProbe)
    {
        const char *pszURL = (PUSH_PRIMARY == Endpoint) ? g_szOtlpPushApiURL : g_szOtlpPushApiURLBackup;
        long TimeoutSec    = bProbe ? OTELFWD_PUSH_PROBE_TIMEOUT_SEC : OTELFWD_PUSH_TIMEOUT_SEC;

        return SendPushPayload (pCurl, pszURL, g_szOtlpPushToken, g_szOtlpCaFile, pszContentType, pszBody, BodyLen, TimeoutSec);
    }, time (NULL), &SwitchedTo);

    if (PUSH_BACKUP == SwitchedTo)
    {
        snprintf (szMessage, sizeof (szMessage), "The primary endpoint failed. Using the backup endpoint %s. The primary is tried again every %lu seconds",
                  g_szOtlpPushApiURLBackup, static_cast<unsigned long> (g_FailbackSec));
        LogMessage (szMessage);
    }
    else if (PUSH_PRIMARY == SwitchedTo)
    {
        snprintf (szMessage, sizeof (szMessage), "The primary endpoint %s accepts requests again. Using it again", g_szOtlpPushApiURL);
        LogMessage (szMessage);
    }

    return Action;
}


/* The push thread is done with a batch, and its records are safe: they were accepted, or kept in the WAL, or refused for good.
   Tells the file thread how far the lines of the file input were delivered. It commits, this thread does not touch the reader */
static void HandOverFilePositions (const std::vector<LogRecord>& Batch)
{
    for (const LogRecord& Record : Batch)
    {
        if (LOG_SOURCE_FILE == Record.Source)
            g_FileCommit.Add (Record.FileGeneration, Record.FileEndOffset);
    }
}


void *PushThread (void *arg)
{
    (void)arg;
    CURL* pCurl = nullptr;
    std::vector<LogRecord> Batch;
    std::vector<const LogRecord*> OtlpBatch;
    PidMap pidMap;

    g_PushThreadRunning = 1;

    if (g_LogLevel)
        LogMessage ("Push Thread started");

    if (*g_szOtlpPushApiURL)
    {
        pCurl = curl_easy_init();
        if (!pCurl)
        {
            LogError ("curl_easy_init failed");
            goto Done;
        }
    }

    while (0 == g_ShutdownRequested)
    {
        while (g_LogFifo.pop_batch (Batch, OTELFWD_MAX_BATCH_RECORDS, OTELFWD_MAX_BATCH_BYTES))
        {
            g_Metric_LogLines.fetch_add (static_cast<std::int64_t>(Batch.size()), std::memory_order_relaxed);

            OtlpBatch.clear();

            /* False if a record could neither be pushed nor kept in the WAL: the position of the file is not moved for this batch.
               That only helps if the program restarts before a later batch is delivered: a later batch moves the position past
               these lines, and they are lost (the WAL was full: see the health) */
            bool bBatchSafe = true;

            /* Annotate the stdin records of the batch and collect the records to push */
            for (LogRecord& Record : Batch)
            {
                if (LOG_SOURCE_STDIN == Record.Source)
                {
                    Record.Pid = ExtractProcessId (Record.Line);

                    GetProcessName (pidMap, Record.Pid, Record.Process);
                    SetStdinAttributes (Record);
                }

                if (*g_szOtlpPushApiURL)
                {
                    /* WAL-TESTING can be used to test WAL logic */
                    if (Record.Line.find ("WAL-TESTING") != std::string::npos)
                    {
                        LogMessage ("WAL-TESTING string received");

                        if (false == SendPayloadToWAL (BuildOtlpPayload (std::vector<const LogRecord*> (1, &Record))))
                            bBatchSafe = false;
                    }
                    else
                    {
                        OtlpBatch.push_back (&Record);
                    }
                }
            }

            /* Push the batch as one request. On failure the whole payload goes to the WAL */
            if (false == OtlpBatch.empty())
            {
                std::string jOtlpPayload = BuildOtlpPayload (OtlpBatch);
                std::int64_t BatchSize   = static_cast<std::int64_t>(OtlpBatch.size());

                PushAction Action = SendPushRequest (pCurl, jOtlpPayload.c_str(), jOtlpPayload.size());

                if (PUSH_ACCEPTED == Action)
                {
                    g_Metric_PushSuccess.fetch_add (BatchSize, std::memory_order_relaxed);
                }
                else if (PUSH_REJECTED == Action)
                {
                    /* The receiver refuses this data for good: not kept in the WAL. It is counted and logged */
                    g_Metric_PushRejected.fetch_add (1, std::memory_order_relaxed);
                }
                else
                {
                    g_Metric_PushErrors.fetch_add (BatchSize, std::memory_order_relaxed);

                    if (false == SendPayloadToWAL (jOtlpPayload))
                        bBatchSafe = false;
                }
            }

            if (bBatchSafe)
                HandOverFilePositions (Batch);
        }
    }

Done:

    if (pCurl)
    {
        curl_easy_cleanup (pCurl);
        pCurl = nullptr;
    }

    g_PushThreadRunning = 0;

    if (g_LogLevel)
        LogMessage ("Push Thread ended");

    return NULL;
}


bool PushWalEntries()
{
    bool bSuccess = false;
    CURL* pCurl = nullptr;

    if (false == g_Wal.IsReplayPending())
        return true;

    pCurl = curl_easy_init();

    if (!pCurl)
    {
        LogError ("curl_easy_init failed");
        return false;
    }

    bSuccess = g_Wal.Replay ([pCurl] (const std::vector<uint8_t>& Record)
    {
        PushAction Action = SendPushRequest (pCurl, (const char *) Record.data(), Record.size());

        if (PUSH_ACCEPTED == Action)
        {
            g_Metric_PushRetrySuccess.fetch_add (1, std::memory_order_relaxed);
            return true;
        }

        if (PUSH_REJECTED == Action)
        {
            /* The receiver refuses this record for good. Drop it, or it would block every record behind it */
            g_Metric_PushRejected.fetch_add (1, std::memory_order_relaxed);
            return true;
        }

        g_Metric_PushRetryErrors.fetch_add (1, std::memory_order_relaxed);
        return false;
    });

    if (pCurl)
    {
        curl_easy_cleanup (pCurl);
        pCurl = nullptr;
    }

    return bSuccess;
}


void *WalThread (void *arg)
{
    (void)arg;
    g_WalThreadRunning = 1;

    if (g_LogLevel)
        LogMessage ("WAL Thread started");

    while (0 == g_ShutdownRequested)
    {
        if (g_Wal.IsReplayPending())
        {
            if (g_ShutdownRequested)
                break;

            if (false == PushWalEntries())
            {
                char szMessage[200] = {0};

                snprintf (szMessage, sizeof (szMessage), "Cannot send the WAL to the receiver. Trying again in %lu seconds", static_cast<unsigned long> (g_WalRetrySec));
                LogError (szMessage);

                if (IdleDelay (g_WalRetrySec))
                {
                    break;
                }
            }
        }

        sleep (1);
    }

    g_WalThreadRunning = 0;

    if (g_LogLevel)
        LogMessage ("WAL Thread ended");

    return NULL;
}


/* ---- Socket inputs ---- */

struct IngestClient
{
    int         fd;
    std::string Buffer;
    bool        bDiscarding = false;    /* skipping the rest of a line that exceeded the maximum length */

    explicit IngestClient (int fdClient) : fd (fdClient) {}
};


void IngestHandleLine (IngestSource *pSource, std::string& Line)
{
    LogRecord Record;

    if ( (false == Line.empty()) && ('\r' == Line.back()) )
        Line.pop_back();

    if (Line.empty())
        return;

    if (false == DecodeFlatRecord (Line, GetEpochNanoseconds(), Record))
    {
        pSource->Invalid.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    if (g_LogFifo.try_push (std::move (Record), g_SocketQueueMax))
        pSource->Accepted.fetch_add (1, std::memory_order_relaxed);
    else
        pSource->Dropped.fetch_add (1, std::memory_order_relaxed);
}


/* Splits the received data into lines. bEof: the sender closed the connection, a last line without new line is processed */
void IngestProcessBuffer (IngestSource *pSource, IngestClient& Client, bool bEof)
{
    size_t Start = 0;
    size_t Pos   = 0;

    while (std::string::npos != (Pos = Client.Buffer.find ('\n', Start)))
    {
        if (Client.bDiscarding)
        {
            Client.bDiscarding = false;
        }
        else
        {
            std::string Line = Client.Buffer.substr (Start, Pos - Start);
            IngestHandleLine (pSource, Line);
        }

        Start = Pos + 1;
    }

    Client.Buffer.erase (0, Start);

    if (bEof)
    {
        if ( (false == Client.bDiscarding) && (false == Client.Buffer.empty()) )
        {
            std::string Line = Client.Buffer;
            IngestHandleLine (pSource, Line);
        }

        Client.Buffer.clear();
        return;
    }

    if (Client.bDiscarding)
    {
        Client.Buffer.clear();
    }
    else if (Client.Buffer.size() > OTELFWD_MAX_LINE_BYTES)
    {
        pSource->Invalid.fetch_add (1, std::memory_order_relaxed);
        Client.Buffer.clear();
        Client.bDiscarding = true;
    }
}


void *IngestThread (void *arg)
{
    IngestSource *pSource = (IngestSource *) arg;
    std::vector<IngestClient> Clients;
    std::vector<struct pollfd> PollFds;
    char szBuffer[65536];
    char szMessage[256] = {0};
    sigset_t SigSet;
    int rc = 0;

    /* Signals are handled by the main thread */
    sigemptyset (&SigSet);
    sigaddset (&SigSet, SIGINT);
    sigaddset (&SigSet, SIGTERM);
    sigaddset (&SigSet, SIGHUP);
    pthread_sigmask (SIG_BLOCK, &SigSet, NULL);

    if (g_LogLevel)
    {
        snprintf (szMessage, sizeof (szMessage), "%s input thread started", pSource->pszTitle);
        LogMessage (szMessage);
    }

    while (0 == g_ShutdownRequested)
    {
        PollFds.clear();

        struct pollfd PollListen = {};
        PollListen.fd     = pSource->fdListen;
        PollListen.events = POLLIN;
        PollFds.push_back (PollListen);

        for (const IngestClient& Client : Clients)
        {
            struct pollfd PollClient = {};
            PollClient.fd     = Client.fd;
            PollClient.events = POLLIN;
            PollFds.push_back (PollClient);
        }

        rc = poll (PollFds.data(), PollFds.size(), 1000);

        if (rc < 0)
        {
            if (EINTR == errno)
                continue;

            LogError ("poll failed", strerror (errno));
            break;
        }

        if (0 == rc)
            continue;

        /* PollFds[i+1] belongs to Clients[i]. Go backwards, so removing a client does not shift the ones still to handle */
        for (size_t i = Clients.size(); i > 0; i--)
        {
            IngestClient& Client = Clients[i-1];
            bool bClose = false;
            ssize_t nread = 0;

            if (0 == (PollFds[i].revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL)))
                continue;

            nread = recv (Client.fd, szBuffer, sizeof (szBuffer), 0);

            if (nread > 0)
            {
                Client.Buffer.append (szBuffer, static_cast<size_t>(nread));
                IngestProcessBuffer (pSource, Client, false);
            }
            else if (0 == nread)
            {
                IngestProcessBuffer (pSource, Client, true);
                bClose = true;
            }
            else if ( (EAGAIN != errno) && (EWOULDBLOCK != errno) && (EINTR != errno) )
            {
                bClose = true;
            }

            if (bClose)
            {
                close (Client.fd);
                Clients.erase (Clients.begin() + (i - 1));
            }
        }

        if (PollFds[0].revents & POLLIN)
        {
            while (true)
            {
                int fdClient = accept4 (pSource->fdListen, NULL, NULL, SOCK_NONBLOCK | SOCK_CLOEXEC);

                if (fdClient < 0)
                {
                    /* Out of file descriptors: do not spin */
                    if ( (EMFILE == errno) || (ENFILE == errno) )
                        sleep_ms (100);

                    break;
                }

                if (Clients.size() >= OTELFWD_MAX_CLIENTS_PER_INPUT)
                {
                    close (fdClient);
                    pSource->Rejected.fetch_add (1, std::memory_order_relaxed);
                    continue;
                }

                Clients.push_back (IngestClient (fdClient));
                pSource->Connections.fetch_add (1, std::memory_order_relaxed);
            }
        }
    }

    for (IngestClient& Client : Clients)
        close (Client.fd);

    close (pSource->fdListen);
    pSource->fdListen = -1;

    if (pSource->szPath[0])
        unlink (pSource->szPath);

    pSource->Running = 0;

    if (g_LogLevel)
    {
        snprintf (szMessage, sizeof (szMessage), "%s input thread ended", pSource->pszTitle);
        LogMessage (szMessage);
    }

    return NULL;
}


/* ---- File input ----
   One file (OTELFWD_FILE_INPUT) is followed like tail -F, and every line is a log record: the line is the body, the time is the time
   it was read, and the attribute log.file.path is the file. The reading is done by the FileReader (filereader/README.md): only complete
   lines, rotation and truncation are handled, and the position is saved in a state file, so that a restart continues where it stopped.

   The position is not saved when a line is read, but when it was delivered. The record carries the end of its line in the file. The
   push thread hands the position of a finished batch over in g_FileCommit (FileCommitSlot), and this thread commits it. A crash or a restart
   repeats the lines which were read and not delivered: at least once, like the WAL.

   The thread reads only while the queue has room (OTELFWD_SOCKET_QUEUE_MAX). When the receiver is slow the queue fills up, and the file
   waits: nothing is dropped, the file is the buffer. Empty lines are not sent. */

/* Commits what the push thread delivered. The state file may not be writable: then the position is tried again in the next round.
   bRetry, Generation and EndOffset are the position which was not committed yet, kept by the caller between the rounds */
static void CommitDeliveredFilePositions (bool& bRetry, uint64_t& Generation, uint64_t& EndOffset)
{
    if (g_FileCommit.Take (Generation, EndOffset))
        bRetry = true;

    if ( bRetry && g_FileReader.Commit (Generation, EndOffset) )
    {
        bRetry = false;
        g_Metric_FileCommitted.store (static_cast<std::int64_t> (g_FileReader.GetCommitted()), std::memory_order_relaxed);
    }
}


/* The resource of the records of the file input: the default resource of the forwarder, with the service which OTELFWD_FILE_SERVICE
   sets, as the name and as the namespace (like the syslog input does with its tag). Empty without the setting: the records then use
   the default resource */
static std::vector<OtelKV> GetFileResource()
{
    std::vector<OtelKV> Resource;

    if (IsNullStr (g_szFileService))
        return Resource;

    Resource = GetDefaultResource();

    for (OtelKV& KV : Resource)
    {
        if ( ("service.name" == KV.Key) || ("service.namespace" == KV.Key) )
            KV.StrValue = g_szFileService;
    }

    return Resource;
}


void *FileThread (void *arg)
{
    (void)arg;

    FileReader::Line    Line;
    uint64_t            Generation = 0;
    uint64_t            EndOffset  = 0;
    bool                bRetry     = false;
    OtelKV              PathAttribute = MakeStringKV ("log.file.path", g_szFileInput);
    std::vector<OtelKV> Resource   = GetFileResource();
    sigset_t            SigSet;

    /* Signals are handled by the main thread */
    sigemptyset (&SigSet);
    sigaddset (&SigSet, SIGINT);
    sigaddset (&SigSet, SIGTERM);
    sigaddset (&SigSet, SIGHUP);
    pthread_sigmask (SIG_BLOCK, &SigSet, NULL);

    if (g_LogLevel)
        LogMessage ("File input thread started");

    while (0 == g_ShutdownRequested)
    {
        size_t Lines = 0;

        CommitDeliveredFilePositions (bRetry, Generation, EndOffset);

        /* The room is looked at before a line is read: a line which was read has to go into the queue */
        while ( (Lines < OTELFWD_FILE_LINES_PER_ROUND) && (g_LogFifo.size() < g_SocketQueueMax) && g_FileReader.ReadLine (Line, time (NULL)) )
        {
            Lines++;

            if (Line.bTruncated)
                g_Metric_FileTruncated.fetch_add (1, std::memory_order_relaxed);

            /* An empty line says nothing. Its position is covered by the next line which is delivered */
            if (Line.Text.empty())
                continue;

            LogRecord Record;

            Record.Source         = LOG_SOURCE_FILE;
            Record.TimeNs         = GetEpochNanoseconds();
            Record.Line           = std::move (Line.Text);
            Record.SeverityNumber = g_FileSeverityNumber;
            Record.SeverityText   = g_szFileSeverityText;
            Record.Resource       = Resource;
            Record.FileGeneration = Line.Generation;
            Record.FileEndOffset  = Line.EndOffset;
            Record.Attributes.push_back (PathAttribute);

            g_LogFifo.push (std::move (Record));
            g_Metric_FileLines.fetch_add (1, std::memory_order_relaxed);
        }

        g_Metric_FileOpen.store (g_FileReader.IsFileOpen() ? 1 : 0, std::memory_order_relaxed);

        if (0 == Lines)
            sleep_ms (OTELFWD_FILE_POLL_MS);
    }

    g_FileThreadRunning = 0;

    if (g_LogLevel)
        LogMessage ("File input thread ended");

    return NULL;
}


/* ---- Syslog input ----
   A UNIX datagram socket. One datagram is one syslog message, as NGINX sends it (RFC 3164 style):

       <PRI>Mmm dd hh:mm:ss [hostname ]tag: message

   The syslog part is decoded generically:
     PRI      the severity of the record (the facility is ignored)
     tag      the scope name of the record
     message  the body of the record, unchanged
   The time in the syslog header has no year, no time zone and only whole seconds. It is not used.
   The event time is the arrival time, unless the message carries its own time (see below).

   If the message is a JSON object, its members become attributes of the record, with their JSON types.
   The member "time" is not an attribute. It is the event time as seconds with a fraction, for example
   "1789769289.831" (the $msec variable of NGINX). The body is still the original message. */

struct SyslogMessage
{
    int         Pri = -1;
    std::string Hostname;
    std::string Tag;
    std::string Text;
};


bool IsSyslogTag (const std::string& Token)
{
    return ( (Token.size() > 1) && (':' == Token.back()) );
}


/* Parses <PRI>, the timestamp, an optional hostname and the tag. Without a tag the rest is the message */
bool ParseSyslogMessage (const std::string& Msg, SyslogMessage& retMsg)
{
    size_t      Pos    = 1;
    int         Pri    = 0;
    int         Digits = 0;
    std::string Rest;
    std::string Token1;
    std::string After;
    size_t      Sp     = 0;

    retMsg = SyslogMessage();

    if ( (Msg.size() < 4) || ('<' != Msg[0]) )
        return false;

    for (; (Pos < Msg.size()) && (Digits < 3) && (Msg[Pos] >= '0') && (Msg[Pos] <= '9'); Pos++, Digits++)
        Pri = (Pri * 10) + (Msg[Pos] - '0');

    if ( (0 == Digits) || (Pos >= Msg.size()) || ('>' != Msg[Pos]) || (Pri > 191) )
        return false;

    Pos++;
    retMsg.Pri = Pri;

    /* "Mmm dd hh:mm:ss " is 16 characters */
    if ( (Msg.size() >= (Pos + 16)) && isalpha (static_cast<unsigned char>(Msg[Pos])) &&
         (' ' == Msg[Pos+3]) && (':' == Msg[Pos+9]) && (':' == Msg[Pos+12]) && (' ' == Msg[Pos+15]) )
        Pos += 16;

    Rest = Msg.substr (Pos);

    while ( (false == Rest.empty()) && (('\n' == Rest.back()) || ('\r' == Rest.back())) )
        Rest.pop_back();

    Sp     = Rest.find (' ');
    Token1 = Rest.substr (0, Sp);
    After  = (std::string::npos == Sp) ? std::string() : Rest.substr (Sp + 1);

    if (IsSyslogTag (Token1))
    {
        retMsg.Tag  = Token1.substr (0, Token1.size() - 1);
        retMsg.Text = After;
    }
    else
    {
        size_t      Sp2    = After.find (' ');
        std::string Token2 = After.substr (0, Sp2);

        if (IsSyslogTag (Token2))
        {
            retMsg.Hostname = Token1;
            retMsg.Tag      = Token2.substr (0, Token2.size() - 1);
            retMsg.Text     = (std::string::npos == Sp2) ? std::string() : After.substr (Sp2 + 1);
        }
        else
        {
            retMsg.Text = Rest;
        }
    }

    /* A tag can carry a process id: tag[123]: */
    if ( (false == retMsg.Tag.empty()) && (']' == retMsg.Tag.back()) )
    {
        size_t Bracket = retMsg.Tag.find ('[');

        if ( (std::string::npos != Bracket) && (Bracket > 0) )
            retMsg.Tag.erase (Bracket);
    }

    return true;
}


/* Syslog severity (0 emerg ... 7 debug) to the OpenTelemetry severity number */
void GetSyslogSeverity (int Severity, int& retNumber, const char *& retpszText)
{
    switch (Severity)
    {
        case 0:  retNumber = 21; retpszText = "EMERG";  break;
        case 1:  retNumber = 21; retpszText = "ALERT";  break;
        case 2:  retNumber = 21; retpszText = "CRIT";   break;
        case 3:  retNumber = 17; retpszText = "ERROR";  break;
        case 4:  retNumber = 13; retpszText = "WARN";   break;
        case 5:  retNumber = 10; retpszText = "NOTICE"; break;
        case 6:  retNumber = 9;  retpszText = "INFO";   break;
        default: retNumber = 5;  retpszText = "DEBUG";  break;
    }
}


/* Seconds with an optional fraction, as string ("1789769289.831") or number, to nanoseconds */
bool GetUnixNanoFromSeconds (const rapidjson::Value& Val, int64_t& retNano)
{
    const uint64_t MaxSeconds = 9000000000ULL;
    uint64_t       Sec        = 0;
    uint64_t       Frac       = 0;
    int            FracDigits = 0;
    bool           bDigits    = false;

    if (Val.IsString())
    {
        const char *p = Val.GetString();

        for (; (*p >= '0') && (*p <= '9'); p++)
        {
            if (Sec > MaxSeconds)
                return false;

            Sec     = (Sec * 10) + static_cast<uint64_t>(*p - '0');
            bDigits = true;
        }

        if (false == bDigits)
            return false;

        if ('.' == *p)
        {
            for (p++; (*p >= '0') && (*p <= '9'); p++)
            {
                if (FracDigits < 9)
                {
                    Frac = (Frac * 10) + static_cast<uint64_t>(*p - '0');
                    FracDigits++;
                }
            }
        }

        if ( ('\0' != *p) || (Sec > MaxSeconds) )
            return false;

        for (; FracDigits < 9; FracDigits++)
            Frac *= 10;
    }
    else if (Val.IsNumber())
    {
        double d = Val.GetDouble();

        if ( (d <= 0) || (d > static_cast<double>(MaxSeconds)) )
            return false;

        Sec  = static_cast<uint64_t>(d);
        Frac = static_cast<uint64_t>((d - static_cast<double>(Sec)) * 1000000000.0);
    }
    else
    {
        return false;
    }

    retNano = static_cast<int64_t>((Sec * 1000000000ULL) + Frac);
    return true;
}


/* The resource of a syslog record: the default resource of the forwarder, with the service identity taken from the tag.
   The producer chooses its identity through the tag. The part before the first underscore is both the service name and the
   service namespace: nginx_access and nginx_error are "nginx", apache_access is "apache". A tag without an underscore is used
   as it is. Without a tag the default resource is used unchanged. The complete tag stays the scope name of the record.
   service.instance.id, host.name and os.type are the defaults of the forwarder. */
std::vector<OtelKV> GetSyslogResource (const std::string& Tag)
{
    std::vector<OtelKV> Resource = GetDefaultResource();
    std::string         Service  = Tag.substr (0, Tag.find ('_'));

    if (Service.empty())
        return Resource;

    for (OtelKV& KV : Resource)
    {
        if ( ("service.name" == KV.Key) || ("service.namespace" == KV.Key) )
            KV.StrValue = Service;
    }

    return Resource;
}


bool DecodeSyslogMessage (const std::string& Datagram, int64_t ArrivalNs, LogRecord& retRecord)
{
    SyslogMessage Msg;
    const char   *pszSeverityText = "";
    int           SeverityNumber  = 0;
    int64_t       TimeNs          = 0;

    retRecord = LogRecord();

    if ( (false == ParseSyslogMessage (Datagram, Msg)) || Msg.Text.empty() )
        return false;

    GetSyslogSeverity (Msg.Pri & 7, SeverityNumber, pszSeverityText);

    retRecord.Source         = LOG_SOURCE_SOCKET;
    retRecord.ObservedTimeNs = ArrivalNs;
    retRecord.Line           = Msg.Text;
    retRecord.ScopeName      = Msg.Tag;
    retRecord.Resource       = GetSyslogResource (Msg.Tag);
    retRecord.SeverityNumber = SeverityNumber;
    retRecord.SeverityText   = pszSeverityText;

    if ('{' == Msg.Text[0])
    {
        rapidjson::Document doc;

        doc.Parse (Msg.Text.c_str());

        if ( (false == doc.HasParseError()) && doc.IsObject() )
        {
            rapidjson::Value::ConstMemberIterator it = doc.FindMember ("time");

            if (it != doc.MemberEnd())
                GetUnixNanoFromSeconds (it->value, TimeNs);

            AddKVsFromJsonObject (doc, retRecord.Attributes);

            /* "time" is the event time and not an attribute. An empty string carries no information: skip it */
            retRecord.Attributes.erase (std::remove_if (retRecord.Attributes.begin(), retRecord.Attributes.end(),
                                        [] (const OtelKV& KV) { return ("time" == KV.Key) || ((OTEL_TYPE_STRING == KV.Type) && KV.StrValue.empty()); }), retRecord.Attributes.end());
        }
    }

    retRecord.TimeNs = TimeNs ? TimeNs : ArrivalNs;

    return true;
}


void SyslogHandleDatagram (IngestSource *pSource, const std::string& Datagram)
{
    LogRecord Record;

    if (false == DecodeSyslogMessage (Datagram, GetEpochNanoseconds(), Record))
    {
        pSource->Invalid.fetch_add (1, std::memory_order_relaxed);
        return;
    }

    if (g_LogFifo.try_push (std::move (Record), g_SocketQueueMax))
        pSource->Accepted.fetch_add (1, std::memory_order_relaxed);
    else
        pSource->Dropped.fetch_add (1, std::memory_order_relaxed);
}


/* One thread for the syslog datagram socket. There are no connections: every datagram is a message */
void *SyslogThread (void *arg)
{
    IngestSource *pSource = (IngestSource *) arg;
    char          szBuffer[65536];
    char          szMessage[256] = {0};
    sigset_t      SigSet;
    struct pollfd Poll {};
    int           rc = 0;

    sigemptyset (&SigSet);
    sigaddset (&SigSet, SIGINT);
    sigaddset (&SigSet, SIGTERM);
    sigaddset (&SigSet, SIGHUP);
    pthread_sigmask (SIG_BLOCK, &SigSet, NULL);

    if (g_LogLevel)
    {
        snprintf (szMessage, sizeof (szMessage), "%s input thread started", pSource->pszTitle);
        LogMessage (szMessage);
    }

    Poll.fd     = pSource->fdListen;
    Poll.events = POLLIN;

    while (0 == g_ShutdownRequested)
    {
        rc = poll (&Poll, 1, 1000);

        if (rc < 0)
        {
            if (EINTR == errno)
                continue;

            LogError ("poll failed", strerror (errno));
            break;
        }

        if (0 == rc)
            continue;

        /* Everything which is waiting. MSG_TRUNC returns the real size, so an oversized datagram is detected */
        for (int i = 0; (i < 1000) && (0 == g_ShutdownRequested); i++)
        {
            ssize_t nread = recv (pSource->fdListen, szBuffer, sizeof (szBuffer), MSG_DONTWAIT | MSG_TRUNC);

            if (nread < 0)
                break;

            if (0 == nread)
                continue;

            if (static_cast<size_t>(nread) > sizeof (szBuffer))
            {
                pSource->Invalid.fetch_add (1, std::memory_order_relaxed);
                continue;
            }

            SyslogHandleDatagram (pSource, std::string (szBuffer, static_cast<size_t>(nread)));
        }
    }

    close (pSource->fdListen);
    pSource->fdListen = -1;

    if (pSource->szPath[0])
        unlink (pSource->szPath);

    pSource->Running = 0;

    if (g_LogLevel)
    {
        snprintf (szMessage, sizeof (szMessage), "%s input thread ended", pSource->pszTitle);
        LogMessage (szMessage);
    }

    return NULL;
}


/* Creates the listening Unix socket. A stale socket file of an earlier run is removed.
   The socket is only accessible by its owner unless a mode is specified.
   SockType SOCK_DGRAM creates a datagram socket (syslog): one datagram is one message and there is no listen(). */
bool CreateUnixListener (IngestSource *pSource, const char *pszPath, mode_t Mode, int SockType = SOCK_STREAM)
{
    struct sockaddr_un Addr {};
    struct stat st {};
    int fd      = -1;
    int fdProbe = -1;
    int rc      = 0;
    mode_t OldMask = 0;

    if (strlen (pszPath) >= sizeof (Addr.sun_path))
    {
        LogError ("Unix socket path is too long", pszPath);
        return false;
    }

    Addr.sun_family = AF_UNIX;
    snprintf (Addr.sun_path, sizeof (Addr.sun_path), "%s", pszPath);

    if (0 == lstat (pszPath, &st))
    {
        if (false == S_ISSOCK (st.st_mode))
        {
            LogError ("Unix socket path exists and is not a socket", pszPath);
            return false;
        }

        /* Remove the socket only if nobody is listening on it. Otherwise another instance is running */
        fdProbe = socket (AF_UNIX, SockType | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

        if (fdProbe < 0)
        {
            LogError ("Cannot create socket", strerror (errno));
            return false;
        }

        rc = connect (fdProbe, (struct sockaddr *) &Addr, sizeof (Addr));

        if ( (0 == rc) || (EAGAIN == errno) || (EINPROGRESS == errno) )
        {
            close (fdProbe);
            LogError ("Unix socket is in use by another process", pszPath);
            return false;
        }

        if (ECONNREFUSED != errno)
        {
            LogError ("Cannot check existing Unix socket", strerror (errno));
            close (fdProbe);
            return false;
        }

        close (fdProbe);
        unlink (pszPath);
    }

    fd = socket (AF_UNIX, SockType | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (fd < 0)
    {
        LogError ("Cannot create socket", strerror (errno));
        return false;
    }

    /* Create the socket without any access for group and others, and set the requested mode afterwards.
       The umask is process wide. This is safe, because no other thread is running yet. */
    OldMask = umask (0177);
    rc = bind (fd, (struct sockaddr *) &Addr, sizeof (Addr));
    umask (OldMask);

    if (0 != rc)
    {
        {
            char szErr[1300] = {0};

            snprintf (szErr, sizeof (szErr), "%s: %s", pszPath, strerror (errno));
            LogError ("Cannot bind Unix socket", szErr);
        }
        close (fd);
        return false;
    }

    if ( (0 != chmod (pszPath, Mode)) || ((SOCK_STREAM == SockType) && (0 != listen (fd, 16))) )
    {
        {
            char szErr[1300] = {0};

            snprintf (szErr, sizeof (szErr), "%s: %s", pszPath, strerror (errno));
            LogError ("Cannot set up Unix socket", szErr);
        }
        close (fd);
        unlink (pszPath);
        return false;
    }

    /* How many datagrams can wait for this thread is not decided by the receive buffer of this socket. For UNIX datagram sockets
       the limits are net.unix.max_dgram_qlen (512 datagrams per receiving socket) and the send buffer of each sender (NGINX: about
       200 KB, roughly 170 to 400 log lines). Measured: SO_RCVBUF makes no difference. A sender which finds the socket full gets
       EAGAIN and loses the message. The input thread decodes every message before it reads the next one: a stall of the thread
       for longer than 512 messages take to arrive loses messages. */

    pSource->fdListen   = fd;
    pSource->bEnabled   = true;
    pSource->bDatagram  = (SOCK_DGRAM == SockType);
    snprintf (pSource->szPath, sizeof (pSource->szPath), "%s", pszPath);

    {
        char szMsg[1200] = {0};

        snprintf (szMsg, sizeof (szMsg), "Input %s: Unix %s socket %s", pSource->pszTitle, (SOCK_DGRAM == SockType) ? "datagram" : "stream", pszPath);
        LogMessage (szMsg);
    }

    return true;
}


/* Creates the listening TCP socket. There is no authentication and no encryption,
   so only loopback addresses (127.0.0.0/8, ::1) are accepted. Format: address:port or [address]:port */
bool CreateTcpListener (IngestSource *pSource, const char *pszSpec)
{
    std::string Spec = pszSpec;
    std::string Host;
    std::string PortText;
    struct sockaddr_storage Addr {};
    socklen_t AddrLen = 0;
    struct in_addr  Addr4 {};
    struct in6_addr Addr6 {};
    unsigned long Port = 0;
    char *pEnd = NULL;
    int fd  = -1;
    int opt = 1;

    size_t Colon = Spec.rfind (':');

    if (std::string::npos == Colon)
    {
        LogError ("TCP listen address needs the format address:port", pszSpec);
        return false;
    }

    Host     = Spec.substr (0, Colon);
    PortText = Spec.substr (Colon + 1);

    if ( (Host.size() >= 2) && ('[' == Host.front()) && (']' == Host.back()) )
        Host = Host.substr (1, Host.size() - 2);

    Port = strtoul (PortText.c_str(), &pEnd, 10);

    if ( PortText.empty() || (NULL == pEnd) || (*pEnd != '\0') || (0 == Port) || (Port > 65535) )
    {
        LogError ("Invalid TCP listen port", pszSpec);
        return false;
    }

    if (1 == inet_pton (AF_INET, Host.c_str(), &Addr4))
    {
        struct sockaddr_in *pAddr = (struct sockaddr_in *) &Addr;

        if (127 != (ntohl (Addr4.s_addr) >> 24))
        {
            LogError ("TCP input only accepts loopback addresses (127.0.0.0/8 or ::1)", pszSpec);
            return false;
        }

        pAddr->sin_family = AF_INET;
        pAddr->sin_port   = htons (static_cast<uint16_t>(Port));
        pAddr->sin_addr   = Addr4;
        AddrLen           = sizeof (struct sockaddr_in);
    }
    else if (1 == inet_pton (AF_INET6, Host.c_str(), &Addr6))
    {
        struct sockaddr_in6 *pAddr = (struct sockaddr_in6 *) &Addr;

        if (0 == IN6_IS_ADDR_LOOPBACK (&Addr6))
        {
            LogError ("TCP input only accepts loopback addresses (127.0.0.0/8 or ::1)", pszSpec);
            return false;
        }

        pAddr->sin6_family = AF_INET6;
        pAddr->sin6_port   = htons (static_cast<uint16_t>(Port));
        pAddr->sin6_addr   = Addr6;
        AddrLen            = sizeof (struct sockaddr_in6);
    }
    else
    {
        LogError ("TCP listen address must be an IP address", pszSpec);
        return false;
    }

    fd = socket (Addr.ss_family, SOCK_STREAM | SOCK_NONBLOCK | SOCK_CLOEXEC, 0);

    if (fd < 0)
    {
        LogError ("Cannot create socket", strerror (errno));
        return false;
    }

    setsockopt (fd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof (opt));

    if ( (0 != bind (fd, (struct sockaddr *) &Addr, AddrLen)) || (0 != listen (fd, 16)) )
    {
        LogError ("Cannot listen on TCP address", strerror (errno));
        close (fd);
        return false;
    }

    pSource->fdListen = fd;
    pSource->bEnabled = true;

    {
        char szMsg[512] = {0};

        snprintf (szMsg, sizeof (szMsg), "Input %s: TCP %s", pSource->pszTitle, pszSpec);
        LogMessage (szMsg);
    }

    return true;
}


bool WriteHelpAndType (FILE *fp, const char *pszStatName, const char *pszType, const char *pszDescription)
{
    if (NULL == fp)
        return false;

    if (IsNullStr (pszStatName))
        return false;

    if (IsNullStr (pszType))
        pszType = g_szPromTypeGauge;

    if (NULL == pszDescription)
        pszDescription = g_szEmpty;

    fprintf (fp, "# HELP %s_%s %s\n", g_szPromPrefix, pszStatName, pszDescription);
    fprintf (fp, "# TYPE %s_%s %s\n", g_szPromPrefix, pszStatName, pszType);

    return true;
}


bool WriteStatsEntryToFile (FILE *fp, uint64_t ValueNum, const char *pszStatName)
{
    if (NULL == fp)
        return false;

    if (NULL == pszStatName)
        return false;

    fprintf (fp, "%s_%s %zu\n", g_szPromPrefix, pszStatName, ValueNum);

    return true;
}

bool WriteStatsEntryToFileWithHelp (FILE *fp, uint64_t ValueNum, const char *pszStatName, const char *pszType, const char *pszDescription)
{
    if (NULL == fp)
        return false;

    if (NULL == pszStatName)
        return false;

    WriteHelpAndType (fp, pszStatName, pszType, pszDescription);
    fprintf (fp, "%s_%s %zu\n", g_szPromPrefix, pszStatName, ValueNum);

    return true;
}


/* Socket input metrics are only written for inputs that are enabled */
void WriteIngestMetrics (FILE *fp, IngestSource *pSource)
{
    char szName[128] = {0};

    if (false == pSource->bEnabled)
        return;

    snprintf (szName, sizeof (szName), "socket_lines_total{source=\"%s\",result=\"accepted\"}", pSource->pszName);
    WriteStatsEntryToFile (fp, pSource->Accepted.load (std::memory_order_relaxed), szName);

    snprintf (szName, sizeof (szName), "socket_lines_total{source=\"%s\",result=\"dropped\"}", pSource->pszName);
    WriteStatsEntryToFile (fp, pSource->Dropped.load (std::memory_order_relaxed), szName);

    snprintf (szName, sizeof (szName), "socket_lines_total{source=\"%s\",result=\"invalid\"}", pSource->pszName);
    WriteStatsEntryToFile (fp, pSource->Invalid.load (std::memory_order_relaxed), szName);
}


void WriteIngestConnectionMetrics (FILE *fp, IngestSource *pSource)
{
    char szName[128] = {0};

    if (false == pSource->bEnabled)
        return;

    snprintf (szName, sizeof (szName), "socket_connections_total{source=\"%s\",result=\"accepted\"}", pSource->pszName);
    WriteStatsEntryToFile (fp, pSource->Connections.load (std::memory_order_relaxed), szName);

    snprintf (szName, sizeof (szName), "socket_connections_total{source=\"%s\",result=\"rejected\"}", pSource->pszName);
    WriteStatsEntryToFile (fp, pSource->Rejected.load (std::memory_order_relaxed), szName);
}


/* Health for alerting: gives what is known now to the monitor, see health.hpp. Called by the metrics thread */
void UpdateHealth()
{
    static int64_t Success   = 0;
    static int64_t Errors    = 0;
    static bool    bFailing  = false;

    HealthInput Input;
    int64_t     NewSuccess = g_Metric_PushSuccess.load (std::memory_order_relaxed) + g_Metric_PushRetrySuccess.load (std::memory_order_relaxed);
    int64_t     NewErrors  = g_Metric_PushErrors.load  (std::memory_order_relaxed) + g_Metric_PushRetryErrors.load  (std::memory_order_relaxed);

    /* Without a push nothing is known: the last result stays. A push which was accepted ends the failure, also when others failed */
    if (NewSuccess > Success)
        bFailing = false;
    else if (NewErrors > Errors)
        bFailing = true;

    Success = NewSuccess;
    Errors  = NewErrors;

    Input.bUnreachable = bFailing;
    Input.bWalFailed   = (false == IsNullStr (g_szOtlpPushApiURL)) && (false == g_bWalOpened);
    Input.WalBytes     = g_bWalOpened ? g_Wal.GetSize() : 0;
    Input.WalMaxBytes  = static_cast<uint64_t> (g_WalMaxMB) * 1024 * 1024;
    Input.Dropped      = static_cast<uint64_t> (g_Metric_WalRefused.load (std::memory_order_relaxed));
    Input.Rejected     = static_cast<uint64_t> (g_Metric_PushRejected.load (std::memory_order_relaxed));

    g_Health.Update (time (NULL), Input, [] (const char *pszMessage) { LogMessage (pszMessage); });
}


bool WriteMetrics (bool bShutdown = false)
{
    char    szTempFilename[2200] = {0};
    char    szTmp[1024]          = {0};
    FILE    *fp = NULL;
    time_t  tNow = time(NULL);

    if (IsNullStr (g_szMetricsFileName))
        return false;

    snprintf (szTempFilename, sizeof (szTempFilename), "%s.tmp", g_szMetricsFileName);

    fp = fopen (szTempFilename, "w");

    if (NULL == fp)
    {
        return false;
    }

    snprintf (szTmp, sizeof (szTmp), "otelfwd Build Version %s", g_szVersion);
    WriteStatsEntryToFileWithHelp (fp, OTELFWD_VERSION_BUILD, "build_number", g_szPromTypeGauge, szTmp);

    WriteStatsEntryToFileWithHelp (fp, g_tStartTime, "started_timestamp_seconds", g_szPromTypeGauge, "Unix timestamp when forwarder was started");
    WriteStatsEntryToFileWithHelp (fp, tNow - g_tStartTime, "uptime_seconds", g_szPromTypeGauge, "Uptime in seconds");

    WriteStatsEntryToFileWithHelp (fp, time(NULL), "lastupdate_timestamp_seconds", g_szPromTypeGauge, "Unix timestamp for last metrics update");
    WriteStatsEntryToFileWithHelp (fp, g_Metric_LogLines.load (std::memory_order_relaxed), "lines_received_total", g_szPromTypeGauge, "Total number of log lines received by forwarder");

    WriteHelpAndType      (fp, "push_total", g_szPromTypeCounter, "Total number of log lines pushed to the destination, labeled by result");
    WriteStatsEntryToFile (fp, g_Metric_PushSuccess.load (std::memory_order_relaxed),      "push_total{result=\"success\"}");
    WriteStatsEntryToFile (fp, g_Metric_PushErrors.load (std::memory_order_relaxed),       "push_total{result=\"error\"}");

    WriteHelpAndType      (fp, "push_retry_total", g_szPromTypeCounter, "Total number of WAL records (one push request each) replayed to the destination, labeled by result");
    WriteStatsEntryToFile (fp, g_Metric_PushRetrySuccess.load (std::memory_order_relaxed), "push_retry_total{result=\"success\"}");
    WriteStatsEntryToFile (fp, g_Metric_PushRetryErrors.load (std::memory_order_relaxed),  "push_retry_total{result=\"error\"}");

    WriteStatsEntryToFileWithHelp (fp, g_Metric_PushRejected.load (std::memory_order_relaxed), "push_rejected_total", g_szPromTypeCounter, "Total number of push requests which are dropped for good, not kept in the WAL: the receiver refused them as bad data (HTTP 400), or they could not be converted to protobuf");

    /* Only with a file input */
    if (g_bFileInputEnabled)
    {
        WriteStatsEntryToFileWithHelp (fp, static_cast<uint64_t> (g_Metric_FileLines.load (std::memory_order_relaxed)), "file_lines_total", g_szPromTypeCounter, "Total number of lines read from the file input and queued (empty lines are skipped)");
        WriteStatsEntryToFileWithHelp (fp, static_cast<uint64_t> (g_Metric_FileTruncated.load (std::memory_order_relaxed)), "file_lines_truncated_total", g_szPromTypeCounter, "Total number of lines of the file input which were longer than 1 MB and were cut");
        WriteStatsEntryToFileWithHelp (fp, static_cast<uint64_t> (g_Metric_FileCommitted.load (std::memory_order_relaxed)), "file_committed_offset_bytes", g_szPromTypeGauge, "Position in the file up to which the lines were delivered (accepted, or kept in the WAL). It starts again at 0 when the file is rotated or truncated");
        WriteStatsEntryToFileWithHelp (fp, static_cast<uint64_t> (g_Metric_FileOpen.load (std::memory_order_relaxed)), "file_open", g_szPromTypeGauge, "1 if the file of the file input is open, 0 if it does not exist");
    }

    /* Only with a push target, which is where a WAL is used */
    if (false == IsNullStr (g_szOtlpPushApiURL))
    {
        WriteStatsEntryToFileWithHelp (fp, static_cast<uint64_t> (g_Metric_WalRefused.load (std::memory_order_relaxed)), "wal_refused_total", g_szPromTypeCounter, "Total number of push requests which the WAL did not take (it was full, or a failure). They are lost");
        WriteStatsEntryToFileWithHelp (fp, static_cast<uint64_t> (g_bWalOpened ? g_Wal.GetSize() : 0), "wal_bytes", g_szPromTypeGauge, "Size of the WAL: push requests which wait on disk for the receiver");
    }

    WriteStatsEntryToFileWithHelp (fp, static_cast<uint64_t> (g_Health.GetState()), "health", g_szPromTypeGauge, "Health for alerting: 0 is OK, 1 is a warning, 2 is an error");

    /* Only with protobuf. These are also counted in push_rejected_total */
    if (g_bPushProtobuf)
        WriteStatsEntryToFileWithHelp (fp, g_Metric_PushConvertErrors.load (std::memory_order_relaxed), "push_convert_errors_total", g_szPromTypeCounter, "Total number of push requests which could not be converted from JSON to protobuf. They are dropped, and counted in push_rejected_total too");

    /* Only with a backup endpoint */
    if (g_PushFailover.HasBackup())
    {
        WriteStatsEntryToFileWithHelp (fp, static_cast<uint64_t> (g_PushFailover.GetActive()), "push_endpoint_active", g_szPromTypeGauge, "OTLP endpoint in use: 0 is the primary, 1 is the backup");

        WriteHelpAndType      (fp, "push_endpoint_requests_total", g_szPromTypeCounter, "Total number of push requests to an OTLP endpoint, labeled by endpoint and result (accepted, retry, rejected). A try of the primary while the backup is in use counts too");
        WriteStatsEntryToFile (fp, g_PushFailover.GetRequests (PUSH_PRIMARY, PUSH_ACCEPTED), "push_endpoint_requests_total{endpoint=\"primary\",result=\"accepted\"}");
        WriteStatsEntryToFile (fp, g_PushFailover.GetRequests (PUSH_PRIMARY, PUSH_RETRY),    "push_endpoint_requests_total{endpoint=\"primary\",result=\"retry\"}");
        WriteStatsEntryToFile (fp, g_PushFailover.GetRequests (PUSH_PRIMARY, PUSH_REJECTED), "push_endpoint_requests_total{endpoint=\"primary\",result=\"rejected\"}");
        WriteStatsEntryToFile (fp, g_PushFailover.GetRequests (PUSH_BACKUP,  PUSH_ACCEPTED), "push_endpoint_requests_total{endpoint=\"backup\",result=\"accepted\"}");
        WriteStatsEntryToFile (fp, g_PushFailover.GetRequests (PUSH_BACKUP,  PUSH_RETRY),    "push_endpoint_requests_total{endpoint=\"backup\",result=\"retry\"}");
        WriteStatsEntryToFile (fp, g_PushFailover.GetRequests (PUSH_BACKUP,  PUSH_REJECTED), "push_endpoint_requests_total{endpoint=\"backup\",result=\"rejected\"}");

        WriteStatsEntryToFileWithHelp (fp, g_PushFailover.GetFailovers(), "push_failovers_total", g_szPromTypeCounter, "Number of times the backup endpoint was taken into use because the primary failed");
        WriteStatsEntryToFileWithHelp (fp, g_PushFailover.GetFailbacks(), "push_failbacks_total", g_szPromTypeCounter, "Number of times the primary endpoint was taken into use again");
    }

    if (g_IngestUnix.bEnabled || g_IngestTcp.bEnabled || g_IngestSyslog.bEnabled)
    {
        WriteHelpAndType (fp, "socket_lines_total", g_szPromTypeCounter, "Total number of lines received on socket inputs, labeled by source and result");
        WriteIngestMetrics (fp, &g_IngestUnix);
        WriteIngestMetrics (fp, &g_IngestTcp);
        WriteIngestMetrics (fp, &g_IngestSyslog);

        /* The syslog input is a datagram socket and has no connections */
        if (g_IngestUnix.bEnabled || g_IngestTcp.bEnabled)
        {
            WriteHelpAndType (fp, "socket_connections_total", g_szPromTypeCounter, "Total number of connections on socket inputs, labeled by source and result");
            WriteIngestConnectionMetrics (fp, &g_IngestUnix);
            WriteIngestConnectionMetrics (fp, &g_IngestTcp);
        }
    }

    if (bShutdown)
    {
        WriteStatsEntryToFileWithHelp (fp, time(NULL), "shutdown_timestamp_seconds", g_szPromTypeGauge, "Unix timestamp when forwarder was shutdown");
    }

    if (fp)
    {
        fclose (fp);
        fp = NULL;

        rename (szTempFilename, g_szMetricsFileName);
    }

    return true;
}

void *MetricsThread (void *arg)
{
    (void)arg;

    g_MetricsThreadRunning = 1;

    if (g_LogLevel)
        LogMessage ("Metrics Thread started");

    while (true)
    {
        if (IdleDelay (10))
            break;

        UpdateHealth();
        WriteMetrics();
    }

    g_MetricsThreadRunning = 0;

    if (g_LogLevel)
        LogMessage ("Metrics Thread ended");

    return NULL;
}


void handle_signal (int sig)
{
    if (sig == SIGINT || sig == SIGTERM)
    {
        LogMessage ("Shutdown requested");
        g_ShutdownRequested = 1;
    }

    else if (sig == SIGHUP)
    {
        LogMessage ("Configuration reload requested");
        g_ReloadRequested = 1;
    }
}

size_t GetEnvironmentValue (const char *pszEnvironmentName)
{
    char *p = NULL;

    if (IsNullStr (pszEnvironmentName))
        return 0;

    p = getenv (pszEnvironmentName);

    if (NULL == p)
        return 0;

    return atoi (p);
}


/* A URL for the log and for -cfg: without the user name and password, and without the query and the fragment. A URL can carry
   a secret there (https://user:password@host/path?token=...). Nothing else is changed */
std::string SanitizeUrlForLog (const char *pszURL)
{
    std::string Url = pszURL ? pszURL : "";
    size_t Cut = Url.find_first_of ("?#");

    if (std::string::npos != Cut)
        Url.erase (Cut);

    size_t Scheme    = Url.find ("://");
    size_t AuthStart = (std::string::npos == Scheme) ? 0 : Scheme + 3;
    size_t AuthEnd   = Url.find ('/', AuthStart);

    if (std::string::npos == AuthEnd)
        AuthEnd = Url.size();

    /* The user information ends at the last @ before the first slash. An @ in the path stays */
    size_t At = Url.rfind ('@', (AuthEnd > 0) ? (AuthEnd - 1) : 0);

    if ( (std::string::npos != At) && (At >= AuthStart) && (At < AuthEnd) )
        Url.erase (AuthStart, At + 1 - AuthStart);

    return Url;
}


/* Without any configured socket input otelfwd listens on the default Unix socket of the Domino add-in (domfwd), so that neither side
   needs a setting. Without a push target there is nothing to forward: no default listener then, unless standalone mode, which reports
   the missing target as an error. The file input does not change that: it is one more input, and it does not stop the other listeners.
   One rule for main, for the summary at start and for -cfg */
static bool UseDefaultUnixSocket()
{
    return IsNullStr (g_szUnixSocketPath) && IsNullStr (g_szTcpListen) && IsNullStr (g_szSyslogSocketPath) && (g_NoStdin || (false == IsNullStr (g_szOtlpPushApiURL)));
}


static std::string GetDefaultUnixSocketPath()
{
    return std::string (g_szDataDir) + "/domino/otelfwd.sock";
}


/* One line of the summary at start: "Name: value". The names are those of -cfg */
static void LogSetting (const char *pszName, const std::string& Value)
{
    LogMessage ((std::string (pszName) + ": " + Value).c_str());
}


/* Logs the configuration which is in use, once at every start, one setting for each line as "Name: value" (the names of -cfg).
   This is where to look first when logs do not arrive where they are expected. The effective values are shown, after the checks
   of the configuration. No secrets: the token is only shown as set or not set, and the URLs are without user name, password
   and query. -cfg shows everything on request */
void LogStartupSummary (bool bWalOpened)
{
    LogSetting ("STDIN input",  g_NoStdin ? "no (-nostdin)" : "yes");
    LogSetting ("Data Dir",     g_szDataDir);
    LogSetting ("Metrics File", g_szMetricsFileName);

    if (IsNullStr (g_szOtlpPushApiURL))
    {
        LogSetting ("OTLP Push API URL", "not set. OTLP push is off, the log lines are not pushed");
    }
    else
    {
        LogSetting ("OTLP Push API URL", SanitizeUrlForLog (g_szOtlpPushApiURL));

        /* The format is for the primary and the backup endpoint */
        LogSetting ("OTLP Push Encoding", std::string (GetPushEncodingName()) + " (Content-Type " + (g_bPushProtobuf ? OTELFWD_CONTENT_TYPE_PROTOBUF : OTELFWD_CONTENT_TYPE_JSON) + ")");

        LogSetting ("OTLP Push Token", IsNullStr (g_szOtlpPushToken) ? "not set" : "set");
        LogSetting ("OTLP CA File",    IsNullStr (g_szOtlpCaFile) ? "not set" : g_szOtlpCaFile);

        if (false == IsNullStr (g_szOtlpPushApiURLBackup))
        {
            LogSetting ("OTLP Push API URL Backup", SanitizeUrlForLog (g_szOtlpPushApiURLBackup));
            LogSetting ("OTLP Push Failback sec",   std::to_string (static_cast<unsigned long> (g_FailbackSec)));
        }

        if (bWalOpened)
        {
            struct stat WalStat {};

            LogSetting ("WAL File", g_szWalFile);

            if ( (0 == stat (g_szWalFile, &WalStat)) && (WalStat.st_size > 0) )
                LogSetting ("WAL Pending", std::to_string (static_cast<long long> (WalStat.st_size)) + " bytes of an earlier run, will be replayed");

            LogSetting ("OTLP Push WAL Retry sec", std::to_string (static_cast<unsigned long> (g_WalRetrySec)));

            if (0 == g_WalMaxMB)
                LogSetting ("OTLP Push WAL Max MB", "0 (no limit)");
            else
                LogSetting ("OTLP Push WAL Max MB", std::to_string (static_cast<unsigned long> (g_WalMaxMB)));
        }
        else
        {
            LogSetting ("WAL File", std::string (g_szWalFile) + " (cannot be opened, failed pushes cannot be kept)");
        }
    }

    LogSetting ("OTLP Service Name",      g_szOtlpServiceName);
    LogSetting ("OTLP Service Namespace", g_szServiceNamespace);
    LogSetting ("OTLP Service Instance",  g_szServiceInstanceId);

    /* The inputs which listen: what is configured, or the default socket. They need a push target: without one they are reported and
       disabled later */
    if (false == IsNullStr (g_szUnixSocketPath))
        LogSetting ("Unix Socket", g_szUnixSocketPath);
    else if (UseDefaultUnixSocket())
        LogSetting ("Unix Socket", GetDefaultUnixSocketPath() + " (default)");

    if (false == IsNullStr (g_szTcpListen))
        LogSetting ("TCP Listen", g_szTcpListen);

    if (false == IsNullStr (g_szSyslogSocketPath))
        LogSetting ("Syslog Socket", g_szSyslogSocketPath);

    if (false == IsNullStr (g_szFileInput))
    {
        LogSetting ("File Input",      g_bFileInputEnabled ? std::string (g_szFileInput) : std::string (g_szFileInput) + " (not used: it needs OTLP_PUSH_API_URL)");
        LogSetting ("File Start",      g_bFileStartAtEnd ? "end (without a state file only the lines which are written later)" : "begin");
        LogSetting ("File Severity",   (0 == g_FileSeverityNumber) ? "off (the lines have no severity)" : g_szFileSeverityText);
        LogSetting ("File Service",    IsNullStr (g_szFileService) ? std::string ("not set (service.name is ") + g_szOtlpServiceName + ")" : std::string (g_szFileService));
        LogSetting ("File State File", g_szFileStateFile);
    }

    if (0 == g_NoStdin)
    {
        if (false == IsNullStr (g_szOutputLogFile))
            LogSetting ("Output log", g_szOutputLogFile);

        if (g_Mirror2Stdout)
            LogSetting ("Mirror to stdout", "yes");
    }
}


/* Checks OTLP_PUSH_ENCODING at start. The value was read before (see main): an invalid one is reported, and json is used.
   It must not end the program: in pipe mode the server would write into a closed pipe */
void ValidatePushEncoding()
{
    const char *pEncoding = getenv (g_szEnvOtlpPushEncoding);
    bool        bProtobuf = false;
    char        szMessage[300] = {0};

    if ( (NULL == pEncoding) || ('\0' == *pEncoding) )
        return;

    if (false == ParsePushEncoding (pEncoding, bProtobuf))
    {
        snprintf (szMessage, sizeof (szMessage), "%s has to be json or protobuf. Using json", g_szEnvOtlpPushEncoding);
        LogError (szMessage, pEncoding);
    }
}


/* Checks OTLP_PUSH_WAL_RETRY_SEC at start. The value was read before (see main): an invalid one is reported, and the default is used */
void ValidateWalRetryConfig()
{
    const char *pValue = getenv (g_szEnvOtlpPushWalRetrySec);
    size_t      Seconds = 0;
    char        szMessage[300] = {0};

    if ( (NULL == pValue) || ('\0' == *pValue) )
        return;

    if (false == ParseSecondsSetting (pValue, OTELFWD_MAX_WAL_RETRY_SEC, Seconds))
    {
        snprintf (szMessage, sizeof (szMessage), "%s has to be a number of seconds from 1 to %d. Using the default of %d seconds",
                  g_szEnvOtlpPushWalRetrySec, OTELFWD_MAX_WAL_RETRY_SEC, OTELFWD_DEFAULT_WAL_RETRY_SEC);
        LogError (szMessage, pValue);
    }
}


/* Checks OTLP_PUSH_WAL_MAX_MB at start. The value was read before (see main): an invalid one is reported, and the default is used */
void ValidateWalMaxConfig()
{
    const char *pValue = getenv (g_szEnvOtlpPushWalMaxMB);
    size_t      Megabytes = 0;
    char        szMessage[300] = {0};

    if ( (NULL == pValue) || ('\0' == *pValue) )
        return;

    if (false == ParseMegabytesSetting (pValue, OTELFWD_MAX_WAL_MAX_MB, Megabytes))
    {
        snprintf (szMessage, sizeof (szMessage), "%s has to be a number of MB from 0 (no limit) to %d. Using the default of %d MB",
                  g_szEnvOtlpPushWalMaxMB, OTELFWD_MAX_WAL_MAX_MB, OTELFWD_DEFAULT_WAL_MAX_MB);
        LogError (szMessage, pValue);
    }
}


/* Checks OTELFWD_FILE_START and OTELFWD_FILE_SEVERITY at start. The values were read before (see main): an invalid one is reported,
   and the default is used (begin, info) */
void ValidateFileConfig()
{
    const char *pValue = getenv (g_szEnvFileStart);
    bool        bAtEnd = false;
    int         Number = 0;
    std::string Text;
    char        szMessage[300] = {0};

    if ( pValue && *pValue && (false == ParseFileStart (pValue, bAtEnd)) )
    {
        snprintf (szMessage, sizeof (szMessage), "%s has to be begin or end. Using begin", g_szEnvFileStart);
        LogError (szMessage, pValue);
    }

    pValue = getenv (g_szEnvFileSeverity);

    if ( pValue && *pValue && (false == FileSeverity::Parse (pValue, Number, Text)) )
    {
        snprintf (szMessage, sizeof (szMessage), "%s has to be trace, debug, info, warn, error, fatal or off. Using info", g_szEnvFileSeverity);
        LogError (szMessage, pValue);
    }
}


/* Checks the backup endpoint and the failback interval at start. A configuration which cannot work is reported and not used.
   It must not end the program: in pipe mode the server would write into a closed pipe */
void ValidateBackupConfig()
{
    const char *pFailback = getenv (g_szEnvOtlpPushFailbackSec);
    char szMessage[300]   = {0};

    if (pFailback && *pFailback)
    {
        char *pEnd  = NULL;
        long  Value = strtol (pFailback, &pEnd, 10);

        if ( ('\0' != *pEnd) || (Value < 1) || (Value > OTELFWD_MAX_FAILBACK_SEC) )
        {
            snprintf (szMessage, sizeof (szMessage), "%s has to be a number of seconds from 1 to %d. Using the default of %d seconds",
                      g_szEnvOtlpPushFailbackSec, OTELFWD_MAX_FAILBACK_SEC, OTELFWD_DEFAULT_FAILBACK_SEC);
            LogError (szMessage, pFailback);
        }
    }

    if (IsNullStr (g_szOtlpPushApiURLBackup))
    {
        if (pFailback && *pFailback)
        {
            snprintf (szMessage, sizeof (szMessage), "Warning: %s has no effect without %s", g_szEnvOtlpPushFailbackSec, g_szEnvOtlpPushApiUrlBackup);
            LogMessage (szMessage);
        }

        return;
    }

    if (IsNullStr (g_szOtlpPushApiURL))
    {
        snprintf (szMessage, sizeof (szMessage), "%s needs %s. The backup endpoint is not used", g_szEnvOtlpPushApiUrlBackup, g_szEnvOtlpPushApiUrl);
        LogError (szMessage);
        g_szOtlpPushApiURLBackup[0] = '\0';
        return;
    }

    if ( (0 != strncasecmp (g_szOtlpPushApiURLBackup, "http://", 7)) && (0 != strncasecmp (g_szOtlpPushApiURLBackup, "https://", 8)) )
    {
        snprintf (szMessage, sizeof (szMessage), "%s has to start with http:// or https://. The backup endpoint is not used", g_szEnvOtlpPushApiUrlBackup);
        LogError (szMessage, g_szOtlpPushApiURLBackup);
        g_szOtlpPushApiURLBackup[0] = '\0';
        return;
    }

    if (0 == strcmp (g_szOtlpPushApiURLBackup, g_szOtlpPushApiURL))
    {
        snprintf (szMessage, sizeof (szMessage), "Warning: %s is the same as %s. The backup endpoint is not used", g_szEnvOtlpPushApiUrlBackup, g_szEnvOtlpPushApiUrl);
        LogMessage (szMessage);
        g_szOtlpPushApiURLBackup[0] = '\0';
        return;
    }

    /* A valid backup endpoint is logged with the rest of the configuration, see LogStartupSummary */
}


void PrintBanner()
{
    fprintf (stderr, "\notelfwd %s - %s %s\n\n", g_szVersion, g_szCopyright, g_szGitHubURL);
}


/* A list of rows with two columns, printed with the first column as wide as its longest entry.
   Used for the help and the configuration output, so a longer name never breaks the alignment.
   AddText adds a line without columns (a heading, or an empty line). */
class AlignedList
{

public:

    void Add (const std::string& Left, const std::string& Right)
    {
        m_Rows.push_back (Row {Left, Right, true});
    }

    void AddText (const std::string& Text)
    {
        m_Rows.push_back (Row {Text, "", false});
    }

    void Print (const char *pszSeparator)
    {
        size_t Width = 0;

        for (const Row& R : m_Rows)
        {
            if (R.bColumns)
                Width = std::max (Width, R.Left.size());
        }

        for (const Row& R : m_Rows)
        {
            if (R.bColumns)
                printf ("%-*s%s%s\n", static_cast<int>(Width), R.Left.c_str(), pszSeparator, R.Right.c_str());
            else
                printf ("%s\n", R.Left.c_str());
        }

        m_Rows.clear();
    }

private:

    struct Row
    {
        std::string Left;
        std::string Right;
        bool        bColumns;
    };

    std::vector<Row> m_Rows;
};

AlignedList g_List;


void LogHelpEnv (const char *pszParameter, const char *pszDescription)
{
    if (IsNullStr (pszParameter))
        return;

    if (IsNullStr (pszDescription))
        return;

    g_List.Add (pszParameter, pszDescription);
}

void LogHelpCmd (const char *pszCmd, const char *pszDescription)
{
    if (IsNullStr (pszCmd))
        return;

    if (IsNullStr (pszDescription))
        return;

    g_List.Add (pszCmd, pszDescription);
}


void PrintHelp ()
{
    printf ("\n");
    printf ("otelfwd - Log Forwarder for OpenTelemetry\n");
    printf ("--------------------------------------------\n");
    printf ("\n");

    g_List.AddText ("Environment Variables:");
    g_List.AddText ("");

    LogHelpEnv (g_szEnvDataDir,               "Data directory for WAL and metrics, has to be writable (default: /local/notesdata)");
    LogHelpEnv (g_szEnvMirrorToStdout,        "Mirror stdin to stdout");
    LogHelpEnv (g_szEnvShutdownMaxSec,        "Shutdown max wait seconds (default: 30 sec)");
    LogHelpEnv (g_szEnvOutputLog,             "Output log file name");
    LogHelpEnv (g_szEnvPromFile,              "Prom File for Metrics output (default: <notesdata>/domino/stats/otelfwd.prom)");
    LogHelpEnv (g_szEnvHostname,              "Hostname to use (default: hostname read from OS)");
    LogHelpEnv (g_szEnvLogLevel,              "Log level for stdout logging");

    g_List.AddText ("");

    LogHelpEnv (g_szEnvOtlpPushApiUrl,        "OTLP/HTTP logs push URL (example: https://otel.example.com:4318/v1/logs)");
    LogHelpEnv (g_szEnvOtlpPushApiUrlBackup,  "Optional backup OTLP/HTTP logs push URL, used when the push URL fails. Same token and CA file");
    LogHelpEnv (g_szEnvOtlpPushFailbackSec,   "Seconds between attempts to use the push URL again while the backup is in use (default: 60 sec)");
    LogHelpEnv (g_szEnvOtlpPushWalRetrySec,   "Seconds to wait before the WAL is sent again after the receiver did not accept it (default: 60 sec)");
    LogHelpEnv (g_szEnvOtlpPushWalMaxMB,      "Largest size of the WAL in MB. Pushes which do not fit are dropped. 0 is no limit (default: 128 MB)");
    LogHelpEnv (g_szEnvOtlpPushEncoding,     "Format of the push requests: json or protobuf (default: json). Use protobuf for receivers which do not take JSON, like VictoriaLogs. The WAL is JSON either way");
    LogHelpEnv (g_szEnvOtlpPushToken,         "OTLP Push Token (bearer token)");
    LogHelpEnv (g_szEnvOtlpCaFile,            "OTLP Trusted Root CA File");
    LogHelpEnv (g_szEnvOtlpServiceName,       "OTLP service.name (default: domino)");
    LogHelpEnv (g_szEnvOtlpServiceNamespace,  "OTLP service.namespace (default: domino)");
    LogHelpEnv (g_szEnvOtlpServiceInstanceId, "OTLP service.instance.id (default: hostname)");

    g_List.AddText ("");

    LogHelpEnv (g_szEnvUnixSocket,            "Unix socket to receive records (flat record format, one JSON object per line). Default if OTLP_PUSH_API_URL is set and no socket input is configured: <notesdata>/domino/otelfwd.sock");
    LogHelpEnv (g_szEnvUnixSocketMode,        "Unix socket file mode in octal (default: 0600)");
    LogHelpEnv (g_szEnvTcpListen,             "Loopback TCP address to receive records (example: 127.0.0.1:4390)");
    LogHelpEnv (g_szEnvSyslogSocket,          "Unix datagram socket to receive syslog messages, for example from NGINX (only enabled if set)");
    LogHelpEnv (g_szEnvSyslogSocketMode,      "Syslog socket file mode in octal (default: 0600)");
    LogHelpEnv (g_szEnvSocketQueueMax,        "Max queued records before socket input drops records (default: 100000). The file input reads only while the queue has room");

    g_List.AddText ("");

    LogHelpEnv (g_szEnvFileInput,             "File to follow (like tail -F), one log record for every line. Needs OTLP_PUSH_API_URL. Rotation, truncation and restarts are handled");
    LogHelpEnv (g_szEnvFileStart,             "Where to start without a state file: begin (default) or end");
    LogHelpEnv (g_szEnvFileStateDir,          "Directory for the state file of the file input (default: next to the file, as <file>.otelfwd-state)");
    LogHelpEnv (g_szEnvFileSeverity,          "Severity of every line of the file input: trace, debug, info, warn, error, fatal or off for none (default: info)");
    LogHelpEnv (g_szEnvFileService,           "service.name and service.namespace of the lines of the file input (default: OTLP_SERVICE_NAME and OTLP_SERVICE_NAMESPACE)");

    g_List.AddText ("");
    g_List.AddText ("Command Line :");
    g_List.AddText ("");

    LogHelpCmd ("-nostdin",    "Do not read STDIN. Only the socket inputs are used, until SIGTERM/SIGINT");
    LogHelpCmd ("-cfg",        "Print configuration");
    LogHelpCmd ("-env",        "Print environment variable config");
    LogHelpCmd ("-help/-h/-?", "Print print help");

    g_List.AddText ("");

    g_List.Print ("  ");
}


void LogCfgText (bool bShowEnvVars, const char *pszDescription, const char *pszValue, const char *pszParameter = "")
{

    if (IsNullStr (pszDescription))
        return;

    if (NULL == (pszValue))
        return;

    if (bShowEnvVars)
    {
        if (IsNullStr (pszParameter))
            return;

        g_List.Add (pszParameter, pszValue);
    }
    else
    {
        g_List.Add (pszDescription, pszValue);
    }
}


void LogCfgNum (bool bShowEnvVars, const char *pszDescription, size_t Value, const char *pszParameter = "")
{
    if (IsNullStr (pszDescription))
        return;

    if (bShowEnvVars)
    {
        if (IsNullStr (pszParameter))
            return;

        g_List.Add (pszParameter, std::to_string (Value));
    }
    else
    {
        g_List.Add (pszDescription, std::to_string (Value));
    }
}


void DumpConfig (bool bShowEnvVars = false)
{
    char szMode[16] = {0};

    printf ("\n");
    printf ("otelfwd Configuration\n");
    printf ("---------------------\n");
    printf ("\n");

    LogCfgText (bShowEnvVars, "STDIN input",             g_NoStdin ? "no (-nostdin)" : "yes");
    LogCfgNum  (bShowEnvVars, "Mirror to stdout",        g_Mirror2Stdout,      g_szEnvMirrorToStdout);
    LogCfgNum  (bShowEnvVars, "Shutdown max wait sec",   g_ShutdownMaxWaitSec, g_szEnvShutdownMaxSec);

    g_List.AddText ("");

    LogCfgText (bShowEnvVars, "Data Dir",                g_szDataDir,          g_szEnvDataDir);
    LogCfgText (bShowEnvVars, "Output log",              g_szOutputLogFile,    g_szEnvOutputLog);
    LogCfgText (bShowEnvVars, "Metrics File",            g_szMetricsFileName,  g_szEnvPromFile);
    LogCfgText (bShowEnvVars, "Hostname",                g_szHostname,         g_szEnvHostname);
    LogCfgNum  (bShowEnvVars, "LogLevel",                g_LogLevel,           g_szEnvLogLevel);

    g_List.AddText ("");

    /* Without user name, password and query: a URL can carry a secret there */
    LogCfgText (bShowEnvVars, "OTLP Push API URL",       SanitizeUrlForLog (g_szOtlpPushApiURL).c_str(),       g_szEnvOtlpPushApiUrl);
    LogCfgText (bShowEnvVars, "OTLP Push API URL Backup", SanitizeUrlForLog (g_szOtlpPushApiURLBackup).c_str(), g_szEnvOtlpPushApiUrlBackup);
    LogCfgNum  (bShowEnvVars, "OTLP Push Failback sec",  g_FailbackSec,        g_szEnvOtlpPushFailbackSec);
    LogCfgNum  (bShowEnvVars, "OTLP Push WAL Retry sec", g_WalRetrySec,        g_szEnvOtlpPushWalRetrySec);
    LogCfgNum  (bShowEnvVars, "OTLP Push WAL Max MB",    g_WalMaxMB,           g_szEnvOtlpPushWalMaxMB);
    LogCfgText (bShowEnvVars, "OTLP Push Encoding",     GetPushEncodingName(), g_szEnvOtlpPushEncoding);
    LogCfgText (bShowEnvVars, "OTLP Push Token",         g_szOtlpPushToken[0] ? "(set)" : "", g_szEnvOtlpPushToken);
    LogCfgText (bShowEnvVars, "OTLP CA File",            g_szOtlpCaFile,       g_szEnvOtlpCaFile);
    LogCfgText (bShowEnvVars, "OTLP Service Name",       g_szOtlpServiceName,  g_szEnvOtlpServiceName);
    LogCfgText (bShowEnvVars, "OTLP Service Namespace",  g_szServiceNamespace, g_szEnvOtlpServiceNamespace);
    LogCfgText (bShowEnvVars, "OTLP Service Instance",   g_szServiceInstanceId, g_szEnvOtlpServiceInstanceId);
    LogCfgText (bShowEnvVars, "WAL File",                g_szWalFile);

    g_List.AddText ("");

    snprintf (szMode, sizeof (szMode), "%04o", static_cast<unsigned int>(g_UnixSocketMode));

    /* -cfg shows the socket which is used: the default one if none is set. -env only shows what can be set */
    std::string UnixSocket = g_szUnixSocketPath;

    if ( (false == bShowEnvVars) && UseDefaultUnixSocket() )
        UnixSocket = GetDefaultUnixSocketPath() + " (default)";

    LogCfgText (bShowEnvVars, "Unix Socket",             UnixSocket.c_str(),   g_szEnvUnixSocket);
    LogCfgText (bShowEnvVars, "Unix Socket Mode",        szMode,               g_szEnvUnixSocketMode);

    snprintf (szMode, sizeof (szMode), "%04o", static_cast<unsigned int>(g_SyslogSocketMode));

    LogCfgText (bShowEnvVars, "Syslog Socket",           g_szSyslogSocketPath, g_szEnvSyslogSocket);
    LogCfgText (bShowEnvVars, "Syslog Socket Mode",      szMode,               g_szEnvSyslogSocketMode);
    LogCfgText (bShowEnvVars, "TCP Listen",              g_szTcpListen,        g_szEnvTcpListen);
    LogCfgNum  (bShowEnvVars, "Socket Queue Max",        g_SocketQueueMax,     g_szEnvSocketQueueMax);

    g_List.AddText ("");

    LogCfgText (bShowEnvVars, "File Input",              g_szFileInput,        g_szEnvFileInput);
    LogCfgText (bShowEnvVars, "File Start",              g_bFileStartAtEnd ? "end" : "begin", g_szEnvFileStart);
    LogCfgText (bShowEnvVars, "File State Dir",          g_szFileStateDir,     g_szEnvFileStateDir);
    LogCfgText (bShowEnvVars, "File Severity",           (0 == g_FileSeverityNumber) ? "off" : g_szFileSeverityText, g_szEnvFileSeverity);
    LogCfgText (bShowEnvVars, "File Service",            g_szFileService,      g_szEnvFileService);
    LogCfgText (bShowEnvVars, "File State File",         g_szFileStateFile);

    g_List.AddText ("");

    g_List.Print (" :  ");
}


void WriteEnvironment (int fd, const char* pszHeader)
{
    char** ppEnv = environ;

    if (fd < 0)
        return;

    if (NULL == ppEnv)
    {
        return;
    }

    if (!IsNullStr (pszHeader))
        dprintf (fd, "\n-----%s-----\n", pszHeader);

    while (*ppEnv)
    {
        dprintf (fd, "%s\n", *ppEnv);
        ppEnv++;
    }

    if (!IsNullStr(pszHeader))
        dprintf (fd, "-----%s-----\n\n", pszHeader);

}


/* pthread_create returns the error number. It does not set errno, so perror would show the wrong reason */
static bool CreateThread (pthread_t *pThread, void *(*pFunction) (void *), void *pArgument)
{
    int rc = pthread_create (pThread, NULL, pFunction, pArgument);

    if (0 == rc)
        return true;

    LogError ("Cannot create a thread (pthread_create)", strerror (rc));
    return false;
}


int main (int argc, char *argv[])
{
    int a   = 0;
    char *p = NULL;
    char *pParam = NULL;

    char    *pLine       = NULL;
    size_t  CountSeconds = 0;
    char    szThreadMessage[300] = {0};
    size_t  len          = 0;
    size_t  seconds      = 0;
    ssize_t nread        = 0;
    ssize_t nwritten     = 0;
    int     ExitCode     = 0;

    struct sigaction sa {};

    /* The lines of the console output start with the name of the process. Not with -nostdin, see below and log_line.hpp */
    SetLogPrefix (g_szTask);

    sa.sa_handler = handle_signal;
    sigemptyset (&sa.sa_mask);
    sa.sa_flags = 0;

    sigaction (SIGINT,  &sa, NULL);
    sigaction (SIGTERM, &sa, NULL);
    sigaction (SIGHUP,  &sa, NULL);

    snprintf (g_szVersion, sizeof (g_szVersion), "%d.%d.%d", OTELFWD_VERSION_MAJOR, OTELFWD_VERSION_MINOR, OTELFWD_VERSION_PATCH);

    /* Read configuration */

    g_LogLevel           = GetEnvironmentValue (g_szEnvLogLevel);
    g_Mirror2Stdout      = GetEnvironmentValue (g_szEnvMirrorToStdout);
    g_ShutdownMaxWaitSec = GetEnvironmentValue (g_szEnvShutdownMaxSec);

    if (0 == g_ShutdownMaxWaitSec)
        g_ShutdownMaxWaitSec = OTELFWD_DEFAULT_SHUTDOWN_MAX_WAIT_SEC;

    g_SocketQueueMax = GetEnvironmentValue (g_szEnvSocketQueueMax);

    if (0 == g_SocketQueueMax)
        g_SocketQueueMax = OTELFWD_DEFAULT_SOCKET_QUEUE_MAX;

    p = getenv (g_szEnvDataDir);
    if (p)
        snprintf (g_szDataDir, sizeof (g_szDataDir), "%s", p);

    if (0 == IsDirectoryWritable (g_szDataDir))
    {
        LogError ("Cannot write to the data directory (OTELFWD_DATA_DIR)");
        goto Done;
    }

    snprintf (g_szWalFile, sizeof (g_szWalFile), "%s/%s.wal", g_szDataDir, g_szTask);

    p = getenv (g_szEnvPromFile);
    if (p)
        snprintf (g_szMetricsFileName, sizeof (g_szMetricsFileName), "%s", p);
    else
        snprintf (g_szMetricsFileName, sizeof (g_szMetricsFileName), "%s/domino/stats/otelfwd.prom", g_szDataDir);

    p = getenv (g_szEnvHostname);
    if (p)
        snprintf (g_szHostname, sizeof (g_szHostname), "%s", p);
    else
        GetHostname (sizeof (g_szHostname), g_szHostname);

    snprintf (g_szPidNbfFile,  sizeof (g_szPidNbfFile),  "%s/pid.nbf",   g_szDataDir);

    p = getenv (g_szEnvOtlpPushApiUrl);
    if (p)
        snprintf (g_szOtlpPushApiURL, sizeof (g_szOtlpPushApiURL), "%s", p);

    p = getenv (g_szEnvOtlpPushApiUrlBackup);
    if (p)
        snprintf (g_szOtlpPushApiURLBackup, sizeof (g_szOtlpPushApiURLBackup), "%s", p);

    /* The value is checked at start (ValidateBackupConfig): an invalid one is reported and the default is used */
    p = getenv (g_szEnvOtlpPushFailbackSec);
    if (p && *p)
    {
        char *pEnd = NULL;
        long  Value = strtol (p, &pEnd, 10);

        if ( (*pEnd == '\0') && (Value >= 1) && (Value <= OTELFWD_MAX_FAILBACK_SEC) )
            g_FailbackSec = static_cast<size_t> (Value);
    }

    /* An invalid value is reported at start (ValidateWalRetryConfig) and the default is used */
    p = getenv (g_szEnvOtlpPushWalRetrySec);
    if (p && *p)
        ParseSecondsSetting (p, OTELFWD_MAX_WAL_RETRY_SEC, g_WalRetrySec);

    /* An invalid value is reported at start (ValidateWalMaxConfig) and the default is used */
    p = getenv (g_szEnvOtlpPushWalMaxMB);
    if (p && *p)
        ParseMegabytesSetting (p, OTELFWD_MAX_WAL_MAX_MB, g_WalMaxMB);

    /* An invalid value is reported at start (ValidatePushEncoding) and json is used */
    p = getenv (g_szEnvOtlpPushEncoding);
    if (p && *p)
        ParsePushEncoding (p, g_bPushProtobuf);

    p = getenv (g_szEnvOtlpPushToken);
    if (p)
        snprintf (g_szOtlpPushToken, sizeof (g_szOtlpPushToken), "%s", p);

    p = getenv (g_szEnvOtlpCaFile);
    if (p)
        snprintf (g_szOtlpCaFile, sizeof (g_szOtlpCaFile), "%s", p);

    p = getenv (g_szEnvOtlpServiceName);
    if (p && *p)
        snprintf (g_szOtlpServiceName, sizeof (g_szOtlpServiceName), "%s", p);

    p = getenv (g_szEnvOtlpServiceNamespace);
    if (p && *p)
        snprintf (g_szServiceNamespace, sizeof (g_szServiceNamespace), "%s", p);

    p = getenv (g_szEnvOtlpServiceInstanceId);
    if (p && *p)
        snprintf (g_szServiceInstanceId, sizeof (g_szServiceInstanceId), "%s", p);
    else
        snprintf (g_szServiceInstanceId, sizeof (g_szServiceInstanceId), "%s", g_szHostname);

    p = getenv (g_szEnvUnixSocket);
    if (p)
        snprintf (g_szUnixSocketPath, sizeof (g_szUnixSocketPath), "%s", p);

    p = getenv (g_szEnvUnixSocketMode);
    if (p && *p)
        g_UnixSocketMode = static_cast<mode_t>(strtoul (p, NULL, 8) & 0777);

    p = getenv (g_szEnvSyslogSocket);
    if (p)
        snprintf (g_szSyslogSocketPath, sizeof (g_szSyslogSocketPath), "%s", p);

    p = getenv (g_szEnvSyslogSocketMode);
    if (p && *p)
        g_SyslogSocketMode = static_cast<mode_t>(strtoul (p, NULL, 8) & 0777);

    p = getenv (g_szEnvTcpListen);
    if (p)
        snprintf (g_szTcpListen, sizeof (g_szTcpListen), "%s", p);

    /* The file input. The paths are made absolute: the name of the state file is made from the path, and it has to be the same every time */
    p = getenv (g_szEnvFileInput);
    if (p && *p)
        MakeAbsolutePath (p, g_szFileInput, sizeof (g_szFileInput));

    /* Without a directory the state file is next to the file */
    p = getenv (g_szEnvFileStateDir);
    if (p && *p)
        MakeAbsolutePath (p, g_szFileStateDir, sizeof (g_szFileStateDir));

    /* An invalid value is reported at start (ValidateFileStart) and begin is used */
    p = getenv (g_szEnvFileStart);
    if (p && *p)
        ParseFileStart (p, g_bFileStartAtEnd);

    /* An invalid value is reported at start (ValidateFileConfig) and info is used */
    p = getenv (g_szEnvFileSeverity);
    if (p && *p)
    {
        int         Number = 0;
        std::string Text;

        if (FileSeverity::Parse (p, Number, Text))
        {
            g_FileSeverityNumber = Number;
            snprintf (g_szFileSeverityText, sizeof (g_szFileSeverityText), "%s", Text.c_str());
        }
    }

    p = getenv (g_szEnvFileService);
    if (p && *p)
        snprintf (g_szFileService, sizeof (g_szFileService), "%s", p);

    if (false == IsNullStr (g_szFileInput))
        snprintf (g_szFileStateFile, sizeof (g_szFileStateFile), "%s", FileStateName::Make (g_szFileStateDir, g_szFileInput).c_str());

    p = getenv (g_szEnvOutputLog);
    if (p)
        snprintf (g_szOutputLogFile, sizeof (g_szOutputLogFile), "%s", p);

    for (a=1; a<argc; a++)
    {
        pParam = argv[a];

        if ( (0 == strcasecmp (pParam, "--version")) ||
             (0 == strcasecmp (pParam, "-version")) )
        {
            printf ("%s", g_szVersion);
            goto Done;
        }

        else if ( (0 == strcasecmp (pParam, "-help")) ||
             (0 == strcasecmp (pParam, "-h")) ||
             (0 == strcasecmp (pParam, "-?")) )
        {
            PrintHelp();
            return 0;
        }

        else if ( (0 == strcasecmp (pParam, "-config")) ||
             (0 == strcasecmp (pParam, "-cfg")) )
        {
            DumpConfig();
            return 0;
        }

        else if (0 == strcasecmp (pParam, "-env"))
        {
            DumpConfig (true);
            return 0;
        }

        else if (0 == strcasecmp (pParam, "-nostdin"))
        {
            g_NoStdin = 1;
        }

        else
        {
            LogError ("Invalid parameter", pParam);
            return 1;
        }
    }

    /* With -nostdin the output is only ours: the runtime names the process. In pipe mode the lines share the output with the
       lines of the server, and the name tells them apart */
    if (g_NoStdin)
        SetLogPrefix (NULL);

    PrintBanner();

    /* --- No operations before this point because the parameter read loop exits for some parameters */

    /* Version 1.x pushed to the Loki push API, which was removed in version 2.0.
       Do not silently lose logs after an upgrade with an old configuration. */
    if (getenv ("LOKI_PUSH_API_URL"))
        LogError ("LOKI_PUSH_API_URL is not supported anymore. Version 2.0 only pushes to OTLP. Please configure OTLP_PUSH_API_URL (see README)");

    ValidatePushEncoding();
    ValidateWalRetryConfig();
    ValidateWalMaxConfig();
    ValidateFileConfig();
    ValidateBackupConfig();

    /* The backup is only used if it is configured, and it is checked above: an invalid one was removed */
    g_PushFailover.Configure (false == IsNullStr (g_szOtlpPushApiURLBackup), static_cast<time_t> (g_FailbackSec));

    {
        char szOldWal[2200] = {0};
        struct stat OldWalStat {};

        snprintf (szOldWal, sizeof (szOldWal), "%s/domfwd.wal", g_szDataDir);

        if ( (0 == stat (szOldWal, &OldWalStat)) && (OldWalStat.st_size > 0) )
            LogMessage ("Warning: Ignoring unsent data of a version before 2.0 in an old WAL file (Loki push format is not supported anymore)");
    }

    if (g_NoStdin && (g_Mirror2Stdout || (false == IsNullStr (g_szOutputLogFile))))
        LogMessage ("Warning: OTELFWD_MIRROR_STDOUT and OTELFWD_OUTPUT_LOG only apply to lines read from STDIN. They are ignored with -nostdin");

    MakeDirectoryTreeFromFileName (g_szMetricsFileName);

    /* The messages of the WAL ("[Error] WAL: ...") are lines of the console output of otelfwd: with the time and the process name */
    g_Wal.SetLogFunction ([] (const char *pszMessage) { LogMessage (pszMessage); });
    g_Wal.SetMaxSize (static_cast<uint64_t> (g_WalMaxMB) * 1024 * 1024);

    if (*g_szOtlpPushApiURL)
        g_bWalOpened = g_Wal.Init (g_szWalFile);

    /* Like the socket inputs, the file input needs a push target: without one the lines have nowhere to go */
    g_bFileInputEnabled = (false == IsNullStr (g_szFileInput)) && (false == IsNullStr (g_szOtlpPushApiURL));

    LogStartupSummary (g_bWalOpened);

    /* Without a WAL a push which fails is lost. In pipe mode that is accepted and shown in the summary and in the health: exiting
       would close the pipe of the server. A standalone instance has no such pipe, and the usual reason for the WAL not to open is
       another instance which uses the same OTELFWD_DATA_DIR (the WAL is locked). Exit with an error, so a service manager sees it */
    if (g_NoStdin && (false == IsNullStr (g_szOtlpPushApiURL)) && (false == g_bWalOpened))
    {
        LogError ("-nostdin needs a WAL and it cannot be opened, see the message above. Every instance needs its own OTELFWD_DATA_DIR", g_szWalFile);
        ExitCode = 1;
        goto Done;
    }

    curl_global_init (CURL_GLOBAL_DEFAULT);

    if ( (0 == g_NoStdin) && (false == IsNullStr (g_szOutputLogFile)) )
    {
        g_fdOutputLogFile = open (g_szOutputLogFile, O_CREAT | O_APPEND | O_WRONLY, 0644);
    }

    /* The file input: the reader is opened here, and the thread is started with the others. Nothing is read yet */
    if (false == IsNullStr (g_szFileInput))
    {
        if (false == g_bFileInputEnabled)
        {
            LogError ("The file input needs OTLP_PUSH_API_URL. The file input is disabled");
        }
        else
        {
            /* A directory which was asked for is made. The directory of the file itself is not: the program which writes the file makes it */
            if (false == IsNullStr (g_szFileStateDir))
                MakeDirectoryTreeFromFileName (g_szFileStateFile);

            /* The state file is written next to the file by default, and that directory may belong to somebody else, or be read only.
               Then the position cannot be saved, and every restart reads the file again from the beginning */
            {
                std::string StateDir  = g_szFileStateFile;
                size_t      Slash     = StateDir.rfind ('/');
                struct stat DirStat {};

                StateDir = (std::string::npos == Slash) ? std::string (".") : ((0 == Slash) ? std::string ("/") : StateDir.substr (0, Slash));

                if ( (0 == stat (StateDir.c_str(), &DirStat)) && (0 != access (StateDir.c_str(), W_OK | X_OK)) )
                    LogError ("The directory of the state file of the file input is not writable: the position cannot be saved, and a restart reads the file again. Set OTELFWD_FILE_STATE_DIR", StateDir.c_str());
            }

            g_FileReader.SetStartAtEnd (g_bFileStartAtEnd);
            g_FileReader.SetLogFunction ([] (const char *pszMessage) { LogMessage (pszMessage); });

            if (false == g_FileReader.Open (g_szFileInput, g_szFileStateFile))
            {
                LogError ("The file input cannot be started", g_szFileInput);
                g_bFileInputEnabled = false;
            }
        }
    }

    /* The default Unix socket, see UseDefaultUnixSocket(). The summary at start has told which one it is */
    if (UseDefaultUnixSocket())
    {
        snprintf (g_szUnixSocketPath, sizeof (g_szUnixSocketPath), "%s", GetDefaultUnixSocketPath().c_str());
        MakeDirectoryTreeFromFileName (g_szUnixSocketPath);
    }

    /* Socket inputs. Records received there are pushed via OTLP, so they need an OTLP endpoint.
       A failing input is logged and skipped, stdin processing continues. */

    if ( (false == IsNullStr (g_szUnixSocketPath)) || (false == IsNullStr (g_szTcpListen)) || (false == IsNullStr (g_szSyslogSocketPath)) )
    {
        if (IsNullStr (g_szOtlpPushApiURL))
        {
            LogError ("Socket inputs need OTLP_PUSH_API_URL. Socket inputs are disabled");
        }
        else
        {
            if (false == IsNullStr (g_szUnixSocketPath))
                CreateUnixListener (&g_IngestUnix, g_szUnixSocketPath, g_UnixSocketMode);

            if (false == IsNullStr (g_szTcpListen))
                CreateTcpListener (&g_IngestTcp, g_szTcpListen);

            if (false == IsNullStr (g_szSyslogSocketPath))
                CreateUnixListener (&g_IngestSyslog, g_szSyslogSocketPath, g_SyslogSocketMode, SOCK_DGRAM);
        }
    }

    /* Without STDIN the socket inputs are the only source. Without one there is nothing to do.
       Exit with an error instead of running idle, so a service manager does not consider it healthy. */
    if (g_NoStdin && (false == g_IngestUnix.bEnabled) && (false == g_IngestTcp.bEnabled) && (false == g_IngestSyslog.bEnabled) && (false == g_bFileInputEnabled))
    {
        LogError ("-nostdin needs at least one working input (OTELFWD_UNIX_SOCKET, OTELFWD_TCP_LISTEN, OTELFWD_SYSLOG_SOCKET or OTELFWD_FILE_INPUT, and OTLP_PUSH_API_URL)");
        ExitCode = 1;
        goto Done;
    }

    /* Create threads */

    if (false == CreateThread (&g_PushThreadInstance, PushThread, NULL))
        return EXIT_FAILURE;

    if (*g_szOtlpPushApiURL)
    {
        if (false == CreateThread (&g_WalThreadInstance, WalThread, NULL))
            return EXIT_FAILURE;
    }

    if (false == CreateThread (&g_MetricsThreadInstance, MetricsThread, NULL))
        return EXIT_FAILURE;

    if (g_IngestUnix.bEnabled)
    {
        g_IngestUnix.Running = 1;

        if (false == CreateThread (&g_IngestUnix.Thread, IngestThread, &g_IngestUnix))
            return EXIT_FAILURE;
    }

    if (g_IngestTcp.bEnabled)
    {
        g_IngestTcp.Running = 1;

        if (false == CreateThread (&g_IngestTcp.Thread, IngestThread, &g_IngestTcp))
            return EXIT_FAILURE;
    }

    if (g_IngestSyslog.bEnabled)
    {
        g_IngestSyslog.Running = 1;

        if (false == CreateThread (&g_IngestSyslog.Thread, SyslogThread, &g_IngestSyslog))
            return EXIT_FAILURE;
    }

    if (g_bFileInputEnabled)
    {
        g_FileThreadRunning = 1;

        if (false == CreateThread (&g_FileThreadInstance, FileThread, NULL))
            return EXIT_FAILURE;
    }

    if (g_Mirror2Stdout && (0 == g_NoStdin))
    {
        if (g_DumpEnvironment)
            WriteEnvironment (g_fdStdOut, "Environment");
    }

    /* Without STDIN the socket input threads do all the work. Wait for SIGTERM/SIGINT */
    if (g_NoStdin)
    {
        LogMessage ("Running without STDIN input. Serving the inputs until SIGTERM or SIGINT");

        while (0 == g_ShutdownRequested)
            sleep_ms (200);
    }

    /* Read from stdin and process the log line (annotating it, writing it to a log, pushing it via OTLP, ...) */

    while ( (0 == g_NoStdin) && ((nread = getline (&pLine, &len, stdin)) != -1) )
    {
        if (nread == 0)
            continue;

        /* Mirror unmodified to a log file */
        if (g_fdOutputLogFile >= 0)
        {
            nwritten = write (g_fdOutputLogFile, pLine, nread);
            if (nread != nwritten)
            {
                /* LATER: Should we log this error and where ... ? */
            }
        }

        /* Mirror it unmodified to stdout */
        if (g_Mirror2Stdout)
        {
            nwritten = write (g_fdStdOut, pLine, nread);
            if (nread != nwritten)
            {
                /* LATER: Should we log this error and where ... ? */
            }
        }

        /* Remove new line */
        if ('\n' == pLine[nread-1])
            pLine[nread-1] = '\0';

        LogRecord Record;

        Record.TimeNs = GetEpochNanoseconds();
        Record.Line   = pLine;

        /* stdin records are never dropped */
        g_LogFifo.push (std::move (Record));

    } /* while read from STDIN */

    /* Gives the push thread a moment to pick up the last lines of STDIN. Not needed without STDIN */
    if (0 == g_NoStdin)
        sleep (2);

    LogMessage ("Waiting for shutdown to complete");

    /* Wait until pending entries have been processed */
    seconds = 0;
    while ( (*g_szOtlpPushApiURL) && g_Wal.IsReplayPending() )
    {
        sleep (1);

        if (g_ShutdownRequested)
            break;

        if (seconds++ > g_ShutdownMaxWaitSec)
        {
            LogMessage ("Shutdown timeout reached");
            break;
        }
    }

    g_ShutdownRequested = 1;

    /* Wait for the socket inputs and the file input to stop, before the queue is shut down. Nothing is queued after that */
    seconds = 0;
    while ( (g_IngestUnix.Running || g_IngestTcp.Running || g_IngestSyslog.Running || g_FileThreadRunning) && (seconds < 200) )
    {
        sleep_ms (50);
        seconds++;
    }

    g_LogFifo.shutdown();

    while (g_PushThreadRunning || g_WalThreadRunning || g_MetricsThreadRunning)
    {
        CountSeconds++;
        if (0 == (CountSeconds %10))
        {
            if (g_LogLevel)
            {
                snprintf (szThreadMessage, sizeof (szThreadMessage), "Waiting %lu seconds for threads to terminate (Push: %lu, Wal: %lu, Metrics: %lu)", CountSeconds,
                          static_cast<unsigned long>(g_PushThreadRunning.load()), static_cast<unsigned long>(g_WalThreadRunning.load()), static_cast<unsigned long>(g_MetricsThreadRunning.load()));
                LogMessage (szThreadMessage);
            }
        }

        sleep (1);

        if (CountSeconds > 300)
        {
            snprintf (szThreadMessage, sizeof (szThreadMessage), "Failed to terminate all threads (Push: %lu, Wal: %lu, Metrics: %lu)",
                      static_cast<unsigned long>(g_PushThreadRunning.load()), static_cast<unsigned long>(g_WalThreadRunning.load()), static_cast<unsigned long>(g_MetricsThreadRunning.load()));
            LogError (szThreadMessage);
            break;
        }
    }

    /* The push thread delivered the last batches after the file thread had ended. Their positions are committed here, where the reader is
       not used by a thread any more: a restart does not repeat lines which were delivered */
    if (g_bFileInputEnabled && (0 == g_FileThreadRunning))
    {
        uint64_t Generation = 0;
        uint64_t EndOffset  = 0;

        if (g_FileCommit.Take (Generation, EndOffset))
            g_FileReader.Commit (Generation, EndOffset);
    }

Done:

    curl_global_cleanup();

    if (g_fdOutputLogFile >= 0)
    {
        close (g_fdOutputLogFile);
        g_fdOutputLogFile = -1;
    }

    if (pLine)
    {
        free (pLine);
        pLine = NULL;
    }

    WriteMetrics (true);
    LogMessage ("Shutdown completed");

    return ExitCode;
}
