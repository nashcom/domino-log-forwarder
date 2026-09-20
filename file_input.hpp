
/* file_input.hpp - the pieces of the file input of otelfwd (OTELFWD_FILE_INPUT) which need no thread of their own and no network,
   so that they can be tested on their own (otelfwd_unit_test.cpp). The reading of the file is the FileReader (filereader/).

   * FileCommitSlot   The push thread learns that a batch was delivered. The FileReader can only be used by one thread, the file
                      thread, which also commits. So the push thread does not commit: it leaves "committed up to here" in this
                      slot, and the file thread takes it and commits. Only the highest position is kept: one Commit() covers
                      all lines before it.
   * FileSeverity     The names of OTELFWD_FILE_SEVERITY: the severity number and text of the lines of a file (info by default).
   * FileStateName    The name of the state file of a file. By default it is next to the file: <path of the file>.otelfwd-state. With
                      a directory (OTELFWD_FILE_STATE_DIR) it is <directory>/<name of the file>.<hash of the path>.otelfwd-state:
                      the hash tells two files with the same name in different directories apart, and it lets one directory hold
                      the state files of many files. The name must not change between versions: a state file which is not found means
                      that the file is read again from the start (or from the end, with OTELFWD_FILE_START=end, which loses lines).
                      The unit test has the names of two paths written down. */

#pragma once

#include <stdint.h>
#include <stdio.h>
#include <strings.h>

#include <mutex>
#include <string>


class FileSeverity
{

public:

    /* The value of OTELFWD_FILE_SEVERITY: the severity of every line of the file, as OpenTelemetry defines it. A text file has no
       level, so it is one for the whole file. trace (1), debug (5), info (9), warn or warning (13), error (17), fatal (21), in any
       case. off, none or unspecified: no severity (number 0, no text). Returns false for any other text, and changes nothing then */
    static bool Parse (const char *pszName, int& retNumber, std::string& retText)
    {
        struct Level
        {
            const char *pszName;
            int         Number;
            const char *pszText;
        };

        static const Level Levels[] =
        {
            { "trace",       1,  "TRACE" },
            { "debug",       5,  "DEBUG" },
            { "info",        9,  "INFO"  },
            { "warn",        13, "WARN"  },
            { "warning",     13, "WARN"  },
            { "error",       17, "ERROR" },
            { "fatal",       21, "FATAL" },
            { "off",         0,  ""      },
            { "none",        0,  ""      },
            { "unspecified", 0,  ""      }
        };

        if (NULL == pszName)
            return false;

        for (const Level& Entry : Levels)
        {
            if (0 == strcasecmp (pszName, Entry.pszName))
            {
                retNumber = Entry.Number;
                retText   = Entry.pszText;
                return true;
            }
        }

        return false;
    }
};


class FileCommitSlot
{

public:

    /* Called by the push thread: the lines of this Generation up to EndOffset were delivered for good (accepted by the receiver,
       or kept in the WAL, or refused by the receiver as bad data). Only a position which is higher than every one before it counts:
       a lower one, or one which was taken already, is ignored. A newer Generation replaces an older one (the file was rotated: the
       position of the old file is of no use). An older Generation is ignored */
    void Add (uint64_t Generation, uint64_t EndOffset)
    {
        std::lock_guard<std::mutex> Lock (m_Mutex);

        if ( (Generation > m_Generation) || ( (Generation == m_Generation) && (EndOffset > m_EndOffset) ) )
        {
            m_Generation = Generation;
            m_EndOffset  = EndOffset;
            m_bPending   = true;
        }
    }

    /* Called by the file thread: what is to be committed. Returns false if there is nothing new since the last call. What it returns
       never goes back: not to a lower position, not to an older generation */
    bool Take (uint64_t& retGeneration, uint64_t& retEndOffset)
    {
        std::lock_guard<std::mutex> Lock (m_Mutex);

        if (false == m_bPending)
            return false;

        retGeneration = m_Generation;
        retEndOffset  = m_EndOffset;
        m_bPending    = false;

        return true;
    }

private:

    std::mutex m_Mutex;
    bool       m_bPending   = false;
    uint64_t   m_Generation = 0;
    uint64_t   m_EndOffset  = 0;
};


class FileStateName
{

public:

    /* The state file of FilePath. Without a directory it is next to the file: the path of the file and ".otelfwd-state". In a directory
       it is named after the file and the hash of its path. FilePath is used as it is, so pass the same text every time (an absolute
       path) */
    static std::string Make (const std::string& Directory, const std::string& FilePath)
    {
        if (Directory.empty())
            return FilePath + ".otelfwd-state";

        char        szHash[16] = {0};
        std::string Name       = BaseName (FilePath);
        std::string Result     = Directory;

        snprintf (szHash, sizeof (szHash), "%08x", static_cast<unsigned int> (Hash (FilePath) & 0xFFFFFFFFu));

        if ( (false == Result.empty()) && ('/' != Result.back()) )
            Result += '/';

        Result += Name;
        Result += '.';
        Result += szHash;
        Result += ".otelfwd-state";

        return Result;
    }

private:

    static constexpr size_t MAX_NAME_LENGTH = 100;

    /* The last part of the path, with what is not a letter, a digit, a dot, a dash or an underscore replaced by an underscore.
       Not longer than MAX_NAME_LENGTH. "file" if there is nothing left */
    static std::string BaseName (const std::string& FilePath)
    {
        size_t      Start = FilePath.rfind ('/');
        std::string Name  = (std::string::npos == Start) ? FilePath : FilePath.substr (Start + 1);

        if (Name.size() > MAX_NAME_LENGTH)
            Name.resize (MAX_NAME_LENGTH);

        for (char& c : Name)
        {
            bool bValid = ( (c >= '0') && (c <= '9') ) || ( (c >= 'a') && (c <= 'z') ) || ( (c >= 'A') && (c <= 'Z') ) || ('.' == c) || ('-' == c) || ('_' == c);

            if (false == bValid)
                c = '_';
        }

        return Name.empty() ? "file" : Name;
    }

    /* FNV-1a, 64 bit. It has to tell paths apart, not to resist an attack */
    static uint64_t Hash (const std::string& Text)
    {
        uint64_t Value = 14695981039346656037ULL;

        for (char c : Text)
        {
            Value ^= static_cast<unsigned char> (c);
            Value *= 1099511628211ULL;
        }

        return Value;
    }
};
