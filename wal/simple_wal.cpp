
#include <stdexcept>
#include <new>
#include <cstring>
#include <mutex>

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <unistd.h>
#include <sys/file.h>
#include <sys/stat.h>

#include "simple_wal.hpp"


/* The files of the WAL contain log lines. Only the owner may access them */
#define WAL_FILE_MODE 0600


std::atomic<int> SimpleWAL::s_DefaultLogTarget {SimpleWAL::LOG_STDOUT};


off_t SimpleWAL::GetFileSize (int fd)
{
    struct stat StatBuf;

    if (fd < 0)
        return 0;

    if (::fstat (fd, &StatBuf) < 0)
        return -1;

    return StatBuf.st_size;
}


/* Writes everything or fails */
static bool WriteAllToFd (int fd, const void* pBuf, size_t Len)
{
    const uint8_t* pPtr = static_cast<const uint8_t*> (pBuf);

    if (fd < 0)
        return false;

    while (Len > 0)
    {
        ssize_t Written = ::write (fd, pPtr, Len);

        if (Written <= 0)
            return false;

        pPtr += Written;
        Len -= static_cast<size_t> (Written);
    }

    return true;
}


/* Gives the messages to the function of the application, or writes them to the target. Never called with the mutex held: the function
   can do anything, also call the WAL. An exception of the function does not leave the WAL */
static void EmitMessages (const SimpleWAL::LogFunction& Function, SimpleWAL::LogTarget Target, const std::vector<std::string>& Messages)
{
    for (const std::string& Message : Messages)
    {
        try
        {
            if (Function)
            {
                Function (Message.c_str());
            }
            else if (SimpleWAL::LOG_STDOUT == Target)
            {
                printf ("%s\n", Message.c_str());
                fflush (stdout);
            }
            else if (SimpleWAL::LOG_STDERR == Target)
            {
                fprintf (stderr, "%s\n", Message.c_str());
            }
        }
        catch (...)
        {
        }
    }
}


/* Takes the mutex, and gives the messages out after it is released. Every public function of the WAL starts with one of these,
   so a message is never given out with the mutex held, and no function has to think about it */
class SimpleWAL::Locked
{
public:

    explicit Locked (SimpleWAL& Wal) : m_Wal (Wal), m_Lock (Wal.m_mutex) {}

    ~Locked()
    {
        Unlock();
    }

    /* Releases the mutex, then gives out the messages. Can be called early. Only the first call does something */
    void Unlock()
    {
        if (false == m_Lock.owns_lock())
            return;

        std::vector<std::string> Messages;
        Messages.swap (m_Wal.m_LogQueue);

        LogFunction Function = m_Wal.m_LogFunction;
        LogTarget   Target   = m_Wal.GetEffectiveTarget();

        m_Lock.unlock();

        EmitMessages (Function, Target, Messages);
    }

private:

    SimpleWAL&                   m_Wal;
    std::unique_lock<std::mutex> m_Lock;
};


/* --- Messages. The Queue functions are only called with the mutex held --- */

SimpleWAL::LogTarget SimpleWAL::GetEffectiveTarget()
{
    if (LOG_DEFAULT != m_LogTarget)
        return m_LogTarget;

    return static_cast<LogTarget> (s_DefaultLogTarget.load());
}


void SimpleWAL::QueueLine (const char *pszLevel, const std::string& Text)
{
    if (pszLevel)
        m_LogQueue.push_back (std::string ("[") + pszLevel + "] " + Text);
    else
        m_LogQueue.push_back (Text);
}


void SimpleWAL::QueueInfo (const std::string& Text)
{
    if (m_LogLevel)
        QueueLine (NULL, Text);
}


void SimpleWAL::QueueWarning (const std::string& Text)
{
    QueueLine ("Warning", Text);
}


void SimpleWAL::QueueError (const std::string& Text)
{
    QueueLine ("Error", Text);
}


void SimpleWAL::QueueErrno (const char *pszText)
{
    int Error = errno;

    QueueLine ("Error", std::string (pszText) + ": " + strerror (Error));
}


/* --- Settings --- */

SimpleWAL::SimpleWAL ()
{
    m_fd = -1;
    m_bSync = false;
    m_LogLevel = 0;
    m_MaxSize = 0;
    m_bFullLogged = false;
    m_PendingReplay = false;
    m_bCommitChecked = false;
    m_bReplaying = false;
    m_bPeeked = false;
    m_PeekOffset = 0;
    m_PeekLen = 0;
    m_PeekEpoch = 0;
    m_PositionEpoch = 0;
    m_LogTarget = LOG_DEFAULT;
}


void SimpleWAL::SetPaths (const std::string& Path)
{
    m_WalPath = Path;
    m_CommitPath = m_WalPath + ".commit";
    m_CorruptPath = m_WalPath + ".corrupt";
}


bool SimpleWAL::GetSync()
{
    Locked Lock (*this);

    return m_bSync;
}


void SimpleWAL::SetSync (bool bEnable)
{
    Locked Lock (*this);

    m_bSync = bEnable;
}


void SimpleWAL::SetLogLevel (size_t LogLevel)
{
    Locked Lock (*this);

    m_LogLevel = LogLevel;
}


void SimpleWAL::SetLogFunction (const LogFunction& Function)
{
    Locked Lock (*this);

    m_LogFunction = Function;
}


void SimpleWAL::SetLogTarget (LogTarget Target)
{
    Locked Lock (*this);

    m_LogTarget = Target;
}


void SimpleWAL::SetDefaultLogTarget (LogTarget Target)
{
    s_DefaultLogTarget = (LOG_DEFAULT == Target) ? static_cast<int> (LOG_STDOUT) : static_cast<int> (Target);
}


void SimpleWAL::LogMessage (const char *pszMessage)
{
    if (NULL == pszMessage)
        return;

    Locked Lock (*this);

    QueueInfo (pszMessage);
}


void SimpleWAL::SetMaxSize (uint64_t Bytes)
{
    Locked Lock (*this);

    m_MaxSize = Bytes;
    m_bFullLogged = false;
}


uint64_t SimpleWAL::GetSize()
{
    Locked Lock (*this);

    off_t Size = GetFileSize (m_fd);

    return (Size > 0) ? static_cast<uint64_t> (Size) : 0;
}


/* --- Opening and closing --- */

bool SimpleWAL::Init (const std::string& Path)
{
    Locked Lock (*this);

    /* Opened before: the old file is closed first. This also ends its lock */
    if (m_fd >= 0)
    {
        ::close (m_fd);
        m_fd = -1;
    }

    SetPaths (Path);

    m_bCommitChecked = false;
    m_PendingReplay  = false;
    m_bPeeked        = false;
    m_bFullLogged    = false;

    m_fd = ::open (m_WalPath.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, WAL_FILE_MODE);

    if (m_fd < 0)
    {
        QueueErrno ("open wal failed");
        return false;
    }

    /* Two objects which write to the same file would mix their records. The lock is for the whole file, it is gone with the file
       descriptor. A file system without locks (some network file systems) is no reason to stop: then it is only reported */
    if (::flock (m_fd, LOCK_EX | LOCK_NB) < 0)
    {
        if (EWOULDBLOCK == errno)
        {
            QueueError ("WAL: " + m_WalPath + " is in use by another WAL object or process");
            ::close (m_fd);
            m_fd = -1;
            return false;
        }

        QueueErrno ("Cannot lock the WAL file. It is used without a lock");
    }

    /* A WAL of an earlier version was created with more permissions. This does not fail if the file belongs to somebody else */
    if (::fchmod (m_fd, WAL_FILE_MODE) < 0)
    {
        QueueInfo ("Cannot restrict the permissions of the WAL file");
    }

    if (GetFileSize (m_fd) > 0)
        m_PendingReplay = true;
    else
        m_PendingReplay = false;

    return true;
}


SimpleWAL::~SimpleWAL()
{
    bool bRemoveFiles = false;

    /* No other thread uses the object any more: the application waits for them. So there is no lock */
    if (m_fd >= 0)
    {
        if (GetFileSize (m_fd) <= 0)
        {
            bRemoveFiles = true;
        }

        m_PendingReplay = false;

        ::close (m_fd);
        m_fd = -1;
    }

    if (bRemoveFiles)
    {
        ::unlink (m_WalPath.c_str());
        ::unlink (m_CommitPath.c_str());
    }
}


/* --- Writing --- */


bool SimpleWAL::IsReplayPending()
{
    Locked Lock (*this);

    return m_PendingReplay;
}


bool SimpleWAL::Append (const void* pData, uint32_t Len)
{
    if (0 == Len)
        return true;

    Locked Lock (*this);

    if (m_fd < 0)
    {
        return false;
    }

    /* The WAL is opened for appending: the end of the file is where this record starts */
    off_t Start = ::lseek (m_fd, 0, SEEK_END);

    if (Start < 0)
    {
        QueueErrno ("Cannot get the end of the WAL");
        return false;
    }

    /* The size limit. The record is refused as a whole, and nothing of it is written. One message, until something was stored again */
    if ( (m_MaxSize > 0) && (static_cast<uint64_t> (Start) + sizeof (Len) + Len > m_MaxSize) )
    {
        if (false == m_bFullLogged)
        {
            m_bFullLogged = true;

            QueueWarning ("WAL: " + m_WalPath + " is full: a record of " + std::to_string (static_cast<unsigned long> (Len)) + " bytes does not fit (size " +
                          std::to_string (static_cast<long long> (Start)) + " bytes, limit " + std::to_string (static_cast<unsigned long long> (m_MaxSize)) +
                          " bytes). Records are not stored until there is room again");
        }

        return false;
    }

    if ( (false == WriteAll (&Len, sizeof (Len))) || (false == WriteAll (pData, Len)) )
    {
        /* Do not leave a piece of the record behind (the disk is full, for example). Every record after it would be unreadable */
        if (::ftruncate (m_fd, Start) < 0)
            QueueErrno ("Cannot remove the partial WAL record");

        return false;
    }

    if (m_bSync)
    {
        ::fsync (m_fd);
    }

    m_bFullLogged   = false;
    m_PendingReplay = true;

    return true;
}


bool SimpleWAL::Clear()
{
    Locked Lock (*this);

    return ClearInternal();
}


/* --- Taking records out --- */

/* The start of a replay takes the place of the replaying thread, the end gives it back, also if the function of the application
   throws. Only one thread at a time */
namespace
{

class ReplayPlace
{
public:

    ReplayPlace (bool& bReplaying) : m_bReplaying (bReplaying), m_bHeld (false) {}

    /* With the mutex held. Returns false if another thread replays */
    bool Take()
    {
        if (m_bReplaying)
            return false;

        m_bReplaying = true;
        m_bHeld      = true;

        return true;
    }

    /* With the mutex held */
    void Give()
    {
        if (m_bHeld)
        {
            m_bReplaying = false;
            m_bHeld      = false;
        }
    }

private:

    bool& m_bReplaying;
    bool  m_bHeld;
};

}   // namespace


bool SimpleWAL::Replay (const Consumer& Consume)
{
    uint64_t NewOffset = 0;
    bool bDidReplay    = false;
    bool bCleared      = false;     // the WAL was emptied in the loop. bClearOk is the result of that
    bool bClearOk      = true;
    bool bChanged      = false;     // Clear() was called by another thread: the position of this replay does not fit any more
    int  fd            = -1;
    ReplayPlace Place (m_bReplaying);

    /* --- Start: one thread which takes records out. The commit position is checked --- */
    {
        Locked Lock (*this);

        if (false == Place.Take())
            return false;

        fd = ::open (m_WalPath.c_str(), O_RDONLY | O_CLOEXEC);

        if (fd < 0)
        {
            Place.Give();
            return false;
        }

        off_t FileSize = GetFileSize (fd);

        if (FileSize < 0)
        {
            QueueErrno ("Cannot get the size of the WAL");
            ::close (fd);
            Place.Give();
            return false;
        }

        NewOffset = GetReplayPosition (fd, FileSize);
    }

    try
    {
        while (true)
        {
            uint32_t Len   = 0;
            bool     bStop = false;
            std::vector<uint8_t> Buffer;

            /* --- One record. The file is only read and emptied with the mutex held, and Append() needs it too: what is appended
                   is either in the file when the size is read, or it comes after the WAL was emptied. Nothing is lost --- */
            {
                Locked Lock (*this);

                switch (ReadRecord (fd, NewOffset, Buffer, Len, bClearOk))
                {
                    case READ_RECORD:
                        break;

                    case READ_EMPTY:        // everything is replayed
                    case READ_DAMAGED:      // the rest was moved to the .corrupt file
                        bCleared = true;
                        bStop    = true;
                        break;

                    case READ_CHANGED:      // another thread emptied the WAL. Nothing of this replay is saved
                        bChanged = true;
                        bStop    = true;
                        break;

                    default:
                        bStop = true;
                        break;
                }
            }

            if (bStop)
                break;

            /* The function of the application runs without the mutex: a slow receiver does not stop Append(), and the function can use the WAL */
            if (false == Consume (Buffer))
                break;

            NewOffset += sizeof (Len) + Len;
            bDidReplay = true;
        }
    }
    catch (...)
    {
        ::close (fd);

        Locked Lock (*this);
        Place.Give();

        throw;
    }

    ::close (fd);

    /* --- End: the position of what was accepted is saved. The WAL was emptied above, if everything was replayed --- */
    Locked Lock (*this);

    Place.Give();

    if (bCleared)
        return bClearOk;

    if (bChanged)
        return false;

    if (bDidReplay)
    {
        /* The records were delivered, but the position is not saved: the next replay delivers them again. Do not report progress
           which is not saved. The caller then waits before it tries again, instead of sending the same records again at once */
        if (false == StoreCommit (NewOffset))
            return false;
    }

    return bDidReplay;
}


bool SimpleWAL::Peek (std::vector<uint8_t>& Record)
{
    Locked Lock (*this);

    Record.clear();
    m_bPeeked = false;

    /* Nothing pending is the usual case of a consumer which asks again and again: no file is opened for it */
    if ( (m_fd < 0) || m_bReplaying || (false == m_PendingReplay) )
        return false;

    int fd = ::open (m_WalPath.c_str(), O_RDONLY | O_CLOEXEC);

    if (fd < 0)
    {
        QueueErrno ("Cannot open the WAL for reading");
        return false;
    }

    off_t FileSize = GetFileSize (fd);

    if (FileSize < 0)
    {
        QueueErrno ("Cannot get the size of the WAL");
        ::close (fd);
        return false;
    }

    uint64_t Offset   = GetReplayPosition (fd, FileSize);
    uint32_t Len      = 0;
    bool     bClearOk = true;

    ReadResult Result = ReadRecord (fd, Offset, Record, Len, bClearOk);

    ::close (fd);

    if (READ_RECORD != Result)
    {
        Record.clear();
        return false;
    }

    m_bPeeked    = true;
    m_PeekOffset = Offset;
    m_PeekLen    = Len;
    m_PeekEpoch  = m_PositionEpoch;

    return true;
}


bool SimpleWAL::Ack()
{
    Locked Lock (*this);

    if (false == m_bPeeked)
        return false;

    m_bPeeked = false;

    /* The position on disk was moved or the WAL was emptied since Peek(): the record which was read is not the record at the
       position any more. Nothing is acknowledged, the next Peek() reads what is there */
    if ( (m_fd < 0) || m_bReplaying || (m_PeekEpoch != m_PositionEpoch) )
        return false;

    off_t    FileSize = GetFileSize (m_fd);
    uint64_t Next     = m_PeekOffset + sizeof (uint32_t) + m_PeekLen;

    if ( (FileSize < 0) || (Next > static_cast<uint64_t> (FileSize)) )
        return false;

    /* The last record: the WAL is emptied. Append() needs the mutex, so a record which was appended is in the file at this point */
    if (Next == static_cast<uint64_t> (FileSize))
        return ClearInternal();

    return StoreCommit (Next);
}


/* --- Private functions. The mutex is held --- */


/* Where a replay starts */
uint64_t SimpleWAL::GetReplayPosition (int fd, off_t FileSize)
{
    uint64_t Offset = LoadCommit();

    /* A commit position behind the end of the WAL is stale: it belongs to an earlier WAL which was truncated, and the commit
       file was not removed (for example a crash in between). Replay the WAL from the start */
    if (Offset > static_cast<uint64_t> (FileSize))
    {
        QueueWarning ("WAL: commit offset " + std::to_string (static_cast<unsigned long long> (Offset)) + " is behind the end of the WAL (" +
                      std::to_string (static_cast<long long> (FileSize)) + " bytes). Replaying from the start");

        Offset = 0;
    }

    /* The commit position has to be the start of a record. A commit file with the right size but a wrong value (damaged, or left
       by an older WAL) would make the replay read the middle of a record as a length and skip or misread valid records. This is
       checked once, when the WAL was opened: after that the position is one which this program wrote. If it does not fit, replay
       the WAL from the start: a record can be delivered twice, none is lost */
    if (false == m_bCommitChecked)
    {
        if ( (Offset > 0) && (Offset < static_cast<uint64_t> (FileSize)) && (false == IsRecordBoundary (fd, Offset, static_cast<uint64_t> (FileSize))) )
        {
            QueueWarning ("WAL: commit offset " + std::to_string (static_cast<unsigned long long> (Offset)) + " is not the start of a record. Replaying from the start");

            Offset = 0;
        }

        m_bCommitChecked = true;
    }

    return Offset;
}


/* Reads the record which starts at Offset */
SimpleWAL::ReadResult SimpleWAL::ReadRecord (int fd, uint64_t Offset, std::vector<uint8_t>& Record, uint32_t& retLen, bool& retClearOk)
{
    uint32_t Len = 0;

    retClearOk = true;

    off_t FileSize = GetFileSize (fd);

    if (FileSize < 0)
    {
        QueueErrno ("Cannot get the size of the WAL");
        return READ_STOP;
    }

    /* The WAL is shorter than the position: another thread emptied it */
    if (Offset > static_cast<uint64_t> (FileSize))
        return READ_CHANGED;

    uint64_t Remaining = static_cast<uint64_t> (FileSize) - Offset;

    /* Everything is read */
    if (0 == Remaining)
    {
        retClearOk = ClearInternal();
        return READ_EMPTY;
    }

    ssize_t ReadBytes = ::pread (fd, &Len, sizeof (Len), static_cast<off_t> (Offset));

    if (ReadBytes < 0)
    {
        QueueErrno ("Cannot read the WAL");
        return READ_STOP;
    }

    /* The length has to fit into the rest of the file. Otherwise the record was cut off by a crash or the length is damaged, and
       everything behind it is not readable either. That part is moved to the .corrupt file, and the WAL continues with what is
       before it. This is also the limit for the memory: a record is never larger than the WAL file */
    if ( (static_cast<size_t> (ReadBytes) != sizeof (Len)) || (0 == Len) || (Len > Remaining - sizeof (Len)) )
    {
        if (QuarantineTail (fd, Offset, static_cast<uint64_t> (FileSize)))
        {
            retClearOk = ClearInternal();
            return READ_DAMAGED;
        }

        return READ_STOP;
    }

    try
    {
        Record.resize (Len);
    }
    catch (const std::bad_alloc&)
    {
        QueueError ("WAL: not enough memory for a record of " + std::to_string (static_cast<unsigned long> (Len)) + " bytes");
        return READ_STOP;
    }

    if (::pread (fd, Record.data(), Len, static_cast<off_t> (Offset + sizeof (Len))) != static_cast<ssize_t> (Len))
        return READ_STOP;

    retLen = Len;

    return READ_RECORD;
}


bool SimpleWAL::ClearInternal()
{
    if (m_fd < 0)
        return true;

    /* Remove the commit file first. A crash after this replays the whole WAL again, which can deliver a record twice.
       The other order could leave a commit position behind which does not fit to the next WAL */
    if ( (::unlink (m_CommitPath.c_str()) < 0) && (ENOENT != errno) )
    {
        /* The WAL is left as it is. Truncating it now would leave a commit position behind which does not fit to the next records */
        QueueErrno ("Cannot remove the commit file");
        return false;
    }

    if (::ftruncate (m_fd, 0) < 0)
    {
        QueueErrno ("file truncate failed");
        return false;
    }

    if (::lseek (m_fd, 0, SEEK_SET) < 0)
    {
        QueueErrno ("file seek failed");
        return false;
    }

    m_PendingReplay = false;
    m_bFullLogged   = false;
    m_bPeeked       = false;
    m_PositionEpoch++;

    QueueInfo ("WAL reset");

    return true;
}


bool SimpleWAL::IsRecordBoundary (int fd, uint64_t Offset, uint64_t FileSize)
{
    uint64_t Pos = 0;

    /* Follow the records from the start. Only their lengths are read */
    while (Pos < Offset)
    {
        uint32_t Len = 0;

        if (::pread (fd, &Len, sizeof (Len), static_cast<off_t> (Pos)) != static_cast<ssize_t> (sizeof (Len)))
            return false;

        if ( (0 == Len) || (Len > FileSize - Pos - sizeof (Len)) )
            return false;

        Pos += sizeof (Len) + Len;
    }

    return (Pos == Offset);
}


bool SimpleWAL::QuarantineTail (int fdWal, uint64_t From, uint64_t To)
{
    uint8_t  Buffer[65536];
    uint64_t Pos = From;
    int fdCorrupt = ::open (m_CorruptPath.c_str(), O_CREAT | O_APPEND | O_WRONLY | O_CLOEXEC, 0600);

    if (fdCorrupt < 0)
    {
        QueueErrno ("Cannot open the file for unreadable WAL data");
        return false;
    }

    while (Pos < To)
    {
        size_t Chunk = sizeof (Buffer);

        if (To - Pos < Chunk)
            Chunk = static_cast<size_t> (To - Pos);

        ssize_t ReadBytes = ::pread (fdWal, Buffer, Chunk, static_cast<off_t> (Pos));

        if ( (ReadBytes <= 0) || (false == WriteAllToFd (fdCorrupt, Buffer, static_cast<size_t> (ReadBytes))) )
        {
            QueueErrno ("Cannot copy unreadable WAL data");
            ::close (fdCorrupt);
            return false;
        }

        Pos += static_cast<uint64_t> (ReadBytes);
    }

    ::close (fdCorrupt);

    QueueError ("WAL: unreadable data at offset " + std::to_string (static_cast<unsigned long long> (From)) + " (" +
                std::to_string (static_cast<unsigned long long> (To - From)) + " bytes) moved to " + m_CorruptPath);

    return true;
}


bool SimpleWAL::WriteAll (const void* pBuf, size_t Len)
{
    if (m_fd < 0)
        return false;

    if (false == WriteAllToFd (m_fd, pBuf, Len))
    {
        QueueErrno ("Cannot write to WAL");
        return false;
    }

    return true;
}


uint64_t SimpleWAL::LoadCommit()
{
    int fdCommit = ::open (m_CommitPath.c_str(), O_RDONLY | O_CLOEXEC);

    if (fdCommit < 0)
        return 0;

    struct stat StatBuf;
    uint64_t Offset = 0;

    /* The file is exactly one offset. Anything else is damaged (a write which was cut off, for example) and is not used:
       replaying the WAL from the start delivers a record twice at worst, and loses nothing */
    if ( (::fstat (fdCommit, &StatBuf) < 0) || (StatBuf.st_size != static_cast<off_t> (sizeof (Offset))) ||
         (sizeof (Offset) != static_cast<size_t> (::read (fdCommit, &Offset, sizeof (Offset)))) )
    {
        QueueWarning ("WAL: ignoring the damaged commit file " + m_CommitPath);
        Offset = 0;
    }

    ::close (fdCommit);

    return Offset;
}


bool SimpleWAL::StoreCommit (uint64_t Offset)
{
    int fdCommit = ::open (m_CommitPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY | O_CLOEXEC, WAL_FILE_MODE);

    if (fdCommit < 0)
    {
        QueueErrno ("open commit failed");
        return false;
    }

    /* The position on disk changes. A record which was read with Peek() before is not the one at the position any more */
    m_PositionEpoch++;

    /* A commit file of an earlier version was created with more permissions */
    if (::fchmod (fdCommit, WAL_FILE_MODE) < 0)
    {
        QueueInfo ("Cannot restrict the permissions of the commit file");
    }

    bool bWritten = WriteAllToFd (fdCommit, &Offset, sizeof (Offset));

    if (false == bWritten)
    {
        QueueErrno ("Cannot write the commit file");
    }

    if (m_bSync)
    {
        ::fsync (fdCommit);
    }

    ::close (fdCommit);
    fdCommit = -1;

    return bWritten;
}
