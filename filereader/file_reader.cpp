
/* FileReader - see file_reader.hpp */

#include "file_reader.hpp"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>


#define FILE_READER_DEFAULT_MAX_LINE   (1024 * 1024)
#define FILE_READER_CHUNK              (64 * 1024)          // how much is read at once
#define FILE_READER_HEAD_MAX           256                  // the number of bytes at the start of the file which are its fingerprint
#define FILE_READER_STATE_VERSION      1
#define FILE_READER_STATE_MAX          4096                 // a state file is a few lines. A larger file is not one


/* FNV-1a, 64 bit: a hash for the fingerprint. It has to tell files apart, not to resist an attack */
uint64_t FileReader::HashBytes (const char *pData, size_t Len)
{
    uint64_t Hash = 14695981039346656037ULL;

    for (size_t i = 0; i < Len; i++)
    {
        Hash ^= static_cast<unsigned char> (pData[i]);
        Hash *= 1099511628211ULL;
    }

    return Hash;
}


/* Reads up to Len bytes at Offset. Returns the number of bytes, or -1 at an error. Fewer bytes than asked means the end of the file */
ssize_t FileReader::ReadAt (int fd, char *pBuffer, size_t Len, uint64_t Offset)
{
    for (;;)
    {
        ssize_t Read = ::pread (fd, pBuffer, Len, static_cast<off_t> (Offset));

        if ( (Read < 0) && (EINTR == errno) )
            continue;

        return Read;
    }
}


/* Writes all of it, also when write() takes only a part. Returns false at an error */
bool FileReader::WriteAll (int fd, const char *pData, size_t Len)
{
    while (Len > 0)
    {
        ssize_t Written = ::write (fd, pData, Len);

        if (Written < 0)
        {
            if (EINTR == errno)
                continue;

            return false;
        }

        pData += Written;
        Len   -= static_cast<size_t> (Written);
    }

    return true;
}


FileReader::FileReader()
{
    m_MaxLineLength = FILE_READER_DEFAULT_MAX_LINE;
}


FileReader::~FileReader()
{
    Close();
}


void FileReader::SetMaxLineLength (size_t Bytes)
{
    m_MaxLineLength = Bytes ? Bytes : FILE_READER_DEFAULT_MAX_LINE;
}


void FileReader::SetStartAtEnd (bool bStartAtEnd)
{
    m_bStartAtEnd = bStartAtEnd;
}


void FileReader::SetIncompleteLineSeconds (unsigned Seconds)
{
    m_IncompleteSec = Seconds;
}


void FileReader::SetLogFunction (LogFunction Function)
{
    m_Log = Function;
}


void FileReader::Log (const char *pszFormat, ...)
{
    char    szMessage[1500] = {0};
    va_list Args;

    va_start (Args, pszFormat);
    vsnprintf (szMessage, sizeof (szMessage), pszFormat, Args);
    va_end (Args);

    if (m_Log)
        m_Log (szMessage);
    else
        printf ("%s\n", szMessage);
}


bool FileReader::Open (const std::string& Path, const std::string& StatePath)
{
    Close();

    if (Path.empty() || StatePath.empty())
        return false;

    m_Path           = Path;
    m_StatePath      = StatePath;
    m_bOpened        = true;
    m_bFirstOpen     = true;
    m_Generation     = 0;
    m_Committed      = 0;
    m_bSaveFailed    = false;
    m_LastOpenError  = 0;

    return true;
}


void FileReader::Close()
{
    CloseFile();

    m_bOpened = false;
}


void FileReader::CloseFile()
{
    if (m_Fd >= 0)
    {
        ::close (m_Fd);
        m_Fd = -1;
    }

    ClearBuffer();
    m_BufferStart      = 0;
    m_bSkipping        = false;
    m_bRotationSeen    = false;
    m_tIncompleteSince = 0;
    m_Head.clear();
}


/* Forgets what was read and not returned */
void FileReader::ClearBuffer()
{
    m_Buffer.clear();
    m_Pos      = 0;
    m_ScanFrom = 0;
}


/* Opens the file of the path. The first time the state file says where to continue, later it is a new file: from the beginning.
   Returns false if there is no file (yet) */
bool FileReader::OpenFile()
{
    struct stat FileStat;
    uint64_t    Offset = 0;
    char        szHead[FILE_READER_HEAD_MAX];
    ssize_t     HeadLen = 0;
    int         fd = ::open (m_Path.c_str(), O_RDONLY | O_CLOEXEC);

    if (fd < 0)
    {
        int Error = errno;

        /* A file which is not there yet is normal. Everything else is a message, once for every kind of error */
        if ( (ENOENT != Error) && (Error != m_LastOpenError) )
            Log ("[Error] FileReader: cannot open %s: %s", m_Path.c_str(), strerror (Error));

        m_LastOpenError = Error;
        return false;
    }

    if (0 != ::fstat (fd, &FileStat))
    {
        Log ("[Error] FileReader: cannot read the status of %s: %s", m_Path.c_str(), strerror (errno));
        ::close (fd);
        return false;
    }

    if (false == S_ISREG (FileStat.st_mode))
    {
        if (EISDIR != m_LastOpenError)
            Log ("[Error] FileReader: %s is not a regular file", m_Path.c_str());

        m_LastOpenError = EISDIR;
        ::close (fd);
        return false;
    }

    m_LastOpenError = 0;
    m_Fd            = fd;
    m_Device        = FileStat.st_dev;
    m_Inode         = FileStat.st_ino;
    m_Generation++;

    if (m_bFirstOpen)
    {
        m_bFirstOpen = false;
        ResolveStart (FileStat, Offset);
    }

    HeadLen = ReadAt (m_Fd, szHead, sizeof (szHead), 0);

    m_Head.assign (szHead, HeadLen > 0 ? static_cast<size_t> (HeadLen) : 0);

    ClearBuffer();
    m_BufferStart = Offset;
    m_Committed   = Offset;

    /* The identity of the file is saved at once: a restart before the first Commit() must find the same file and position */
    SaveState();

    return true;
}


/* Where to continue in the file which was opened for the first time: the position of the state file if it is the same file,
   otherwise the beginning, or the end without a state file if the settings say so */
void FileReader::ResolveStart (const struct stat& FileStat, uint64_t& retOffset)
{
    SavedState  Saved;
    StateResult Result = LoadState (Saved);
    uint64_t    Size   = static_cast<uint64_t> (FileStat.st_size);
    char        szHead[FILE_READER_HEAD_MAX];

    if ( (STATE_NONE == Result) || (STATE_INVALID == Result) )
    {
        retOffset = m_bStartAtEnd ? Size : 0;
        return;
    }

    retOffset = 0;

    if ( (Saved.Device != static_cast<uint64_t> (FileStat.st_dev)) || (Saved.Inode != static_cast<uint64_t> (FileStat.st_ino)) )
    {
        Log ("[Warning] FileReader: %s is another file than the one of the state file (it was replaced while the reader was not running). Reading it from the start", m_Path.c_str());
        return;
    }

    if (Saved.Offset > Size)
    {
        Log ("[Warning] FileReader: %s is shorter than the saved position %llu (it was truncated while the reader was not running). Reading it from the start",
             m_Path.c_str(), static_cast<unsigned long long> (Saved.Offset));
        return;
    }

    /* The start of the file must be the one which was seen: a file which was emptied and written again keeps its inode */
    ssize_t HeadLen = ReadAt (m_Fd, szHead, static_cast<size_t> (Saved.HeadLen), 0);

    if ( (HeadLen != static_cast<ssize_t> (Saved.HeadLen)) || (HashBytes (szHead, static_cast<size_t> (Saved.HeadLen)) != Saved.HeadHash) )
    {
        Log ("[Warning] FileReader: the start of %s has changed (it was replaced while the reader was not running). Reading it from the start", m_Path.c_str());
        return;
    }

    retOffset = Saved.Offset;
}


/* The next complete line of the buffer. A line which is too long is cut. Returns false if there is no line in the buffer */
bool FileReader::ExtractLine (Line& retLine)
{
    for (;;)
    {
        /* Pos is an index in m_Buffer, behind m_Pos. Nothing is erased here: the lines which were returned stay in front of m_Pos
           until the next FillBuffer(). Erasing at the front for every line would move the whole buffer every time */
        size_t Pos = m_Buffer.find ('\n', m_Pos + m_ScanFrom);

        /* The rest of a line which was too long: dropped up to the new line */
        if (m_bSkipping)
        {
            if (std::string::npos == Pos)
            {
                m_BufferStart += Pending();
                ClearBuffer();
                return false;
            }

            m_BufferStart += Pos + 1 - m_Pos;
            m_Pos       = Pos + 1;
            m_ScanFrom  = 0;
            m_bSkipping = false;
            continue;
        }

        if (std::string::npos == Pos)
        {
            /* More than the maximum and still no new line: the line is too long. It is cut, the rest is dropped */
            if (Pending() > m_MaxLineLength)
            {
                retLine.Text.assign (m_Buffer, m_Pos, m_MaxLineLength);
                retLine.bTruncated = true;
                retLine.Generation = m_Generation;

                m_BufferStart += m_MaxLineLength;
                m_Pos         += m_MaxLineLength;
                m_ScanFrom     = 0;
                m_bSkipping    = true;

                retLine.EndOffset = m_BufferStart;
                return true;
            }

            m_ScanFrom = Pending();
            return false;
        }

        /* The line is complete. Without the new line, and the carriage return before it */
        size_t Len = Pos - m_Pos;

        retLine.bTruncated = (Len > m_MaxLineLength);

        if (retLine.bTruncated)
            Len = m_MaxLineLength;
        else if ( (Len > 0) && ('\r' == m_Buffer[m_Pos + Len - 1]) )
            Len--;

        retLine.Text.assign (m_Buffer, m_Pos, Len);
        retLine.Generation = m_Generation;

        m_BufferStart += Pos + 1 - m_Pos;
        m_Pos          = Pos + 1;
        m_ScanFrom     = 0;

        retLine.EndOffset = m_BufferStart;
        return true;
    }
}


/* The end of a file which is finished, or a line which did not grow for a while: what is in the buffer is a line */
void FileReader::EmitRemainder (Line& retLine)
{
    size_t Len = Pending();

    retLine.bTruncated = false;
    retLine.Generation = m_Generation;

    if ( (Len > 0) && ('\r' == m_Buffer[m_Pos + Len - 1]) )
        Len--;

    retLine.Text.assign (m_Buffer, m_Pos, Len);

    m_BufferStart += Pending();
    ClearBuffer();
    m_tIncompleteSince = 0;

    retLine.EndOffset = m_BufferStart;
}


/* Reads more of the file into the buffer. Returns the number of bytes, 0 at the end of the file, -1 at an error */
ssize_t FileReader::FillBuffer()
{
    char    szChunk[FILE_READER_CHUNK];
    ssize_t Read = ReadAt (m_Fd, szChunk, sizeof (szChunk), m_BufferStart + Pending());

    if (Read < 0)
    {
        Log ("[Error] FileReader: cannot read %s: %s", m_Path.c_str(), strerror (errno));
        return -1;
    }

    /* The lines which were returned are dropped here, once for a whole chunk: what is moved is only the start of the last line */
    if (m_Pos > 0)
    {
        m_Buffer.erase (0, m_Pos);
        m_Pos = 0;
    }

    m_Buffer.append (szChunk, static_cast<size_t> (Read));
    return Read;
}


/* The start of the file is still the one which was seen. The start grows with the file, up to a few hundred bytes */
bool FileReader::RefreshHead()
{
    char    szHead[FILE_READER_HEAD_MAX];
    ssize_t Len = ReadAt (m_Fd, szHead, sizeof (szHead), 0);

    if (Len < 0)
        return true;

    if (static_cast<size_t> (Len) < m_Head.size())
        return false;

    if (0 != m_Head.compare (0, m_Head.size(), szHead, m_Head.size()))
        return false;

    m_Head.assign (szHead, static_cast<size_t> (Len));
    return true;
}


/* Before more is read: was the file emptied or written again? Returns false if it was, and the reader started again */
bool FileReader::CheckFile()
{
    struct stat FileStat;

    if (0 != ::fstat (m_Fd, &FileStat))
        return true;

    if (static_cast<uint64_t> (FileStat.st_size) < m_BufferStart + Pending())
    {
        ResetFile ("was truncated");
        return false;
    }

    if (false == RefreshHead())
    {
        ResetFile ("was written again (its start has changed)");
        return false;
    }

    return true;
}


/* The same file has other content: starts again at the beginning. A line which was not finished is dropped */
void FileReader::ResetFile (const char *pszReason)
{
    Log ("[Warning] FileReader: %s %s. Reading it again from the start", m_Path.c_str(), pszReason);

    m_Generation++;
    ClearBuffer();
    m_BufferStart      = 0;
    m_bSkipping        = false;
    m_bRotationSeen    = false;
    m_tIncompleteSince = 0;
    m_Head.clear();
    m_Committed        = 0;

    RefreshHead();
    SaveState();
}


/* Does the path lead to another file than the one which is open? A path without a file does not count: the file is deleted and
   there may be a new one soon */
bool FileReader::PathReplaced()
{
    struct stat PathStat;

    if (0 != ::stat (m_Path.c_str(), &PathStat))
        return false;

    return (PathStat.st_dev != m_Device) || (PathStat.st_ino != m_Inode);
}


bool FileReader::ReadLine (Line& retLine, time_t tNow)
{
    if (false == m_bOpened)
        return false;

    for (;;)
    {
        if ( (m_Fd < 0) && (false == OpenFile()) )
            return false;

        if (ExtractLine (retLine))
            return true;

        /* The file was emptied or written again, and the reader started again: look at the new content */
        if (false == CheckFile())
            continue;

        ssize_t Read = FillBuffer();

        if (Read < 0)
            return false;

        if (Read > 0)
        {
            m_bRotationSeen    = false;
            m_tIncompleteSince = 0;
            continue;
        }

        /* The end of the file. Is there a new file at the path? Then this one is finished, but the writer gets one more call to
           finish what it does */
        if (PathReplaced())
        {
            if (false == m_bRotationSeen)
            {
                m_bRotationSeen = true;
                return false;
            }

            if ( (Pending() > 0) && (false == m_bSkipping) )
            {
                EmitRemainder (retLine);
                return true;
            }

            CloseFile();
            continue;
        }

        m_bRotationSeen = false;

        /* A last line without a new line which did not grow for a while */
        if ( (m_IncompleteSec > 0) && (Pending() > 0) && (false == m_bSkipping) )
        {
            if (0 == m_tIncompleteSince)
            {
                m_tIncompleteSince = tNow;
            }
            else if ( (tNow - m_tIncompleteSince) >= static_cast<time_t> (m_IncompleteSec) )
            {
                EmitRemainder (retLine);
                return true;
            }
        }

        return false;
    }
}


bool FileReader::Commit (uint64_t Generation, uint64_t EndOffset)
{
    if (false == m_bOpened)
        return false;

    /* Lines of a file which was replaced: the state file belongs to the new file already */
    if (Generation != m_Generation)
        return true;

    if (EndOffset > m_BufferStart)
    {
        Log ("[Error] FileReader: the position %llu is behind the lines which were read (%llu). Not saved",
             static_cast<unsigned long long> (EndOffset), static_cast<unsigned long long> (m_BufferStart));
        return false;
    }

    if (EndOffset <= m_Committed)
        return true;

    uint64_t Before = m_Committed;

    m_Committed = EndOffset;

    if (false == SaveState())
    {
        m_Committed = Before;
        return false;
    }

    return true;
}


/* The state file: lines "name=number". The fingerprint is as long as the committed part of the file, up to a few hundred bytes */
FileReader::StateResult FileReader::LoadState (SavedState& retState)
{
    char    szText[FILE_READER_STATE_MAX + 1];
    int     fd = ::open (m_StatePath.c_str(), O_RDONLY | O_CLOEXEC);
    ssize_t Len = 0;
    bool    bVersion = false, bDevice = false, bInode = false, bOffset = false, bHeadLen = false, bHeadHash = false;
    char   *pLine = NULL;

    if (fd < 0)
    {
        if (ENOENT == errno)
            return STATE_NONE;

        Log ("[Warning] FileReader: cannot read the state file %s: %s. It is ignored", m_StatePath.c_str(), strerror (errno));
        return STATE_INVALID;
    }

    Len = ReadAt (fd, szText, FILE_READER_STATE_MAX + 1, 0);
    ::close (fd);

    if ( (Len < 0) || (Len > FILE_READER_STATE_MAX) )
    {
        Log ("[Warning] FileReader: the state file %s cannot be used (%s). It is ignored", m_StatePath.c_str(), (Len < 0) ? "cannot be read" : "too large");
        return STATE_INVALID;
    }

    szText[Len] = '\0';

    for (pLine = strtok (szText, "\n"); pLine; pLine = strtok (NULL, "\n"))
    {
        char *pValue = strchr (pLine, '=');
        char *pEnd   = NULL;

        if (NULL == pValue)
            continue;

        *pValue++ = '\0';

        errno = 0;

        unsigned long long Number = strtoull (pValue, &pEnd, 10);

        /* Only digits: strtoull also takes a sign and blanks */
        if ( (ERANGE == errno) || (pEnd == pValue) || ('\0' != *pEnd) || (*pValue < '0') || (*pValue > '9') )
        {
            Log ("[Warning] FileReader: the state file %s is damaged (%s). It is ignored", m_StatePath.c_str(), pLine);
            return STATE_INVALID;
        }

        if (0 == strcmp (pLine, "version"))       { if (Number != FILE_READER_STATE_VERSION) { Log ("[Warning] FileReader: the state file %s has the version %llu. It is ignored", m_StatePath.c_str(), Number); return STATE_INVALID; } bVersion = true; }
        else if (0 == strcmp (pLine, "device"))   { retState.Device   = Number; bDevice   = true; }
        else if (0 == strcmp (pLine, "inode"))    { retState.Inode    = Number; bInode    = true; }
        else if (0 == strcmp (pLine, "offset"))   { retState.Offset   = Number; bOffset   = true; }
        else if (0 == strcmp (pLine, "headlen"))  { retState.HeadLen  = Number; bHeadLen  = true; }
        else if (0 == strcmp (pLine, "headhash")) { retState.HeadHash = Number; bHeadHash = true; }
    }

    if ( (false == bVersion) || (false == bDevice) || (false == bInode) || (false == bOffset) || (false == bHeadLen) || (false == bHeadHash) ||
         (retState.HeadLen > FILE_READER_HEAD_MAX) || (retState.HeadLen > retState.Offset) )
    {
        Log ("[Warning] FileReader: the state file %s is incomplete. It is ignored", m_StatePath.c_str());
        return STATE_INVALID;
    }

    return STATE_OK;
}


/* Writes a file of its own and renames it, so that a reader never sees a half written state file. It is flushed to disk before
   the rename: a state file which is empty after a crash would lose the position */
bool FileReader::SaveState()
{
    std::string TempPath = m_StatePath + ".tmp";
    char        szText[400];
    size_t      HeadLen = (m_Committed < m_Head.size()) ? static_cast<size_t> (m_Committed) : m_Head.size();
    int         Len = snprintf (szText, sizeof (szText), "version=%d\ndevice=%llu\ninode=%llu\noffset=%llu\nheadlen=%llu\nheadhash=%llu\n",
                                FILE_READER_STATE_VERSION,
                                static_cast<unsigned long long> (m_Device), static_cast<unsigned long long> (m_Inode),
                                static_cast<unsigned long long> (m_Committed), static_cast<unsigned long long> (HeadLen),
                                static_cast<unsigned long long> (HashBytes (m_Head.data(), HeadLen)));
    int         Error = 0;
    bool        bSaved = false;
    int         fd = ::open (TempPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);

    if (fd >= 0)
    {
        bSaved = WriteAll (fd, szText, static_cast<size_t> (Len)) && (0 == ::fsync (fd));
        Error  = errno;

        if ( (0 != ::close (fd)) && bSaved )
        {
            bSaved = false;
            Error  = errno;
        }

        if (bSaved && (0 != ::rename (TempPath.c_str(), m_StatePath.c_str())))
        {
            bSaved = false;
            Error  = errno;
        }

        if (false == bSaved)
            ::unlink (TempPath.c_str());
    }
    else
    {
        Error = errno;
    }

    if (bSaved)
    {
        m_bSaveFailed = false;
    }
    else if (false == m_bSaveFailed)
    {
        m_bSaveFailed = true;
        Log ("[Error] FileReader: cannot save the state file %s: %s", m_StatePath.c_str(), strerror (Error));
    }

    return bSaved;
}
