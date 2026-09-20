/* domfwd.cpp - Minimal Domino server add-in to inspect events delivered by Domino Event Monitoring */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stddef.h>
#include <errno.h>
#include <time.h>
#include <stdint.h>
#include <ctype.h>

/* Windows is detected with the compiler's _WIN32, not with W32: the Domino headers also define W32 on
   UNIX/Linux ("Windows 32 API - its emulated"), so W32 does not mean Windows.

   Winsock has to be included before any header that includes windows.h (used by domfwd_socket.hpp) */
#if defined (_WIN32)
    #include <winsock2.h>
    #include <ws2tcpip.h>
#endif

#include <global.h>
#include <addin.h>
#include <event.h>
#include <eventerr.h>
#include <globerr.h>
#include <kfm.h>
#include <miscerr.h>
#include <mq.h>
#include <nsfdb.h>
#include <nsfnote.h>
#include <osenv.h>
#include <oserr.h>
#include <osfile.h>
#include <osmem.h>
#include <osmisc.h>
#include <ostime.h>
#include <reg.h>
#include <stdnames.h>

/* Direct OTLP push via libcurl. Optional: build with -DDOMFWD_CURL to include it (the makefiles do that unless USE_CURL=0).
   The libcurl bundled with Domino is used (Windows: import stub libcurl-x64.def for the curl_* exports of
   nnotes.dll, curl SDK headers are only needed for the types). The socket transport to the forwarder
   (domfwd_socket.hpp) is always built and is the default way to forward events. */

#ifdef DOMFWD_CURL
    #include <curl/curl.h>
#endif

/* mswin64.mak passes -DW (Domino platform flag); it collides with a
   parameter named W in rapidjson/internal/dtoa.h. Safe to drop here -
   no Domino header below this point depends on it. */

#ifdef W
    #undef W
#endif

/* Override assertions to ensure we don't terminate for logical errors */

#define RAPIDJSON_ASSERT(x)

#include <rapidjson/document.h>
#include <rapidjson/stringbuffer.h>
#include <rapidjson/writer.h>

#include "domfwd_durable.hpp"
#include "../health.hpp"

#include <stdarg.h>
#include <time.h>

/* Version of the add-in. Shown at startup together with the build date and time, to tell builds apart. */
#define DOMFWD_VERSION_MAJOR 0
#define DOMFWD_VERSION_MINOR 9
#define DOMFWD_VERSION_PATCH 0

#define DOMFWD_STR_(x) #x
#define DOMFWD_STR(x)  DOMFWD_STR_(x)
#define DOMFWD_VERSION DOMFWD_STR(DOMFWD_VERSION_MAJOR) "." DOMFWD_STR(DOMFWD_VERSION_MINOR) "." DOMFWD_STR(DOMFWD_VERSION_PATCH)

/* Same numeric convention as otelfwd: major * 10000 + minor * 100 + patch */
#define DOMFWD_VERSION_BUILD (DOMFWD_VERSION_MAJOR * 10000 + DOMFWD_VERSION_MINOR * 100 + DOMFWD_VERSION_PATCH)

char  g_szTask[]     = "domfwd";
char  g_szTaskLong[] = "Domino Event Forwarder";

/* Name of the event queue which receives the events. Not the task queue (MQ$DOMFWD, see DOMFWD_MQ_NAME).
   The event handling of the server (events4.nsf) posts to this name. Changing it stops the delivery until
   that configuration is changed too. It is deliberately not derived from the task name. */
char  g_szEventQueue[] = "domfwd";
char  g_szScope[]    = "domino.event";

FILE *g_pEventLogFile                      = NULL;
WORD  g_wVerboseLevel                      = 0;

/* Tracing with -vv: printf to the stdout of the add-in process. It does not use AddInLogMessageText, so it does not
   show up in the Domino console log and cannot come back as an event. Where stdout ends up depends on how the server was started. */
void TraceF (const char *pszFormat, ...)
{
    va_list Args;

    printf ("domfwd trace %lu: ", (unsigned long) time (NULL));

    va_start (Args, pszFormat);
    vprintf (pszFormat, Args);
    va_end (Args);

    printf ("\n");
    fflush (stdout);
}

#define DOMFWD_TRACE(...) do { if (g_wVerboseLevel >= 2) TraceF (__VA_ARGS__); } while (0)

#define DOMFWD_PROM_INTERVAL_SEC 30

/* Shutdown: how long the event queue has to stay empty before it is freed, and the longest wait per drain */
#define DOMFWD_SHUTDOWN_QUIET_MS 500
#define DOMFWD_SHUTDOWN_MAX_MS   3000

/* Counters for the Prometheus file (domfwd.prom). Only written by the add-in main loop. */
unsigned long g_ulEventsReceived       = 0;
unsigned long g_ulEventsSkippedOwn     = 0;
unsigned long g_ulEventsDroppedVersion = 0;
time_t        g_tLastEvent             = 0;
time_t        g_tStartTime             = 0;
BOOL          g_fPromEnabled           = TRUE;

BOOL  g_fDumpTypesAndExit                  = FALSE;
char  g_szHostName[MAXUSERNAME + 1]        = {0};
char  g_szLocalServerName[MAXUSERNAME + 1] = {0};
char  g_szDominoVersion[40]                = {0};
char  g_szOtelPushURL[512]                 = {0};
char  g_szOtelPushToken[512]               = {0};
char  g_szOtelCaFile[512]                  = {0};
DWORD g_dwOwnProcessId                     = 0;
char  g_szSocketTarget[512]                = {0};
char  g_szSocketWal[512]                   = {0};

/* Socket transport to the forwarder (pipe tool). notes.ini: DOMFWD_Socket=unix:/path/to/socket (Linux)
   or DOMFWD_Socket=tcp:127.0.0.1:4390. Events wait in a bounded queue in memory when the forwarder is connected and slow, and in a WAL
   on disk (DOMFWD_SocketWAL, Linux) when it is not reachable: see domfwd_durable.hpp. */
DomfwdDurableSender g_SocketSender;
uint64_t            g_SocketWalMaxBytes = 0;

/* The health state for alerting (see ../health.hpp). Updated in the main loop, with the metrics */
HealthMonitor       g_Health;

#define DOMFWD_SOCKET_MAX_LINES      2000
#define DOMFWD_SOCKET_MAX_BYTES      (4 * 1024 * 1024)
#define DOMFWD_SOCKET_FLUSH_MS       2000
#define DOMFWD_SOCKET_WAL_DEFAULT_MB 128

#define DOMFWD_TASKNAME "DOMFWD"
#define DOMFWD_MQ_NAME  TASK_QUEUE_PREFIX DOMFWD_TASKNAME

#define DOMFWD_EXPECTED_EVENT_VERSION 2
#define DOMFWD_MAX_EVENTS_PER_IDLE 20
#define DOMFWD_UTF8_SIZE(n) (((n) * 2) + 1)

#if defined (_WIN32)
    #include <direct.h>
    #include <windows.h>
    #define DOMFWD_PATHSEP "\\"
    #define DOMFWD_OS_TYPE "windows"
#else
    #include <sys/stat.h>
    #include <unistd.h>
    #define DOMFWD_PATHSEP "/"
    #define DOMFWD_OS_TYPE "linux"
#endif

/* Substring match, not exact: observed ExecName values are "ndomfwd"
   (Windows, "n" + base name, no ".exe") and presumably "domfwd" on Linux -
   but EventExtractAddinExt is undocumented, so match loosely rather than
   assume the exact format holds across versions/platforms. */

#define DOMFWD_OWN_EXEC_NAME "domfwd"

struct DOMFWD_EVENT_DATA_EXT_OPAQUE;

extern "C"
{
    void LNPUBLIC EventExtractAddinExt (struct DOMFWD_EVENT_DATA_EXT_OPAQUE *pEvent,
                                         char *pszAddinName, WORD wAddinBuffLen,
                                         char *pszErrorText, WORD wErrorBuffLen,
                                         char *pszErrorCode, WORD wCodeBuffLen,
                                         char *pszParameters, WORD wParamBuffLen,
                                         char *pszPIDTID, WORD wPIDTIDBuffLen,
                                         char *pszExecName, WORD wExecBuffLen);

    void LNPUBLIC EventExtractTargetData (struct DOMFWD_EVENT_DATA_EXT_OPAQUE *pEvent,
                                           char *pszTargetServer, WORD wTargetServerBuffLen,
                                           char *pszTargetDatabase, WORD wTargetDbBuffLen,
                                           char *pszTargetUser, WORD wTargetUserBuffLen,
                                           char *pszTargetExtraData, WORD wTargetExtraDataBuffLen);

    STATUS LNPUBLIC EventQueueGetExt (char far *pszQueueName, DHANDLE far *rethEvent);
}


typedef struct
{
    char szAddinName[MAXSPRINTF + 1];
    char szExecName[MAXSPRINTF + 1];
    char szPIDTID[MAXSPRINTF + 1];
    char szTargetServer[MAXUSERNAME + 1];
    char szTargetDatabase[MAXPATH + 1];
    char szTargetUser[MAXUSERNAME + 1];
    char szErrorCode[MAXSPRINTF + 1];
    char szErrorText[(MAXSPRINTF * 4) + 1];
} DOMFWD_EXT_EVENT_INFO;


typedef struct
{
    WORD     wVersion;
    WORD     wErrorCode;
    WORD     wAdditionalErrorCode;
    WORD     wType;
    WORD     wSeverity;
    WORD     wFormatSpecifier;
    WORD     wEventDataLength;
    WORD     wAddinNameLength;
    TIMEDATE tdEventTime;
    BOOL     fExtValid;

    char szOriginatingServerName[MAXUSERNAME];
    char szEventTime[MAXALPHATIMEDATE + 1];

    DOMFWD_EXT_EVENT_INFO ext;
} DOMFWD_EVENT_INFO;


/* One resource/attribute entry - either a string or an int value, never
   both. Lets WriteEventJson and PushEventToOtel share the same "which
   fields, what values" list and only differ in how they serialize it
   (flat object vs. OTLP's typed key/value arrays). */

typedef struct
{
    const char *pszKey;
    BOOL        fIsInt;
    LONG        lIntValue;
    const char *pszStringValue;
} DOMFWD_KV;

#define DOMFWD_MAX_RESOURCE_KV   8
#define DOMFWD_MAX_ATTRIBUTE_KV 11

/* Everything derived once from a DOMFWD_EVENT_INFO (UTF8 conversion, epoch
   timestamps, OTel severity mapping, resource/attribute key/value lists)
   that both WriteEventJson and PushEventToOtel need - computed once via
   BuildOtelFields instead of each consumer redoing the same work. */

typedef struct
{
    char        szTimeUnixNano[24];
    char        szObservedTimeUnixNano[24];
    WORD        wSeverityNumber;
    const char *pszSeverityText;
    const char *pszBody;
    DWORD       dwProcessId;

    char szServerUtf8[DOMFWD_UTF8_SIZE (MAXUSERNAME)];
    char szErrorTextUtf8[DOMFWD_UTF8_SIZE (MAXSPRINTF + 1)];
    char szAddinNameUtf8[DOMFWD_UTF8_SIZE (MAXSPRINTF + 1)];
    char szExecNameUtf8[DOMFWD_UTF8_SIZE (MAXSPRINTF + 1)];
    char szTargetServerUtf8[DOMFWD_UTF8_SIZE (MAXUSERNAME + 1)];
    char szTargetDbUtf8[DOMFWD_UTF8_SIZE (MAXPATH + 1)];
    char szTargetUserUtf8[DOMFWD_UTF8_SIZE (MAXUSERNAME + 1)];

    DOMFWD_KV ResourceKV[DOMFWD_MAX_RESOURCE_KV];
    WORD      wResourceKVCount;

    DOMFWD_KV AttributeKV[DOMFWD_MAX_ATTRIBUTE_KV];
    WORD      wAttributeKVCount;
} DOMFWD_OTEL_FIELDS;


void AddKV (DOMFWD_KV *pArray, WORD *pCount, WORD wMax, const char *pszKey, const char *pszValue)
{
    if (*pCount >= wMax)
        return;

    pArray[*pCount].pszKey         = pszKey;
    pArray[*pCount].fIsInt         = FALSE;
    pArray[*pCount].pszStringValue = pszValue;
    (*pCount)++;
}


void AddKVInt (DOMFWD_KV *pArray, WORD *pCount, WORD wMax, const char *pszKey, LONG lValue)
{
    if (*pCount >= wMax)
        return;

    pArray[*pCount].pszKey    = pszKey;
    pArray[*pCount].fIsInt    = TRUE;
    pArray[*pCount].lIntValue = lValue;
    (*pCount)++;
}


/* Flat-shape serializer: {"key": value, ...} - used by WriteEventJson. */

void AddKVsToJsonObject (rapidjson::Value &retObj, const DOMFWD_KV *pArray, WORD wCount, rapidjson::Document::AllocatorType &alloc)
{
    WORD i = 0;

    for (i = 0; i < wCount; i++)
    {
        if (pArray[i].fIsInt)
        {
            /* Named key and explicit int: GCC does not bind a temporary to the non-const reference
               of this AddMember overload (MSVC accepts that as a language extension) */

            rapidjson::Value Key (pArray[i].pszKey, alloc);

            retObj.AddMember (Key, static_cast<int> (pArray[i].lIntValue), alloc);
        }
        else
            retObj.AddMember (rapidjson::Value (pArray[i].pszKey, alloc), rapidjson::Value (pArray[i].pszStringValue, alloc), alloc);
    }
}


/* Case-insensitive strstr - no portable standard one. Linux has strcasestr
   (glibc/BSD extension) so use the real thing there; Windows has no
   equivalent in the standard CRT (StrStrIA would need linking
   Shlwapi.lib), so fall back to a hand-rolled version there. Needed
   because EventExtractAddinExt is undocumented, so casing of values like
   ExecName isn't guaranteed. */

BOOL ContainsCaseInsensitive (const char *pszHaystack, const char *pszNeedle)
{
    if ((NULL == pszHaystack) || (NULL == pszNeedle) || ('\0' == *pszNeedle))
        return FALSE;

#if defined (_WIN32)
    {
        size_t wNeedleLen = strlen (pszNeedle);
        size_t i          = 0;

        for (i = 0; '\0' != pszHaystack[i]; i++)
        {
            size_t j = 0;

            for (j = 0; j < wNeedleLen; j++)
            {
                if ('\0' == pszHaystack[i + j])
                    break;

                if (tolower ((unsigned char) pszHaystack[i + j]) != tolower ((unsigned char) pszNeedle[j]))
                    break;
            }

            if (j == wNeedleLen)
                return TRUE;
        }

        return FALSE;
    }
#else
    return (NULL != strcasestr (pszHaystack, pszNeedle));
#endif
}


/* Shifts pszString left in place if it starts with pszPrefix, discarding
   the prefix. Shared by StripVersionPrefix and the addin-name-prefix
   strip below - both are "does this string start with a known prefix I
   want to discard" operations. Returns TRUE if a prefix was stripped. */

BOOL StripPrefix (char *pszString, const char *pszPrefix)
{
    size_t len = 0;

    if ((NULL == pszString) || (NULL == pszPrefix) || ('\0' == pszPrefix[0]))
        return FALSE;

    len = strlen (pszPrefix);

    if (0 != strncmp (pszString, pszPrefix, len))
        return FALSE;

    memmove (pszString, pszString + len, strlen (pszString + len) + 1);
    return TRUE;
}


BOOL ExtractEventDataExt (EVENT_DATA *pEvent, DOMFWD_EXT_EVENT_INFO *retpInfo)
{
    struct DOMFWD_EVENT_DATA_EXT_OPAQUE *pExt = NULL;

    memset (retpInfo, 0, sizeof (DOMFWD_EXT_EVENT_INFO));

    if (EVENT_VERSION != pEvent->Version)
        return FALSE;

    pExt = (struct DOMFWD_EVENT_DATA_EXT_OPAQUE *) pEvent;

    /* ErrorCode is research-only (see comment on DOMFWD_EXT_EVENT_INFO) -
       ErrorText is used, see the same comment. */

    EventExtractAddinExt (pExt,
                          retpInfo->szAddinName, sizeof (retpInfo->szAddinName) - 1,
                          retpInfo->szErrorText, sizeof (retpInfo->szErrorText) - 1,
                          retpInfo->szErrorCode, sizeof (retpInfo->szErrorCode) - 1,
                          NULL, 0,
                          retpInfo->szPIDTID, sizeof (retpInfo->szPIDTID) - 1,
                          retpInfo->szExecName, sizeof (retpInfo->szExecName) - 1);

    EventExtractTargetData (pExt,
                            retpInfo->szTargetServer, sizeof (retpInfo->szTargetServer) - 1,
                            retpInfo->szTargetDatabase, sizeof (retpInfo->szTargetDatabase) - 1,
                            retpInfo->szTargetUser, sizeof (retpInfo->szTargetUser) - 1,
                            NULL, 0);

    /* Defensive: these functions have been observed filling an output buffer */
    retpInfo->szAddinName[sizeof (retpInfo->szAddinName) - 1]           = '\0';
    retpInfo->szPIDTID[sizeof (retpInfo->szPIDTID) - 1]                 = '\0';
    retpInfo->szExecName[sizeof (retpInfo->szExecName) - 1]             = '\0';
    retpInfo->szTargetServer[sizeof (retpInfo->szTargetServer) - 1]     = '\0';
    retpInfo->szTargetDatabase[sizeof (retpInfo->szTargetDatabase) - 1] = '\0';
    retpInfo->szTargetUser[sizeof (retpInfo->szTargetUser) - 1]         = '\0';
    retpInfo->szErrorCode[sizeof (retpInfo->szErrorCode) - 1]           = '\0';
    retpInfo->szErrorText[sizeof (retpInfo->szErrorText) - 1]           = '\0';

    return TRUE;
}


const char *GetFormatSpecifierName (WORD wFormatSpecifier)
{
    switch (wFormatSpecifier)
    {
        case FMT_UNKNOWN:
            return "FMT_UNKNOWN";

        case FMT_TEXT:
            return "FMT_TEXT";

        case FMT_ERROR_CODE:
            return "FMT_ERROR_CODE";

        case FMT_ERROR_MSG:
            return "FMT_ERROR_MSG";

        default:
            return "FMT_?";
    }
}


const char *GetEventTypeName (WORD wType)
{
    switch (wType)
    {
        case EVT_UNKNOWN:   return UNKNOWN_NAME;
        case EVT_COMM:      return COMM_NAME;
        case EVT_SECURITY:  return SECURE_NAME;
        case EVT_MAIL:      return MAIL_NAME;
        case EVT_REPLICA:   return REPLICA_NAME;
        case EVT_RESOURCE:  return RESOURCE_NAME;
        case EVT_MISC:      return MISC_NAME;
        case EVT_SERVER:    return SERVER_NAME;
        case EVT_ALARM:     return ALARM_NAME;
        case EVT_UPDATE:    return UPDATE_NAME;
        case EVT_DATABASE:  return DATABASE_NAME;
        case EVT_NETWORK:   return NETWORK_NAME;
        case EVT_COMPILER:  return COMPILER_NAME;
        case EVT_ROUTER:    return ROUTER_NAME;
        case EVT_AGENT:     return AGENT_NAME;
        case EVT_CLIENT:    return CLIENT_NAME;
        case EVT_ADDIN:     return ADDIN_NAME;
        case EVT_ADMINP:    return ADMINP_NAME;
        case EVT_WEB_CODE:  return WEB_NAME;
        case EVT_NNTP_CODE: return NNTP_NAME;
        case EVT_FTP_CODE:  return FTP_NAME;
        case EVT_CODE:      return CODE_NAME;
        case EVT_LDAP:      return LDAP_NAME;
        case EVT_MONITOR:   return MONITOR_NAME;
        case EVT_PROG:      return PROG_NAME;
        default:            return "EVT_?";
    }
}


const char *GetEventSeverityName (WORD wSeverity)
{
    switch (wSeverity)
    {
        case SEV_UNKNOWN:   return UNKNOWN_NAME;
        case SEV_FATAL:     return FATAL_NAME;
        case SEV_FAILURE:   return FAILURE_NAME;
        case SEV_WARNING1:  return WARNING1_NAME;
        case SEV_WARNING2:  return WARNING2_NAME;
        case SEV_NORMAL:    return NORMAL_NAME;
        default:            return "SEV_?";
    }
}


/* Maps Domino's severity scale onto the OpenTelemetry Log Data Model's
   normalized SeverityNumber (1-24, 0 = unspecified) and SeverityText. */

void GetOtelSeverity (WORD wSeverity, WORD *retpwOtelNumber, const char **retppszOtelText)
{
    *retpwOtelNumber = 0;
    *retppszOtelText = "UNSPECIFIED";

    switch (wSeverity)
    {
        case SEV_FATAL:
            *retpwOtelNumber = 21;
            *retppszOtelText = "FATAL";
            break;

        case SEV_FAILURE:
            *retpwOtelNumber = 17;
            *retppszOtelText = "ERROR";
            break;

        case SEV_WARNING1:
            *retpwOtelNumber = 13;
            *retppszOtelText = "WARN";
            break;

        case SEV_WARNING2:
            *retpwOtelNumber = 14;
            *retppszOtelText = "WARN";
            break;

        case SEV_NORMAL:
            *retpwOtelNumber = 9;
            *retppszOtelText = "INFO";
            break;

        default:
            break;
    }
}


uint64_t GetEpochNanoseconds (void)
{
    return (uint64_t) time (NULL) * 1000000000ULL;
}


/* Julian Day Number for 1970-01-01 (Unix epoch), per the standard astronomical
   JDN convention that TimeExtractJulianDate follows (day 0 = 4713 B.C.). This
   is a fixed public constant, not a Domino-specific or derived value. */

#define DOMFWD_JULIAN_DAY_UNIX_EPOCH 2440588UL

/* Converts a Domino TIMEDATE (GMT) to Unix epoch nanoseconds by direct
   calculation from its Julian Day Number, rather than diffing against a
   parsed reference TIMEDATE - the SDK exposes no direct TIMEDATE->epoch
   conversion, but TimeExtractJulianDate gives a well-defined day count we
   can convert with a fixed offset. Sub-second resolution comes from
   TimeExtractTicks (10ms ticks), the finest resolution TIMEDATE carries. */

uint64_t GetEventEpochNanoseconds (const TIMEDATE *ptdEventTime)
{
    DWORD    dwJulianDate     = 0;
    DWORD    dwTicks          = 0;
    LONG     lDaysSinceEpoch  = 0;
    DWORD    dwSecondsOfDay   = 0;
    DWORD    dwSubSecondTicks = 0;
    uint64_t ullEpochSeconds  = 0;

    dwJulianDate = TimeExtractJulianDate (ptdEventTime);

    if ((0 == dwJulianDate) || (0xFFFFFFFFUL == dwJulianDate) || (dwJulianDate < DOMFWD_JULIAN_DAY_UNIX_EPOCH))
        return GetEpochNanoseconds ();

    dwTicks = TimeExtractTicks (ptdEventTime);

    lDaysSinceEpoch  = (LONG) (dwJulianDate - DOMFWD_JULIAN_DAY_UNIX_EPOCH);
    dwSecondsOfDay   = dwTicks / TICKS_IN_SECOND;
    dwSubSecondTicks = dwTicks % TICKS_IN_SECOND;

    ullEpochSeconds = ((uint64_t) lDaysSinceEpoch * SECS_IN_DAY) + dwSecondsOfDay;

    return (ullEpochSeconds * 1000000000ULL) + ((uint64_t) dwSubSecondTicks * 10000000ULL);
}


void PrintDelimiter (void)
{
    AddInLogMessageText ("------------------------------------------------------------", 0);
}


void DumpKnownEnums (void)
{
    WORD wType;
    WORD wSeverity;
    WORD wFormat;

    PrintDelimiter ();
    AddInLogMessageText ("Known EVT_xxx types:", 0);

    for (wType = 0; wType < MAX_TYPE; wType++)
        AddInLogMessageText ("  %u = %s", 0, wType, GetEventTypeName (wType));

    AddInLogMessageText ("Known SEV_xxx severities:", 0);

    for (wSeverity = 0; wSeverity <= SEV_NORMAL; wSeverity++)
        AddInLogMessageText ("  %u = %s", 0, wSeverity, GetEventSeverityName (wSeverity));

    AddInLogMessageText ("Known FMT_xxx format specifiers:", 0);

    for (wFormat = FMT_UNKNOWN; wFormat <= FMT_ERROR_MSG; wFormat++)
        AddInLogMessageText ("  %u = %s", 0, wFormat, GetFormatSpecifierName (wFormat));

    PrintDelimiter ();
}


void DumpHex (const BYTE *pData, WORD wDataLen)
{
    WORD wOffset                = 0;
    WORD wIndex                 = 0;
    BYTE ch                     = 0;
    char szLine[MAXSPRINTF + 1] = {0};
    int  nPos                   = 0;

    for (wOffset = 0; wOffset < wDataLen; wOffset += 16)
    {
        nPos = snprintf (szLine, sizeof (szLine), "%04x  ", wOffset);

        for (wIndex = 0; wIndex < 16; wIndex++)
        {
            if ((WORD) (wOffset + wIndex) < wDataLen)
                nPos += snprintf (szLine + nPos, sizeof (szLine) - (size_t) nPos, "%02x ", pData[wOffset + wIndex]);
            else
                nPos += snprintf (szLine + nPos, sizeof (szLine) - (size_t) nPos, "   ");
        }

        nPos += snprintf (szLine + nPos, sizeof (szLine) - (size_t) nPos, " ");

        for (wIndex = 0;
             (wIndex < 16) && ((WORD) (wOffset + wIndex) < wDataLen);
             wIndex++)
        {
            ch = pData[wOffset + wIndex];

            nPos += snprintf (szLine + nPos, sizeof (szLine) - (size_t) nPos, "%c",
                               ((ch >= 32) && (ch < 127)) ? ch : '.');
        }

        AddInLogMessageText ("%s", 0, szLine);
    }
}


void DumpPrintable (const BYTE *pData, WORD wDataLen)
{
    WORD wIndex                 = 0;
    BYTE ch                     = 0;
    char szLine[MAXSPRINTF + 1] = {0};
    int  nPos                   = 0;

    for (wIndex = 0; wIndex < wDataLen; wIndex++)
    {
        ch = pData[wIndex];

        if ((ch >= 32) && (ch < 127))
            nPos += snprintf (szLine + nPos, sizeof (szLine) - (size_t) nPos, "%c", ch);

        else if ('\r' == ch)
            nPos += snprintf (szLine + nPos, sizeof (szLine) - (size_t) nPos, "\\r");

        else if ('\t' == ch)
            nPos += snprintf (szLine + nPos, sizeof (szLine) - (size_t) nPos, "\\t");

        else if ('\n' == ch)
        {
            nPos += snprintf (szLine + nPos, sizeof (szLine) - (size_t) nPos, "\\n");
            AddInLogMessageText ("%s", 0, szLine);
            nPos = 0;
        }

        else
            nPos += snprintf (szLine + nPos, sizeof (szLine) - (size_t) nPos, ".");

        if (nPos >= (int) sizeof (szLine) - 8)
        {
            AddInLogMessageText ("%s", 0, szLine);
            nPos = 0;
        }
    }

    if (nPos > 0)
        AddInLogMessageText ("%s", 0, szLine);
}


/* Resolves standard "core" package error text correctly. For codes in the
   addin-reserved range (IS_PKG(statusCode, PKG_ADDIN) or PKG_ADDIN2,
   globerr.h), the real text lives in the originating servertask's own
   resource/string table, not ours - and there's no API to reach it from
   here: OSLoadString needs an HMODULE for that specific task's already-
   loaded module, and neither AddInFormatErrorText nor AddInFormatError
   offer any way to target a different task. So for addin-range codes this
   returns whatever generic/empty text the core tables have for them - a
   known, unavoidable limitation, not a bug. */

void FormatErrorText (STATUS statusCode, char *retBuffer, size_t bufferLen)
{
    char szFormatted[MAXSPRINTF + 1] = {0};

    AddInFormatErrorText (szFormatted, "%e", statusCode);
    snprintf (retBuffer, bufferLen, "%s", szFormatted);
}


BOOL ConvertLmbcsToUtf8 (const char *pszLmbcsIn, char *pszUtf8Out, DWORD dwUtf8OutSize)
{
    DWORD dwOutLen = 0;

    if ((NULL == pszUtf8Out) || (0 == dwUtf8OutSize))
        return FALSE;

    pszUtf8Out[0] = '\0';

    if (NULL == pszLmbcsIn)
        return TRUE;

    dwOutLen = OSTranslate32 (OS_TRANSLATE_LMBCS_TO_UTF8, pszLmbcsIn, (DWORD) strlen (pszLmbcsIn),
                               pszUtf8Out, dwUtf8OutSize - 1);

    pszUtf8Out[(dwOutLen < dwUtf8OutSize) ? dwOutLen : (dwUtf8OutSize - 1)] = '\0';

    return TRUE;
}


void PopulateEventInfo (EVENT_DATA *pEvent, DOMFWD_EVENT_INFO *retpInfo)
{
    memset (retpInfo, 0, sizeof (DOMFWD_EVENT_INFO));

    retpInfo->wVersion             = pEvent->Version;
    retpInfo->wErrorCode           = pEvent->ErrorCode;
    retpInfo->wAdditionalErrorCode = pEvent->AdditionalErrorCode;
    retpInfo->wType                = pEvent->Type;
    retpInfo->wSeverity            = pEvent->Severity;
    retpInfo->wFormatSpecifier     = pEvent->FormatSpecifier;
    retpInfo->wEventDataLength     = pEvent->EventDataLength;
    retpInfo->wAddinNameLength     = pEvent->AddinNameLength;
    retpInfo->tdEventTime          = pEvent->EventTime;

    snprintf (retpInfo->szOriginatingServerName, sizeof (retpInfo->szOriginatingServerName),
              "%s", pEvent->OriginatingServerName);

    ConvertTIMEDATEtoRFC3339Date (&pEvent->EventTime, retpInfo->szEventTime, sizeof (retpInfo->szEventTime));

    retpInfo->fExtValid = ExtractEventDataExt (pEvent, &retpInfo->ext);
}


WORD ReadWordLE (const BYTE *pData)
{
    return (WORD) ((WORD) pData[0] | ((WORD) pData[1] << 8));
}


void DumpEventSpecificData (const BYTE *pEventData, WORD wEventDataLen, WORD wFormatSpecifier)
{
    const char *pszFirst           = NULL;
    const char *pszSecond          = NULL;
    WORD wFirstLen                 = 0;
    WORD wSecondLen                = 0;
    char szSource[MAXSPRINTF + 1]  = {0};
    char szMessage[MAXSPRINTF + 1] = {0};
    char szText[MAXSPRINTF + 1]    = {0};

    AddInLogMessageText ("EventSpecificData decoded (%s):", 0, GetFormatSpecifierName (wFormatSpecifier));

    switch (wFormatSpecifier)
    {
        case FMT_TEXT:
            snprintf (szText, sizeof (szText), "%.*s", (int) wEventDataLen, (const char *) pEventData);
            AddInLogMessageText ("  Text    : %s", 0, szText);
            break;

        case FMT_ERROR_MSG:
        {
            WORD wCode                      = 0;
            const BYTE *pRemaining          = pEventData;
            WORD wRemainingLen              = wEventDataLen;
            char szCodeLine[MAXSPRINTF + 64] = {0};     /* code text plus the "Code : %u (0x%04x)" prefix */
            char szCodeText[MAXSPRINTF + 1] = {0};

            if (wEventDataLen >= sizeof (WORD))
            {
                wCode = ReadWordLE (pEventData);
                pRemaining = pEventData + 2;
                wRemainingLen = (WORD) (wEventDataLen - 2);
            }

            FormatErrorText ((STATUS) wCode, szCodeText, sizeof (szCodeText));

            snprintf (szCodeLine, sizeof (szCodeLine), "  Code    : %u (0x%04x) %s", wCode, wCode, szCodeText);
            AddInLogMessageText ("%s", 0, szCodeLine);

            pszFirst  = (const char *) pRemaining;
            wFirstLen = (WORD) strnlen (pszFirst, wRemainingLen);

            if ((wFirstLen < wRemainingLen) && ((WORD) (wFirstLen + 1) < wRemainingLen))
            {
                pszSecond  = pszFirst + wFirstLen + 1;
                wSecondLen = (WORD) strnlen (pszSecond, (WORD) (wRemainingLen - wFirstLen - 1));

                snprintf (szSource, sizeof (szSource), "%.*s", (int) wFirstLen, pszFirst);
                AddInLogMessageText ("  Source  : %s", 0, szSource);

                snprintf (szMessage, sizeof (szMessage), "%.*s", (int) wSecondLen, pszSecond);
                AddInLogMessageText ("  Message : %s", 0, szMessage);
            }
            else
            {
                snprintf (szMessage, sizeof (szMessage), "%.*s", (int) wFirstLen, pszFirst);
                AddInLogMessageText ("  Message : %s", 0, szMessage);
            }

            break;
        }

        case FMT_ERROR_CODE:
        {
            WORD wCode = 0;
            char szCodeText[MAXSPRINTF + 1] = {0};

            if (wEventDataLen >= sizeof (WORD))
                wCode = ReadWordLE (pEventData);

            FormatErrorText ((STATUS) wCode, szCodeText, sizeof (szCodeText));

            AddInLogMessageText ("  Code    : %u (0x%04x) %s", 0, wCode, wCode, szCodeText);
            break;
        }

        default:
            AddInLogMessageText ("  (no known decode for this FormatSpecifier)", 0);
            break;
    }
}


void DumpEvent (EVENT_DATA *pEvent, const DOMFWD_EVENT_INFO *pInfo)
{
    PrintDelimiter ();
    AddInLogMessageText ("%s: Event received", 0, g_szTask);
    PrintDelimiter ();

    AddInLogMessageText ("OriginatingServerName : %s", 0, pInfo->szOriginatingServerName);
    AddInLogMessageText ("Version               : %u", 0, pInfo->wVersion);

    if (DOMFWD_EXPECTED_EVENT_VERSION != pInfo->wVersion)
    {
        AddInLogMessageText ("WARNING: Version %u != EVENT_VERSION %u - struct layout assumptions may not hold", 0, pInfo->wVersion, (WORD) EVENT_VERSION);
        return;
    }

    AddInLogMessageText ("ErrorCode             : %u (0x%04x)", 0, pInfo->wErrorCode, pInfo->wErrorCode);
    AddInLogMessageText ("AdditionalErrorCode   : %u (0x%04x)", 0, pInfo->wAdditionalErrorCode, pInfo->wAdditionalErrorCode);

    AddInLogMessageText ("Type                  : %u (%s)", 0, pInfo->wType, GetEventTypeName (pInfo->wType));
    AddInLogMessageText ("Severity              : %u (%s)", 0, pInfo->wSeverity, GetEventSeverityName (pInfo->wSeverity));
    AddInLogMessageText ("EventTime             : %s (%z)", 0, pInfo->szEventTime, &pInfo->tdEventTime);
    AddInLogMessageText ("FormatSpecifier       : %u (%s)", 0, pInfo->wFormatSpecifier, GetFormatSpecifierName (pInfo->wFormatSpecifier));
    AddInLogMessageText ("EventDataLength       : %u", 0, pInfo->wEventDataLength);
    AddInLogMessageText ("AddinNameLength       : %u", 0, pInfo->wAddinNameLength);

    if (pInfo->fExtValid)
    {
        AddInLogMessageText ("ExtAddinName          : %s", 0, pInfo->ext.szAddinName);
        AddInLogMessageText ("ExtExecName           : %s", 0, pInfo->ext.szExecName);
        AddInLogMessageText ("ExtPIDTID             : %s", 0, pInfo->ext.szPIDTID);
        AddInLogMessageText ("ExtTargetServer       : %s", 0, pInfo->ext.szTargetServer);
        AddInLogMessageText ("ExtTargetDatabase     : %s", 0, pInfo->ext.szTargetDatabase);
        AddInLogMessageText ("ExtTargetUser         : %s", 0, pInfo->ext.szTargetUser);
        AddInLogMessageText ("ExtErrorCode          : %s", 0, pInfo->ext.szErrorCode);
        AddInLogMessageText ("ExtErrorText          : %s", 0, pInfo->ext.szErrorText);
    }
    else
    {
        AddInLogMessageText ("ExtData               : skipped - Version %u != EVENT_VERSION %u", 0, pInfo->wVersion, (WORD) EVENT_VERSION);
    }

    DumpEventSpecificData (&pEvent->EventSpecificData, pInfo->wEventDataLength, pInfo->wFormatSpecifier);

    PrintDelimiter ();
}


BOOL EnsureDirectoryExists (const char *pszPath)
{
#if defined (_WIN32)
    if (0 == _mkdir (pszPath))
        return TRUE;
#else
    if (0 == mkdir (pszPath, 0755))
        return TRUE;
#endif

    return (EEXIST == errno) ? TRUE : FALSE;
}


BOOL BuildEventLogPath (size_t cbPath, char *retszPath)
{
    char szDataDir[MAXPATH + 1]   = {0};
    char szEventDir[MAXPATH + 16] = {0};    /* data dir + "domino" */
    char szLogDir[MAXPATH + 32]   = {0};    /* event dir + separator + "logs" */

    OSGetDataDirectory (szDataDir);
    OSPathAddTrailingPathSeparator (szDataDir, (WORD) (sizeof (szDataDir) - 1));

    snprintf (szEventDir, sizeof (szEventDir), "%sdomino", szDataDir);
    snprintf (szLogDir, sizeof (szLogDir), "%s%slogs", szEventDir, DOMFWD_PATHSEP);

    if (!EnsureDirectoryExists (szEventDir))
        AddInLogMessageText ("%s: Failed to create directory: %s", 0, g_szTask, szEventDir);

    if (!EnsureDirectoryExists (szLogDir))
        AddInLogMessageText ("%s: Failed to create directory: %s", 0, g_szTask, szLogDir);

    snprintf (retszPath, cbPath, "%s%sdomino-events.json", szLogDir, DOMFWD_PATHSEP);

    return TRUE;
}


STATUS GetLocalServerName (void)
{
    return SECKFMGetUserName (g_szLocalServerName);
}


STATUS OpenAddressBook (DBHANDLE *retphAddressBook)
{
    if (NULL != retphAddressBook)
        *retphAddressBook = NULLHANDLE;

    if (NULL == retphAddressBook)
        return ERR_MISC_INVALID_ARGS;

    return NSFDbOpen ("names.nsf", retphAddressBook);
}


STATUS FindServerDocument (DBHANDLE hAddressBook, const char *pszServerName, NOTEHANDLE *retphNote)
{
    STATUS error       = NOERROR;
    NOTEID EntryNoteID = 0;

    if (NULL != retphNote)
        *retphNote = NULLHANDLE;

    if ((NULLHANDLE == hAddressBook) || (NULL == pszServerName) || ('\0' == *pszServerName) || (NULL == retphNote))
        return ERR_MISC_INVALID_ARGS;

    error = REGFindAddressBookEntry (hAddressBook, NETAUTH_NAMESPACE_SERVERS, (char *) pszServerName, &EntryNoteID);

    if (error)
        return error;

    return NSFNoteOpen (hAddressBook, EntryNoteID, 0, retphNote);
}


void StripVersionPrefix (char *pszVersion)
{
    static const char *pszPrefixes[] = { "Release ", "Build " };
    size_t i = 0;

    for (i = 0; i < (sizeof (pszPrefixes) / sizeof (pszPrefixes[0])); i++)
    {
        if (StripPrefix (pszVersion, pszPrefixes[i]))
            return;
    }
}


STATUS LookupServerInfo (void)
{
    STATUS     error        = NOERROR;
    DBHANDLE   hAddressBook = NULLHANDLE;
    NOTEHANDLE hNote        = NULLHANDLE;

    error = OpenAddressBook (&hAddressBook);

    if (error)
        return error;

    error = FindServerDocument (hAddressBook, g_szLocalServerName, &hNote);

    if (error)
    {
        NSFDbClose (hAddressBook);
        return error;
    }

    NSFItemGetText (hNote, "SMTPFullHostDomain", g_szHostName, sizeof (g_szHostName) - 1);
    NSFItemGetText (hNote, "ServerBuildNumber", g_szDominoVersion, sizeof (g_szDominoVersion) - 1);
    StripVersionPrefix (g_szDominoVersion);

    NSFNoteClose (hNote);
    NSFDbClose (hAddressBook);

    return NOERROR;
}


/* Direct OTLP push config (libcurl), read from notes.ini once at startup.
   Pushing is entirely opt-in - PushEventToOtel no-ops if the URL isn't set.
   Only used in builds with libcurl support (DOMFWD_CURL).
   If DOMFWD_OtelCaFile isn't set, defaults to <datadir>/cacert.pem - the
   same location several Domino subsystems already use for a TLS trust
   bundle - but only if that file actually exists. */

void LoadOtelConfig (void)
{
    char szDataDir[MAXPATH + 1] = {0};
    FILE *pCaFileTest           = NULL;

    OSGetEnvironmentString ("DOMFWD_OtelPushURL",   g_szOtelPushURL,   sizeof (g_szOtelPushURL) - 1);
    OSGetEnvironmentString ("DOMFWD_OtelPushToken", g_szOtelPushToken, sizeof (g_szOtelPushToken) - 1);

    if (!OSGetEnvironmentString ("DOMFWD_OtelCaFile", g_szOtelCaFile, sizeof (g_szOtelCaFile) - 1))
    {
        OSGetDataDirectory (szDataDir);
        OSPathAddTrailingPathSeparator (szDataDir, (WORD) (sizeof (szDataDir) - 1));

        snprintf (g_szOtelCaFile, sizeof (g_szOtelCaFile), "%scacert.pem", szDataDir);

        pCaFileTest = fopen (g_szOtelCaFile, "r");

        if (NULL != pCaFileTest)
            fclose (pCaFileTest);
        else
            g_szOtelCaFile[0] = '\0';
    }
}


/* Local trace file with every record sent, for troubleshooting. Only written with DOMFWD_TraceFile=1 in notes.ini.
   The file is appended to and never rotated. */
BOOL OpenEventLogFile (void)
{
    char szPath[MAXPATH + 64] = {0};        /* log dir + separator + "domino-events.json" */

    if (0 == OSGetEnvironmentInt ("DOMFWD_TraceFile"))
        return FALSE;

    BuildEventLogPath (sizeof (szPath), szPath);

    g_pEventLogFile = fopen (szPath, "a");

    if (NULL == g_pEventLogFile)
    {
        AddInLogMessageText ("%s: Failed to open trace file: %s", 0, g_szTask, szPath);
        return FALSE;
    }

    AddInLogMessageText ("%s: Trace file enabled (DOMFWD_TraceFile), writing JSON events to: %s", 0, g_szTask, szPath);

    return TRUE;
}


void BuildOtelFields (const DOMFWD_EVENT_INFO *pInfo, DOMFWD_OTEL_FIELDS *retpFields)
{
    TIMEDATE tdNow = {0};

    memset (retpFields, 0, sizeof (DOMFWD_OTEL_FIELDS));

    OSCurrentTIMEDATE (&tdNow);

    snprintf (retpFields->szTimeUnixNano, sizeof (retpFields->szTimeUnixNano), "%llu", (unsigned long long) GetEventEpochNanoseconds (&pInfo->tdEventTime));
    snprintf (retpFields->szObservedTimeUnixNano, sizeof (retpFields->szObservedTimeUnixNano), "%llu", (unsigned long long) GetEventEpochNanoseconds (&tdNow));

    GetOtelSeverity (pInfo->wSeverity, &retpFields->wSeverityNumber, &retpFields->pszSeverityText);

    ConvertLmbcsToUtf8 (pInfo->szOriginatingServerName, retpFields->szServerUtf8, sizeof (retpFields->szServerUtf8));

    if (pInfo->fExtValid)
    {
        ConvertLmbcsToUtf8 (pInfo->ext.szAddinName,      retpFields->szAddinNameUtf8,    sizeof (retpFields->szAddinNameUtf8));
        ConvertLmbcsToUtf8 (pInfo->ext.szExecName,       retpFields->szExecNameUtf8,     sizeof (retpFields->szExecNameUtf8));
        ConvertLmbcsToUtf8 (pInfo->ext.szTargetServer,   retpFields->szTargetServerUtf8, sizeof (retpFields->szTargetServerUtf8));
        ConvertLmbcsToUtf8 (pInfo->ext.szTargetDatabase, retpFields->szTargetDbUtf8,     sizeof (retpFields->szTargetDbUtf8));
        ConvertLmbcsToUtf8 (pInfo->ext.szTargetUser,     retpFields->szTargetUserUtf8,   sizeof (retpFields->szTargetUserUtf8));
        ConvertLmbcsToUtf8 (pInfo->ext.szErrorText,      retpFields->szErrorTextUtf8,    sizeof (retpFields->szErrorTextUtf8));

        /* PIDTID is hex "PID:count-tid" (e.g. "8AF0:0002-CC18"). strtoul stops
           at the colon, so this cleanly lifts out just the PID. The part after
           the colon isn't split yet - see domino.process.pidtid - pending
           confirmation the format holds on Linux. */

        retpFields->dwProcessId = (DWORD) strtoul (pInfo->ext.szPIDTID, NULL, 16);
    }

    retpFields->pszBody = retpFields->szErrorTextUtf8;

    /* ---- resource key/values ---- */

    AddKV (retpFields->ResourceKV, &retpFields->wResourceKVCount, DOMFWD_MAX_RESOURCE_KV, "service.name",        "domino");
    AddKV (retpFields->ResourceKV, &retpFields->wResourceKVCount, DOMFWD_MAX_RESOURCE_KV, "service.instance.id", retpFields->szServerUtf8);
    AddKV (retpFields->ResourceKV, &retpFields->wResourceKVCount, DOMFWD_MAX_RESOURCE_KV, "host.name",           g_szHostName[0] ? g_szHostName : retpFields->szServerUtf8);
    AddKV (retpFields->ResourceKV, &retpFields->wResourceKVCount, DOMFWD_MAX_RESOURCE_KV, "os.type",             DOMFWD_OS_TYPE);

    if (g_szDominoVersion[0])
        AddKV (retpFields->ResourceKV, &retpFields->wResourceKVCount, DOMFWD_MAX_RESOURCE_KV, "service.version", g_szDominoVersion);

    if (pInfo->fExtValid)
    {
        AddKV    (retpFields->ResourceKV, &retpFields->wResourceKVCount, DOMFWD_MAX_RESOURCE_KV, "process.executable.name", retpFields->szExecNameUtf8);
        AddKVInt (retpFields->ResourceKV, &retpFields->wResourceKVCount, DOMFWD_MAX_RESOURCE_KV, "process.pid", (LONG) retpFields->dwProcessId);
        AddKV    (retpFields->ResourceKV, &retpFields->wResourceKVCount, DOMFWD_MAX_RESOURCE_KV, "domino.process.pidtid", pInfo->ext.szPIDTID);
    }

    /* ---- log record attribute key/values ---- */

    AddKVInt (retpFields->AttributeKV, &retpFields->wAttributeKVCount, DOMFWD_MAX_ATTRIBUTE_KV, "domino.event.type_code",       (LONG) pInfo->wType);
    AddKV    (retpFields->AttributeKV, &retpFields->wAttributeKVCount, DOMFWD_MAX_ATTRIBUTE_KV, "domino.event.type",            GetEventTypeName (pInfo->wType));
    AddKV    (retpFields->AttributeKV, &retpFields->wAttributeKVCount, DOMFWD_MAX_ATTRIBUTE_KV, "domino.event.time",            pInfo->szEventTime);
    AddKVInt (retpFields->AttributeKV, &retpFields->wAttributeKVCount, DOMFWD_MAX_ATTRIBUTE_KV, "domino.error.code",            (LONG) pInfo->wErrorCode);
    AddKV    (retpFields->AttributeKV, &retpFields->wAttributeKVCount, DOMFWD_MAX_ATTRIBUTE_KV, "domino.error.text",            retpFields->szErrorTextUtf8);
    AddKVInt (retpFields->AttributeKV, &retpFields->wAttributeKVCount, DOMFWD_MAX_ATTRIBUTE_KV, "domino.error.additional_code", (LONG) pInfo->wAdditionalErrorCode);

    if (pInfo->fExtValid)
    {
        AddKV (retpFields->AttributeKV, &retpFields->wAttributeKVCount, DOMFWD_MAX_ATTRIBUTE_KV, "domino.addin.name",       retpFields->szAddinNameUtf8);
        AddKV (retpFields->AttributeKV, &retpFields->wAttributeKVCount, DOMFWD_MAX_ATTRIBUTE_KV, "domino.target.server",    retpFields->szTargetServerUtf8);
        AddKV (retpFields->AttributeKV, &retpFields->wAttributeKVCount, DOMFWD_MAX_ATTRIBUTE_KV, "domino.target.database",  retpFields->szTargetDbUtf8);
        AddKV (retpFields->AttributeKV, &retpFields->wAttributeKVCount, DOMFWD_MAX_ATTRIBUTE_KV, "domino.target.user",     retpFields->szTargetUserUtf8);
    }
}


void BuildEventPayload (const DOMFWD_OTEL_FIELDS *pFields, rapidjson::StringBuffer *retpBuffer)
{
    rapidjson::Document doc;
    rapidjson::Document::AllocatorType &alloc = doc.GetAllocator ();
    rapidjson::Writer<rapidjson::StringBuffer> writer (*retpBuffer);

    doc.SetObject ();

    doc.AddMember ("time_unix_nano",          rapidjson::Value (pFields->szTimeUnixNano, alloc), alloc);
    doc.AddMember ("observed_time_unix_nano", rapidjson::Value (pFields->szObservedTimeUnixNano, alloc), alloc);
    doc.AddMember ("severity_number",         pFields->wSeverityNumber, alloc);
    doc.AddMember ("severity_text",           rapidjson::Value (pFields->pszSeverityText, alloc), alloc);
    doc.AddMember ("body",                    rapidjson::Value (pFields->pszBody, alloc), alloc);

    /* scope: identifies the emitting instrumentation, per the OTel Log Data Model. The version is the one of this add-in: the version of
       the Domino server is the resource attribute service.version */
    rapidjson::Value scope (rapidjson::kObjectType);
    scope.AddMember ("name",    rapidjson::Value (g_szScope, alloc), alloc);
    scope.AddMember ("version", rapidjson::Value (DOMFWD_VERSION, alloc), alloc);
    doc.AddMember ("scope", scope, alloc);

    /* resource/attributes: field list built once in BuildOtelFields */
    rapidjson::Value resource (rapidjson::kObjectType);
    AddKVsToJsonObject (resource, pFields->ResourceKV, pFields->wResourceKVCount, alloc);
    doc.AddMember ("resource", resource, alloc);

    rapidjson::Value attributes (rapidjson::kObjectType);
    AddKVsToJsonObject (attributes, pFields->AttributeKV, pFields->wAttributeKVCount, alloc);
    doc.AddMember ("attributes", attributes, alloc);

    doc.Accept (writer);
}


void WriteEventJson (const rapidjson::StringBuffer *pBuffer)
{
    if (NULL == g_pEventLogFile)
        return;

    fwrite (pBuffer->GetString (), 1, pBuffer->GetSize (), g_pEventLogFile);
    fputc ('\n', g_pEventLogFile);
    fflush (g_pEventLogFile);
}


#ifdef DOMFWD_CURL

void AddOtlpStringAttr (rapidjson::Value &retAttrs, const char *pszKey, const char *pszValue, rapidjson::Document::AllocatorType &alloc)
{
    rapidjson::Value attr (rapidjson::kObjectType);
    rapidjson::Value val (rapidjson::kObjectType);

    val.AddMember ("stringValue", rapidjson::Value (pszValue, alloc), alloc);
    attr.AddMember ("key",   rapidjson::Value (pszKey, alloc), alloc);
    attr.AddMember ("value", val, alloc);

    retAttrs.PushBack (attr, alloc);
}


BOOL SendOtelPayload (const char *pszBuffer, size_t BufferLen)
{
    CURL     *pCurl              = NULL;
    CURLcode  error              = CURLE_OK;
    struct curl_slist *pHeaders  = NULL;
    BOOL      fSuccess           = FALSE;
    char      szAuthHeader[600]  = {0};

    if ('\0' == g_szOtelPushURL[0])
        return FALSE;

    pCurl = curl_easy_init ();

    if (NULL == pCurl)
        return FALSE;

    pHeaders = curl_slist_append (pHeaders, "Content-Type: application/json");

    if (g_szOtelPushToken[0])
    {
        snprintf (szAuthHeader, sizeof (szAuthHeader), "Authorization: Bearer %s", g_szOtelPushToken);
        pHeaders = curl_slist_append (pHeaders, szAuthHeader);
    }

    curl_easy_setopt (pCurl, CURLOPT_HTTPHEADER,    pHeaders);
    curl_easy_setopt (pCurl, CURLOPT_URL,           g_szOtelPushURL);
    curl_easy_setopt (pCurl, CURLOPT_POST,          1L);
    curl_easy_setopt (pCurl, CURLOPT_POSTFIELDS,    pszBuffer);
    curl_easy_setopt (pCurl, CURLOPT_POSTFIELDSIZE, (long) BufferLen);
    curl_easy_setopt (pCurl, CURLOPT_TIMEOUT,       15L);

    if (g_szOtelCaFile[0])
        curl_easy_setopt (pCurl, CURLOPT_CAINFO, g_szOtelCaFile);

    error = curl_easy_perform (pCurl);

    fSuccess = (CURLE_OK == error);

    if (!fSuccess)
        AddInLogMessageText ("%s: OTLP push failed: %s", 0, g_szTask, curl_easy_strerror (error));

    curl_slist_free_all (pHeaders);
    curl_easy_cleanup (pCurl);

    return fSuccess;
}


void PushEventToOtel (const DOMFWD_OTEL_FIELDS *pFields, const rapidjson::StringBuffer *pPayloadBuffer)
{
    if ('\0' == g_szOtelPushURL[0])
        return;

    rapidjson::Document doc;
    rapidjson::Document::AllocatorType &alloc = doc.GetAllocator ();
    doc.SetObject ();

    /* ---- resource: minimal, just enough for stream routing ---- */
    rapidjson::Value resourceAttrs (rapidjson::kArrayType);
    AddOtlpStringAttr (resourceAttrs, "service.name", "domino", alloc);

    rapidjson::Value resource (rapidjson::kObjectType);
    resource.AddMember ("attributes", resourceAttrs, alloc);

    /* ---- scope: the same as in the record of the socket path (BuildEventPayload), so both paths look alike ---- */
    rapidjson::Value scope (rapidjson::kObjectType);
    scope.AddMember ("name",    rapidjson::Value (g_szScope, alloc), alloc);
    scope.AddMember ("version", rapidjson::Value (DOMFWD_VERSION, alloc), alloc);

    /* ---- logRecord: body is the already-built payload buffer, verbatim ---- */
    rapidjson::Value logRecord (rapidjson::kObjectType);
    logRecord.AddMember ("timeUnixNano",         rapidjson::Value (pFields->szTimeUnixNano, alloc), alloc);
    logRecord.AddMember ("observedTimeUnixNano", rapidjson::Value (pFields->szObservedTimeUnixNano, alloc), alloc);
    logRecord.AddMember ("severityNumber",       pFields->wSeverityNumber, alloc);
    logRecord.AddMember ("severityText",         rapidjson::Value (pFields->pszSeverityText, alloc), alloc);

    rapidjson::Value body (rapidjson::kObjectType);
    body.AddMember ("stringValue", rapidjson::Value (pPayloadBuffer->GetString (), alloc), alloc);
    logRecord.AddMember ("body", body, alloc);

    rapidjson::Value logRecords (rapidjson::kArrayType);
    logRecords.PushBack (logRecord, alloc);

    rapidjson::Value scopeLog (rapidjson::kObjectType);
    scopeLog.AddMember ("scope", scope, alloc);
    scopeLog.AddMember ("logRecords", logRecords, alloc);

    rapidjson::Value scopeLogs (rapidjson::kArrayType);
    scopeLogs.PushBack (scopeLog, alloc);

    rapidjson::Value resourceLog (rapidjson::kObjectType);
    resourceLog.AddMember ("resource", resource, alloc);
    resourceLog.AddMember ("scopeLogs", scopeLogs, alloc);

    rapidjson::Value resourceLogs (rapidjson::kArrayType);
    resourceLogs.PushBack (resourceLog, alloc);

    doc.AddMember ("resourceLogs", resourceLogs, alloc);

    rapidjson::StringBuffer otlpBuffer;
    rapidjson::Writer<rapidjson::StringBuffer> writer (otlpBuffer);
    doc.Accept (writer);

    SendOtelPayload (otlpBuffer.GetString (), otlpBuffer.GetSize ());
}

#endif /* DOMFWD_CURL */


void SocketLog (const char *pszMessage)
{
    AddInLogMessageText ("%s: Socket: %s", 0, g_szTask, pszMessage);
}


/* Reads DOMFWD_Socket, DOMFWD_SocketWAL and DOMFWD_SocketWALMaxMB from notes.ini once at startup and prepares the sender. It
   connects later in the main loop. The WAL is only used on Linux: on Windows a setting is reported, and events wait in memory only. */
void LoadSocketConfig (void)
{
    char     szError[256]   = {0};
    char     szMessage[600] = {0};
    long     WalMaxMB       = 0;
    uint64_t WalMaxBytes    = 0;

    OSGetEnvironmentString ("DOMFWD_Socket", g_szSocketTarget, sizeof (g_szSocketTarget) - 1);

    if (0 == strcmp (g_szSocketTarget, "off"))
    {
        g_szSocketTarget[0] = '\0';
        return;
    }

#if !defined (_WIN32)
    /* Default on Linux: the Unix socket of otelfwd -nostdin. Not used when a direct push is configured. */
    if (('\0' == g_szSocketTarget[0]) && ('\0' == g_szOtelPushURL[0]))
    {
        char szDataDir[MAXPATH + 1] = {0};

        OSGetDataDirectory (szDataDir);
        snprintf (g_szSocketTarget, sizeof (g_szSocketTarget), "unix:%s/domino/otelfwd.sock", szDataDir);
    }
#endif

    if ('\0' == g_szSocketTarget[0])
        return;

    /* The WAL: events wait on disk while the forwarder is not reachable, also when this program or the server stops.
       DOMFWD_SocketWAL=off switches it off. Default on Linux: <data>/domino/domfwd.wal */
    OSGetEnvironmentString ("DOMFWD_SocketWAL", g_szSocketWal, sizeof (g_szSocketWal) - 1);

    if (0 == strcmp (g_szSocketWal, "off"))
    {
        g_szSocketWal[0] = '\0';
    }
#if !defined (_WIN32)
    else if ('\0' == g_szSocketWal[0])
    {
        char szDataDir[MAXPATH + 1]    = {0};
        char szDominoDir[MAXPATH + 16] = {0};

        OSGetDataDirectory (szDataDir);
        snprintf (szDominoDir, sizeof (szDominoDir), "%s/domino", szDataDir);
        EnsureDirectoryExists (szDominoDir);
        snprintf (g_szSocketWal, sizeof (g_szSocketWal), "%s/domfwd.wal", szDominoDir);
    }
#endif

    /* The largest size of the WAL. Not set or 0: the default. An event record is about 1 KB: 128 MB are roughly 150,000 events */
    WalMaxMB = (long) OSGetEnvironmentInt ("DOMFWD_SocketWALMaxMB");

    if (WalMaxMB <= 0)
        WalMaxMB = DOMFWD_SOCKET_WAL_DEFAULT_MB;

    WalMaxBytes = (uint64_t) WalMaxMB * 1024 * 1024;
    g_SocketWalMaxBytes = WalMaxBytes;

    if (g_SocketSender.Configure (g_szSocketTarget, SocketLog, DOMFWD_SOCKET_MAX_LINES, DOMFWD_SOCKET_MAX_BYTES, g_szSocketWal, WalMaxBytes, szError, sizeof (szError)))
    {
        AddInLogMessageText ("%s: Forwarding events to %s", 0, g_szTask, g_szSocketTarget);

        if (g_SocketSender.HasWal ())
        {
            snprintf (szMessage, sizeof (szMessage), "Events wait in the WAL %s (at most %ld MB) while the forwarder is not reachable", g_szSocketWal, WalMaxMB);
            AddInLogMessageText ("%s: Socket: %s", 0, g_szTask, szMessage);

            if (g_SocketSender.GetWalSize () > 0)
            {
                snprintf (szMessage, sizeof (szMessage), "The WAL holds %lu bytes of events of an earlier run. They are sent when the forwarder is reachable", (unsigned long) g_SocketSender.GetWalSize ());
                AddInLogMessageText ("%s: Socket: %s", 0, g_szTask, szMessage);
            }
        }
    }
    else
    {
        AddInLogMessageText ("%s: Invalid DOMFWD_Socket setting \"%s\": %s", 0, g_szTask, g_szSocketTarget, szError);
    }
}


/* Never blocks: the line is queued, or written to the WAL if the forwarder is not there, and sent as far as the connection allows
   right now */
void SendEventToSocket (const rapidjson::StringBuffer *pPayloadBuffer)
{
    if (false == g_SocketSender.IsConfigured ())
        return;

    g_SocketSender.Send (pPayloadBuffer->GetString (), pPayloadBuffer->GetSize ());
    g_SocketSender.Pump ();
}


/* At shutdown: gives the queue a moment to drain. What could not be sent goes to the WAL. Logs what happened and closes the connection */
void ShutdownSocket (void)
{
    char szStats[320] = {0};

    if (false == g_SocketSender.IsConfigured ())
        return;

    if (false == g_SocketSender.Flush (DOMFWD_SOCKET_FLUSH_MS))
        AddInLogMessageText ("%s: Socket: %u events could not be delivered to the forwarder%s", 0, g_szTask, (unsigned) g_SocketSender.GetQueuedLines (),
                             g_SocketSender.HasWal () ? " and could not be stored in the WAL" : "");

    if (g_SocketSender.HasWal () && (g_SocketSender.GetWalSize () > 0))
    {
        snprintf (szStats, sizeof (szStats), "%lu bytes of events wait in the WAL for the next start", (unsigned long) g_SocketSender.GetWalSize ());
        AddInLogMessageText ("%s: Socket: %s", 0, g_szTask, szStats);
    }

    if (g_SocketSender.HasWal ())
        snprintf (szStats, sizeof (szStats), "%lu sent, %lu dropped, %lu written to the WAL, %lu taken from it",
                  (unsigned long) g_SocketSender.GetSent (), (unsigned long) g_SocketSender.GetDropped (),
                  (unsigned long) g_SocketSender.GetSpilled (), (unsigned long) g_SocketSender.GetDrained ());
    else
        snprintf (szStats, sizeof (szStats), "%lu sent, %lu dropped", (unsigned long) g_SocketSender.GetSent (), (unsigned long) g_SocketSender.GetDropped ());

    AddInLogMessageText ("%s: Socket: %s", 0, g_szTask, szStats);

    g_SocketSender.Close ();
}


/* Metrics file for the Prometheus node_exporter textfile collector: <data>/domino/stats/domfwd.prom
   Written to a temporary file first and then renamed, so a reader never sees a half written file.
   DOMFWD_PromFile=off in notes.ini disables it. */
void LoadPromConfig (void)
{
    char szValue[32] = {0};

    OSGetEnvironmentString ("DOMFWD_PromFile", szValue, sizeof (szValue) - 1);

    if (0 == strcmp (szValue, "off"))
    {
        g_fPromEnabled = FALSE;
        AddInLogMessageText ("%s: Metrics file disabled (DOMFWD_PromFile=off)", 0, g_szTask);
    }
}


void HealthLog (const char *pszMessage)
{
    AddInLogMessageText ("%s: %s", 0, g_szTask, pszMessage);
}


/* Health for alerting: gives what is known now to the monitor, see ../health.hpp. Only the socket to the forwarder is watched:
   the direct push has no counters. Called from the main loop, also when the metrics file is off */
void UpdateHealth (void)
{
    HealthInput Input;

    if (false == g_SocketSender.IsConfigured ())
        return;

    Input.bUnreachable = (false == g_SocketSender.IsConnected ());
    Input.Dropped      = g_SocketSender.GetDropped ();
    Input.Rejected     = g_SocketSender.GetRejected ();

    /* A WAL was wanted: a path is set. It is not available on Windows, which is not an error there */
#if !defined (_WIN32)
    Input.bWalFailed  = ('\0' != g_szSocketWal[0]) && (false == g_SocketSender.HasWal ());
#endif

    if (g_SocketSender.HasWal ())
    {
        Input.WalBytes    = g_SocketSender.GetWalSize ();
        Input.WalMaxBytes = g_SocketWalMaxBytes;
    }

    g_Health.Update (time (NULL), Input, HealthLog);
}


void WritePromFile (void)
{
    char  szDataDir[MAXPATH + 1]    = {0};
    char  szDomino[MAXPATH + 16]    = {0};
    char  szStatsDir[MAXPATH + 32]  = {0};
    char  szFile[MAXPATH + 64]      = {0};
    char  szTmp[MAXPATH + 72]       = {0};
    FILE *pFile                     = NULL;

    if (FALSE == g_fPromEnabled)
        return;

    OSGetDataDirectory (szDataDir);
    OSPathAddTrailingPathSeparator (szDataDir, (WORD) (sizeof (szDataDir) - 1));

    snprintf (szDomino,   sizeof (szDomino),   "%sdomino", szDataDir);
    snprintf (szStatsDir, sizeof (szStatsDir), "%s%sstats", szDomino, DOMFWD_PATHSEP);
    snprintf (szFile,     sizeof (szFile),     "%s%sdomfwd.prom", szStatsDir, DOMFWD_PATHSEP);
    snprintf (szTmp,      sizeof (szTmp),      "%s.tmp", szFile);

    /* Created on the first write. Fails silently later if it already exists */
    EnsureDirectoryExists (szDomino);
    EnsureDirectoryExists (szStatsDir);

    pFile = fopen (szTmp, "w");

    if (NULL == pFile)
        return;

    fprintf (pFile, "# HELP domfwd_build_number domfwd Build Version %s\n", DOMFWD_VERSION);
    fprintf (pFile, "# TYPE domfwd_build_number gauge\n");
    fprintf (pFile, "domfwd_build_number %d\n", DOMFWD_VERSION_BUILD);

    fprintf (pFile, "# HELP domfwd_started_timestamp_seconds Unix timestamp when the add-in was started\n");
    fprintf (pFile, "# TYPE domfwd_started_timestamp_seconds gauge\n");
    fprintf (pFile, "domfwd_started_timestamp_seconds %lu\n", (unsigned long) g_tStartTime);

    fprintf (pFile, "# HELP domfwd_uptime_seconds Uptime in seconds\n");
    fprintf (pFile, "# TYPE domfwd_uptime_seconds gauge\n");
    fprintf (pFile, "domfwd_uptime_seconds %lu\n", (unsigned long) (time (NULL) - g_tStartTime));

    fprintf (pFile, "# HELP domfwd_lastupdate_timestamp_seconds Unix timestamp for last metrics update\n");
    fprintf (pFile, "# TYPE domfwd_lastupdate_timestamp_seconds gauge\n");
    fprintf (pFile, "domfwd_lastupdate_timestamp_seconds %lu\n", (unsigned long) time (NULL));

    fprintf (pFile, "# HELP domfwd_events_received_total Events read from the Domino event queue\n");
    fprintf (pFile, "# TYPE domfwd_events_received_total counter\n");
    fprintf (pFile, "domfwd_events_received_total %lu\n", g_ulEventsReceived);

    fprintf (pFile, "# HELP domfwd_events_skipped_total Events not forwarded on purpose\n");
    fprintf (pFile, "# TYPE domfwd_events_skipped_total counter\n");
    fprintf (pFile, "domfwd_events_skipped_total{reason=\"own\"} %lu\n", g_ulEventsSkippedOwn);

    fprintf (pFile, "# HELP domfwd_events_dropped_total Events which could not be processed\n");
    fprintf (pFile, "# TYPE domfwd_events_dropped_total counter\n");
    fprintf (pFile, "domfwd_events_dropped_total{reason=\"version\"} %lu\n", g_ulEventsDroppedVersion);

    fprintf (pFile, "# HELP domfwd_last_event_timestamp_seconds Time of the last event received, own events excluded (0 = none since start)\n");
    fprintf (pFile, "# TYPE domfwd_last_event_timestamp_seconds gauge\n");
    fprintf (pFile, "domfwd_last_event_timestamp_seconds %lu\n", (unsigned long) g_tLastEvent);

    if (g_SocketSender.IsConfigured ())
    {
        fprintf (pFile, "# HELP domfwd_socket_sent_total Records sent to the forwarder\n");
        fprintf (pFile, "# TYPE domfwd_socket_sent_total counter\n");
        fprintf (pFile, "domfwd_socket_sent_total %llu\n", (unsigned long long) g_SocketSender.GetSent ());

        fprintf (pFile, "# HELP domfwd_socket_dropped_total Records dropped because the queue was full, and the WAL too (or there is none)\n");
        fprintf (pFile, "# TYPE domfwd_socket_dropped_total counter\n");
        fprintf (pFile, "domfwd_socket_dropped_total %llu\n", (unsigned long long) g_SocketSender.GetDropped ());

        fprintf (pFile, "# HELP domfwd_socket_rejected_total Records rejected by the sender (invalid line)\n");
        fprintf (pFile, "# TYPE domfwd_socket_rejected_total counter\n");
        fprintf (pFile, "domfwd_socket_rejected_total %llu\n", (unsigned long long) g_SocketSender.GetRejected ());

        fprintf (pFile, "# HELP domfwd_socket_connects_total Connections established to the forwarder\n");
        fprintf (pFile, "# TYPE domfwd_socket_connects_total counter\n");
        fprintf (pFile, "domfwd_socket_connects_total %llu\n", (unsigned long long) g_SocketSender.GetConnects ());

        fprintf (pFile, "# HELP domfwd_socket_queued Records waiting to be sent\n");
        fprintf (pFile, "# TYPE domfwd_socket_queued gauge\n");
        fprintf (pFile, "domfwd_socket_queued %lu\n", (unsigned long) g_SocketSender.GetQueuedLines ());

        fprintf (pFile, "# HELP domfwd_socket_connected 1 if connected to the forwarder\n");
        fprintf (pFile, "# TYPE domfwd_socket_connected gauge\n");
        fprintf (pFile, "domfwd_socket_connected %d\n", g_SocketSender.IsConnected () ? 1 : 0);

        fprintf (pFile, "# HELP domfwd_health Health for alerting: 0 is OK, 1 is a warning, 2 is an error\n");
        fprintf (pFile, "# TYPE domfwd_health gauge\n");
        fprintf (pFile, "domfwd_health %d\n", g_Health.GetState ());

        /* Only with a WAL */
        if (g_SocketSender.HasWal ())
        {
            fprintf (pFile, "# HELP domfwd_socket_wal_bytes Size of the WAL: events which wait on disk for the forwarder\n");
            fprintf (pFile, "# TYPE domfwd_socket_wal_bytes gauge\n");
            fprintf (pFile, "domfwd_socket_wal_bytes %llu\n", (unsigned long long) g_SocketSender.GetWalSize ());

            fprintf (pFile, "# HELP domfwd_socket_wal_written_total Records written to the WAL because the forwarder was not connected or records waited\n");
            fprintf (pFile, "# TYPE domfwd_socket_wal_written_total counter\n");
            fprintf (pFile, "domfwd_socket_wal_written_total %llu\n", (unsigned long long) g_SocketSender.GetSpilled ());

            fprintf (pFile, "# HELP domfwd_socket_wal_taken_total Records taken out of the WAL and given to the sender\n");
            fprintf (pFile, "# TYPE domfwd_socket_wal_taken_total counter\n");
            fprintf (pFile, "domfwd_socket_wal_taken_total %llu\n", (unsigned long long) g_SocketSender.GetDrained ());

            fprintf (pFile, "# HELP domfwd_socket_wal_refused_total Records which the WAL did not take (it was full, or a failure). They are dropped\n");
            fprintf (pFile, "# TYPE domfwd_socket_wal_refused_total counter\n");
            fprintf (pFile, "domfwd_socket_wal_refused_total %llu\n", (unsigned long long) g_SocketSender.GetWalRefused ());
        }
    }

    fclose (pFile);

#if defined (_WIN32)
    MoveFileExA (szTmp, szFile, MOVEFILE_REPLACE_EXISTING);
#else
    rename (szTmp, szFile);
#endif
}


STATUS ProcessEventData (DHANDLE hEventData)
{
    /* Function releases handle in any case */

    STATUS                  error         = NOERROR;
    EVENT_DATA              *pEvent       = NULL;
    DOMFWD_EVENT_INFO       EventInfo     = {0};
    DOMFWD_OTEL_FIELDS      OtelFields    = {0};
    rapidjson::StringBuffer PayloadBuffer;
    DWORD                   dwEventPid    = 0;
    BOOL                    fIsOwnEvent   = FALSE;

    if (NULLHANDLE == hEventData)
    {
        return ERR_MISC_INVALID_ARGS;
    }

    pEvent = (EVENT_DATA *) OSLockObject (hEventData);

    if (NULL == pEvent)
    {
        error = ERR_MEMORY;
        goto Done;
    }

    PopulateEventInfo (pEvent, &EventInfo);

    DOMFWD_TRACE ("event received: type=%u version=%u severity=%u error=%u ext=%d pidtid=%s exec=%s",
                  (unsigned) EventInfo.wType, (unsigned) EventInfo.wVersion, (unsigned) EventInfo.wSeverity, (unsigned) EventInfo.wErrorCode,
                  (int) EventInfo.fExtValid,
                  EventInfo.fExtValid ? EventInfo.ext.szPIDTID : "-",
                  EventInfo.fExtValid ? EventInfo.ext.szExecName : "-");

    /* Skip events domfwd generated itself (see g_dwOwnProcessId comment in
       AddInMain) - avoids reprocessing our own console/debug output as if
       it were a real Domino event. PID catches this running instance
       precisely; exec name is a backstop for a second instance that gets
       rejected by the single-instance guard and exits quickly - its own
       "already running" message would otherwise slip through under a
       different, no-longer-running PID. */

    if (EventInfo.fExtValid)
    {
        dwEventPid  = (DWORD) strtoul (EventInfo.ext.szPIDTID, NULL, 16);
        fIsOwnEvent = ((g_dwOwnProcessId && (dwEventPid == g_dwOwnProcessId)) ||
                       ContainsCaseInsensitive (EventInfo.ext.szExecName, DOMFWD_OWN_EXEC_NAME));
    }

    if (fIsOwnEvent)
    {
        g_ulEventsSkippedOwn++;
        DOMFWD_TRACE ("event skipped: generated by domfwd itself (own pid %lu)", (unsigned long) g_dwOwnProcessId);
        goto Done;
    }

    g_tLastEvent = time (NULL);

    if (g_wVerboseLevel > 0)
    {
        DumpEvent (pEvent, &EventInfo);
    }

    if (DOMFWD_EXPECTED_EVENT_VERSION == EventInfo.wVersion)
    {
        BuildOtelFields (&EventInfo, &OtelFields);
        BuildEventPayload (&OtelFields, &PayloadBuffer);

        WriteEventJson (&PayloadBuffer);

        /* Socket first: it never blocks. The direct push below waits for the server (up to its timeout) */
        SendEventToSocket (&PayloadBuffer);

        DOMFWD_TRACE ("event queued: %u bytes, connected=%d queued=%u sent=%lu dropped=%lu rejected=%lu wal written=%lu taken=%lu",
                      (unsigned) PayloadBuffer.GetSize (), (int) g_SocketSender.IsConnected (), (unsigned) g_SocketSender.GetQueuedLines (),
                      (unsigned long) g_SocketSender.GetSent (), (unsigned long) g_SocketSender.GetDropped (), (unsigned long) g_SocketSender.GetRejected (),
                      (unsigned long) g_SocketSender.GetSpilled (), (unsigned long) g_SocketSender.GetDrained ());

#ifdef DOMFWD_CURL
        PushEventToOtel (&OtelFields, &PayloadBuffer);
#endif
    }
    else
    {
        g_ulEventsDroppedVersion++;
        DOMFWD_TRACE ("event dropped: event version %u, expected %d", (unsigned) EventInfo.wVersion, DOMFWD_EXPECTED_EVENT_VERSION);
    }

Done:

    if (pEvent)
    {
        OSUnlockObject (hEventData);
        pEvent = NULL;
    }

    if (hEventData)
    {
        OSMemFree (hEventData);
        hEventData = NULLHANDLE;
    }

    return error;
}


/* Reads the event queue until it has been empty for QuietMs (at most MaxMs). Used at shutdown: the server posts the
   events to the queue asynchronously, so the last lines this add-in logged arrive here a moment after they were written.
   Freeing the queue before that gives "Error posting event to event queue: No such queue" in the server log.
   ProcessEventData skips our own events. Nothing may be logged with AddInLogMessageText after the last drain. */
void DrainEventQueue (unsigned QuietMs, unsigned MaxMs)
{
    const unsigned StepMs  = 50;
    unsigned       Quiet   = 0;
    unsigned       Waited  = 0;
    unsigned       Events  = 0;
    DHANDLE        hEvent  = NULLHANDLE;

    while ((Quiet < QuietMs) && (Waited < MaxMs) && (Events < 10000))
    {
        hEvent = NULLHANDLE;

        if (NOERROR == EventQueueGetExt (g_szEventQueue, &hEvent))
        {
            ProcessEventData (hEvent);
            Events++;
            Quiet = 0;
            continue;
        }

#if defined (_WIN32)
        Sleep (StepMs);
#else
        usleep (StepMs * 1000);
#endif
        Quiet  += StepMs;
        Waited += StepMs;
    }

    DOMFWD_TRACE ("event queue drained: %u events, waited %u ms", Events, Waited);
}


void ParseCommandLine (int argc, char far *argv[])
{
    int  i     = 0;
    char *pArg = NULL;
    char *p    = NULL;

    for (i = 0; i < argc; i++)
    {
        pArg = (char *) argv[i];

        if ((NULL == pArg) || ('-' != pArg[0]))
            continue;

        p = pArg + 1;

        if (('e' == *p) || ('E' == *p))
        {
            g_fDumpTypesAndExit = TRUE;
            continue;
        }

        while (('v' == *p) || ('V' == *p))
        {
            g_wVerboseLevel++;
            p++;
        }
    }
}


STATUS LNPUBLIC AddInMain (HMODULE hModule, int argc, char far *argv[])
{
    STATUS   error           = NOERROR;
    DHANDLE  hEventData      = NULLHANDLE;
    DHANDLE  hOldStatusLine  = NULLHANDLE;
    DHANDLE  hStatusLineDesc = NULLHANDLE;
    HMODULE  hMod            = NULLHANDLE;
    MQHANDLE hQueue          = NULLHANDLE;
    WORD     wDrainedCount   = 0;
    unsigned long ulTraceCycles = 0;
    time_t        tTraceNext    = 0;
    time_t        tPromNext     = 0;
    bool          fTraceFirstGet = false;

    (void) hModule;

    AddInQueryDefaults    (&hMod, &hOldStatusLine);
    AddInDeleteStatusLine (hOldStatusLine);

    hStatusLineDesc = AddInCreateStatusLine (g_szTaskLong);
    AddInSetDefaults (hMod, hStatusLineDesc);

    ParseCommandLine (argc, argv);

    AddInLogMessageText ("%s: Starting (%s) version %s, built %s %s", 0, g_szTask, g_szTaskLong, DOMFWD_VERSION, __DATE__, __TIME__);

    if (g_fDumpTypesAndExit)
    {
        DumpKnownEnums ();
        return NOERROR;
    }

    if (g_wVerboseLevel > 0)
        AddInLogMessageText ("%s: Verbose level: %u", 0, g_szTask, g_wVerboseLevel);

    error = MQCreate (DOMFWD_MQ_NAME, 0, 0);

    DOMFWD_TRACE ("MQCreate (%s): status 0x%04x", DOMFWD_MQ_NAME, (unsigned) error);

    if (error)
    {
        AddInLogMessageText ("%s: Servertask already started", 0, g_szTask);
        return NOERROR;
    }

    error = MQOpen (DOMFWD_MQ_NAME, 0, &hQueue);

    DOMFWD_TRACE ("MQOpen (%s): status 0x%04x", DOMFWD_MQ_NAME, (unsigned) error);

    if (error)
    {
        AddInLogMessageText ("%s: Cannot open message queue", error, g_szTask);
        return error;
    }

    AddInSetStatusText ("Starting");

    error = EventQueueAlloc (g_szEventQueue);

    DOMFWD_TRACE ("EventQueueAlloc (%s): status 0x%04x", g_szEventQueue, (unsigned) error);

    if (error)
    {
        AddInLogMessageText ("%s: EventQueueAlloc failed: %u (0x%04x)", error, g_szTask, error, error);
        MQClose (hQueue, 0);
        return error;
    }

    OpenEventLogFile ();
    GetLocalServerName ();
    LookupServerInfo ();
    LoadOtelConfig ();
    LoadSocketConfig ();
    LoadPromConfig ();
    g_tStartTime = time (NULL);

#ifndef DOMFWD_CURL
    if (g_szOtelPushURL[0])
        AddInLogMessageText ("%s: DOMFWD_OtelPushURL is set, but this build has no libcurl support (no DOMFWD_CURL). Use DOMFWD_Socket", 0, g_szTask);
#endif

    if (('\0' == g_szOtelPushURL[0]) && (false == g_SocketSender.IsConfigured ()))
        AddInLogMessageText ("%s: No forwarding target configured. Set DOMFWD_Socket (or DOMFWD_OtelPushURL) in notes.ini", 0, g_szTask);

    /* Our own AddInLogMessageText output goes to the console, which Domino
       turns into its own EVENTS4 event - so without this, domfwd reads its
       own console output back out of the queue and reprocesses it as if it
       were a real event. Compare against this to skip those. */
#if defined (_WIN32)
    g_dwOwnProcessId = (DWORD) GetCurrentProcessId ();
#else
    g_dwOwnProcessId = (DWORD) getpid ();
#endif

#ifdef DOMFWD_CURL
    curl_global_init (CURL_GLOBAL_DEFAULT);

    AddInLogMessageText ("%s: %s", 0, g_szTask, curl_version ());
#endif

    AddInSetStatusText ("Listening");

    AddInLogMessageText ("%s: Listening to Event Queue: %s", 0, g_szTask, g_szTask);

    DOMFWD_TRACE ("started: pid %lu, verbose level %u, socket configured=%d", (unsigned long) g_dwOwnProcessId, (unsigned) g_wVerboseLevel, (int) g_SocketSender.IsConfigured ());

    while (!AddInIdle ())
    {
        /* Heartbeat: shows the loop is running and how many events were received so far */
        ulTraceCycles++;

        if (time (NULL) >= tTraceNext)
        {
            DOMFWD_TRACE ("alive: idle cycles=%lu events received=%lu connected=%d queued=%u sent=%lu",
                          ulTraceCycles, g_ulEventsReceived, (int) g_SocketSender.IsConnected (), (unsigned) g_SocketSender.GetQueuedLines (), (unsigned long) g_SocketSender.GetSent ());
            tTraceNext = time (NULL) + 10;
        }

        /* Metrics file every 30 seconds. Also right at the start, so the file shows up at once */
        if (time (NULL) >= tPromNext)
        {
            UpdateHealth ();
            WritePromFile ();
            tPromNext = time (NULL) + DOMFWD_PROM_INTERVAL_SEC;
        }

        /* Per idle cycle. Without this reset no event is drained anymore once
           DOMFWD_MAX_EVENTS_PER_IDLE events have been processed in total. */
        wDrainedCount = 0;

        /* Connect / reconnect and send queued lines, also when there are no events */
        g_SocketSender.Pump ();

        while (wDrainedCount < DOMFWD_MAX_EVENTS_PER_IDLE)
        {
            hEventData = NULLHANDLE;

            error = EventQueueGetExt (g_szEventQueue, &hEventData);

            /* Once: what the queue answers when nothing was received yet. Empty is the expected answer. */
            if (false == fTraceFirstGet)
            {
                DOMFWD_TRACE ("first EventQueueGetExt (%s): status 0x%04x (empty is 0x%04x)", g_szEventQueue, (unsigned) error, (unsigned) ERR_EVTQUEUE_EMPTY);
                fTraceFirstGet = true;
            }

            if (ERR_EVTQUEUE_EMPTY == error)
                break;

            if (error)
            {
                AddInLogMessageText ("%s: EventQueueGetExt failed: %u (0x%04x)", error, g_szTask, error, error);
                break;
            }

            g_ulEventsReceived++;

            /* Function releases handle */
            error = ProcessEventData (hEventData);

            if (error)
                AddInLogMessageText ("%s: ProcessEventData failed: %u (0x%04x)", error, g_szTask, error, error);

            wDrainedCount++;

        } /* inner drain loop */

    } /* while */

    AddInLogMessageText ("%s: Shutting down", 0, g_szTask);

    /* First drain: events which arrived meanwhile are still forwarded, and the event of the line above is consumed */
    DrainEventQueue (DOMFWD_SHUTDOWN_QUIET_MS, DOMFWD_SHUTDOWN_MAX_MS);

    ShutdownSocket ();     /* logs the statistics: the last line this add-in logs */

    /* Second drain: takes the event of the statistics line. The socket is closed, so events are no longer forwarded */
    DrainEventQueue (DOMFWD_SHUTDOWN_QUIET_MS, DOMFWD_SHUTDOWN_MAX_MS);

    WritePromFile ();

#ifdef DOMFWD_CURL
    curl_global_cleanup ();
#endif

    if (NULL != g_pEventLogFile)
    {
        fclose (g_pEventLogFile);
        g_pEventLogFile = NULL;
    }

    /* No AddInLogMessageText from here on. The queue goes away last */
    EventQueueFree (g_szEventQueue);

    if (NULLHANDLE != hQueue)
    {
        MQClose (hQueue, 0);
        hQueue = NULLHANDLE;
    }

    return NOERROR;
}
