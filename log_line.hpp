
/* The lines which otelfwd writes to its console (stderr): its own messages and the messages of the WAL. One format for all:

     2026-09-19T21:05:03Z  otelfwd: [Error] Curl operation failed: Failed to connect to localhost port 9428

   The time is UTC in ISO 8601, so it is the same on every host and can be compared with the times in OTLP and Grafana.
   The name of the process ("otelfwd: ") is set once at start with SetLogPrefix. otelfwd does not use it with -nostdin: the output
   is only its own then, and the runtime (journald, Docker) names the process. In pipe mode the lines share the output with the
   lines of the server which are mirrored to stdout, and the name tells them apart.
   The level ("[Error]", "[Warning]") is optional. A text after the message ("message: text") is optional too.

   A line is written with one call, so lines of two threads do not mix. Not for the mirrored lines, the output log file, the
   help and the configuration output: they stay as they are */

#pragma once

#include <errno.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include <string>


/* 1789852029 -> "2026-09-19T21:27:09Z" */
inline std::string FormatLogTime (time_t Time)
{
    struct tm Tm;
    char      szTime[32] = {0};

    if (NULL == gmtime_r (&Time, &Tm))
        return "0000-00-00T00:00:00Z";

    strftime (szTime, sizeof (szTime), "%Y-%m-%dT%H:%M:%SZ", &Tm);

    return szTime;
}


/* The text of one line with the newline. Time and prefix are parameters, so it can be tested. pszPrefix, pszLevel and pszText
   are optional (NULL or empty for none) */
inline std::string FormatLogLine (time_t Time, const char *pszPrefix, const char *pszLevel, const char *pszMessage, const char *pszText)
{
    std::string Line = FormatLogTime (Time);

    /* Two blanks: the time stands out from the rest of the line */
    Line += "  ";

    if (pszPrefix && *pszPrefix)
    {
        Line += pszPrefix;
        Line += ": ";
    }

    if (pszLevel && *pszLevel)
    {
        Line += '[';
        Line += pszLevel;
        Line += "] ";
    }

    Line += pszMessage ? pszMessage : "";

    if (pszText && *pszText)
    {
        Line += ": ";
        Line += pszText;
    }

    Line += '\n';

    return Line;
}


/* The name of the process at the start of a line. Set it once at start, before other threads exist. No name: NULL */
inline std::string& GetLogPrefixStorage()
{
    static std::string Prefix;

    return Prefix;
}


inline void SetLogPrefix (const char *pszPrefix)
{
    GetLogPrefixStorage() = pszPrefix ? pszPrefix : "";
}


/* Writes one line to stderr, now */
inline void WriteLogLine (const char *pszLevel, const char *pszMessage, const char *pszText = NULL)
{
    std::string Line = FormatLogLine (time (NULL), GetLogPrefixStorage().c_str(), pszLevel, pszMessage, pszText);

    fputs (Line.c_str(), stderr);
}


/* An error with the text of errno, in the place of perror. errno is read first: nothing else may change it before */
inline void WriteLogErrno (const char *pszMessage)
{
    int Error = errno;

    WriteLogLine ("Error", pszMessage, strerror (Error));
}
