
#include <stdexcept>
#include <new>
#include <cstring>
#include <mutex>

#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>

#include "simple_wal.hpp"


/* The files of the WAL contain log lines. Only the owner may access them */
#define WAL_FILE_MODE 0600


off_t get_file_size (int fd)
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


void SimpleWAL::LogMessage (const char *pszMessage)
{
    if (NULL == pszMessage)
        return;

    if (m_LogLevel)
    {
        fprintf (stderr, "%s\n", pszMessage);
    }
}

SimpleWAL::SimpleWAL ()
{
    m_fd = -1;
    m_bCommit = false;
    m_LogLevel = 0;
    m_PendingReplay = false;
    m_bCommitChecked = false;
}


bool SimpleWAL::Init (const std::string& Path)
{
    SetWalFile (Path);

    m_bCommitChecked = false;

    m_fd = ::open (m_WalPath.c_str(), O_CREAT | O_APPEND | O_WRONLY, WAL_FILE_MODE);

    if (m_fd < 0)
    {
        perror ("open wal failed");
        return false;
    }

    /* A WAL of an earlier version was created with more permissions. This does not fail if the file belongs to somebody else */
    if (::fchmod (m_fd, WAL_FILE_MODE) < 0)
    {
        LogMessage ("Cannot restrict the permissions of the WAL file");
    }

    if (get_file_size (m_fd) > 0)
        m_PendingReplay = true;
    else
        m_PendingReplay = false;

    return true;
}


SimpleWAL::~SimpleWAL()
{
    bool bRemoveFiles = false;

    if (m_fd >= 0)
    {
        if (get_file_size (m_fd) <= 0)
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


/* --- Public functions --- */


bool SimpleWAL::IsReplayPending()
{
   std::lock_guard<std::mutex> lock (m_mutex);
   return m_PendingReplay;
}


bool SimpleWAL::Append (const void* pData, uint32_t Len)
{
    if (0 == Len)
        return true;

    if (m_fd < 0)
    {
        return false;
    }

    {
        std::lock_guard<std::mutex> lock (m_mutex);

        /* The WAL is opened for appending: the end of the file is where this record starts */
        off_t Start = ::lseek (m_fd, 0, SEEK_END);

        if (Start < 0)
        {
            perror ("Cannot get the end of the WAL");
            return false;
        }

        if ( (false == WriteAll (&Len, sizeof (Len))) || (false == WriteAll (pData, Len)) )
        {
            /* Do not leave a piece of the record behind (the disk is full, for example). Every record after it would be unreadable */
            if (::ftruncate (m_fd, Start) < 0)
                perror ("Cannot remove the partial WAL record");

            return false;
        }

        if (m_bCommit)
        {
            ::fsync (m_fd);
        }

        m_PendingReplay = true;
    }

    return true;
}


bool SimpleWAL::ReplaySingleCommit (const std::function<bool (const std::vector<uint8_t>&)>& Consume)
{
    uint64_t Offset = 0;
    int fd = -1;

    std::lock_guard<std::mutex> lock (m_mutex);

    Offset = LoadCommit();

    fd = ::open (m_WalPath.c_str(), O_RDONLY);

    if (fd < 0)
    {
        goto Done;
    }

    if (::lseek (fd, Offset, SEEK_SET) < 0)
    {
        perror ("Cannot seek WAL file");
        goto Done;
    }

    while (true)
    {
        uint32_t Len;
        ssize_t ReadBytes = ::read (fd, &Len, sizeof (Len));

        if (ReadBytes == 0)
            break;

        if (ReadBytes != sizeof (Len))
            break;

        std::vector<uint8_t> Buffer (Len);

        ReadBytes = ::read (fd, Buffer.data(), Len);

        if (ReadBytes != static_cast<ssize_t> (Len))
        {
            break;
        }

        if (!Consume (Buffer))
        {
            goto Done;
        }

        Offset += sizeof (Len) + Len;
        StoreCommit (Offset);
    }

Done:

    if (fd >= 0)
    {
        ::close (fd);
        fd = -1;
    }

    return true;
}


bool SimpleWAL::Replay (const std::function<bool (const std::vector<uint8_t>&)>& Consume)
{
    uint64_t Offset = 0;
    uint64_t NewOffset = 0;
    int fd = -1;
    off_t FileSize = 0;
    bool bDidReplay = false;
    bool bEmpty     = false;

    std::lock_guard<std::mutex> lock (m_mutex);

    Offset = LoadCommit();
    NewOffset = Offset;

    fd = ::open (m_WalPath.c_str(), O_RDONLY);

    if (fd < 0)
        return false;

    FileSize = get_file_size (fd);

    if (FileSize < 0)
    {
        perror ("Cannot get the size of the WAL");
        ::close (fd);
        return false;
    }

    /* A commit position behind the end of the WAL is stale: it belongs to an earlier WAL which was truncated, and the commit
       file was not removed (for example a crash in between). Replay the WAL from the start */
    if (Offset > static_cast<uint64_t> (FileSize))
    {
        fprintf (stderr, "WAL: Warning - commit offset %llu is behind the end of the WAL (%llu bytes). Replaying from the start\n",
                 static_cast<unsigned long long> (Offset), static_cast<unsigned long long> (FileSize));

        Offset    = 0;
        NewOffset = 0;
    }

    /* The commit position has to be the start of a record. A commit file with the right size but a wrong value (damaged, or left
       by an older WAL) would make the replay read the middle of a record as a length and skip or misread valid records. This is
       checked once, when the WAL was opened: after that the position is one which this program wrote. If it does not fit, replay
       the WAL from the start: a record can be delivered twice, none is lost */
    if (false == m_bCommitChecked)
    {
        if ( (Offset > 0) && (Offset < static_cast<uint64_t> (FileSize)) && (false == IsRecordBoundary (fd, Offset, static_cast<uint64_t> (FileSize))) )
        {
            fprintf (stderr, "WAL: Warning - commit offset %llu is not the start of a record. Replaying from the start\n",
                     static_cast<unsigned long long> (Offset));

            Offset    = 0;
            NewOffset = 0;
        }

        m_bCommitChecked = true;
    }

    /* A commit position at the end of the WAL means that everything was replayed, but the WAL was not cleared (for example a
       crash in between). Clear it now, otherwise it would stay pending forever */
    if (Offset == static_cast<uint64_t> (FileSize))
    {
        ::close (fd);
        return ClearInternal();
    }

    ::lseek (fd, Offset, SEEK_SET);

    while (true)
    {
        uint32_t Len = 0;
        uint64_t Remaining = static_cast<uint64_t> (FileSize) - NewOffset;
        ssize_t ReadBytes = 0;

        /* Everything is replayed. The WAL is only appended to under the lock, which is held: the size does not change */
        if (0 == Remaining)
        {
            bEmpty = true;
            break;
        }

        ReadBytes = ::read (fd, &Len, sizeof (Len));

        if (ReadBytes < 0)
        {
            perror ("Cannot read the WAL");
            break;
        }

        /* The length has to fit into the rest of the file. Otherwise the record was cut off by a crash or the length is damaged,
           and everything behind it is not readable either. That part is moved to the .corrupt file, and the WAL continues with
           what is before it. This is also the limit for the memory: a record is never larger than the WAL file */
        if ( (static_cast<size_t> (ReadBytes) != sizeof (Len)) || (0 == Len) || (Len > Remaining - sizeof (Len)) )
        {
            if (QuarantineTail (fd, NewOffset, static_cast<uint64_t> (FileSize)))
                bEmpty = true;

            break;
        }

        std::vector<uint8_t> Buffer;

        try
        {
            Buffer.resize (Len);
        }
        catch (const std::bad_alloc&)
        {
            fprintf (stderr, "WAL: Error - not enough memory for a record of %lu bytes\n", static_cast<unsigned long> (Len));
            break;
        }

        ReadBytes = ::read (fd, Buffer.data(), Len);

        if (ReadBytes != static_cast<ssize_t> (Len))
        {
            break;
        }

        if (!Consume (Buffer))
        {
            break;
        }

        NewOffset += sizeof (Len) + Len;
        bDidReplay = true;
    }

    ::close (fd);

    /* The whole WAL is replayed or moved away: start with an empty one */
    if (bEmpty)
        return ClearInternal();

    if (bDidReplay)
    {
        /* The records were delivered, but the position is not saved: the next replay delivers them again. Do not report progress
           which is not saved. The caller then waits before it tries again, instead of sending the same records again at once */
        if (false == StoreCommit (NewOffset))
            return false;
    }

    return bDidReplay;
}


bool SimpleWAL::Clear()
{
    std::lock_guard<std::mutex> lock (m_mutex);

    return ClearInternal();
}


/* --- Private functions --- */


bool SimpleWAL::ClearInternal()
{
    if (m_fd < 0)
        return true;

    /* Remove the commit file first. A crash after this replays the whole WAL again, which can deliver a record twice.
       The other order could leave a commit position behind which does not fit to the next WAL */
    if ( (::unlink (m_CommitPath.c_str()) < 0) && (ENOENT != errno) )
    {
        /* The WAL is left as it is. Truncating it now would leave a commit position behind which does not fit to the next records */
        perror ("Cannot remove the commit file");
        return false;
    }

    if (::ftruncate (m_fd, 0) < 0)
    {
        perror ("file truncate failed");
        return false;
    }

    if (::lseek (m_fd, 0, SEEK_SET) < 0)
    {
        perror ("file seek failed");
        return false;
    }

    m_PendingReplay = false;

    LogMessage ("WAL reset");

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
    int fdCorrupt = ::open (m_CorruptPath.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0600);

    if (fdCorrupt < 0)
    {
        perror ("Cannot open the file for unreadable WAL data");
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
            perror ("Cannot copy unreadable WAL data");
            ::close (fdCorrupt);
            return false;
        }

        Pos += static_cast<uint64_t> (ReadBytes);
    }

    ::close (fdCorrupt);

    fprintf (stderr, "WAL: Error - unreadable data at offset %llu (%llu bytes) moved to %s\n",
             static_cast<unsigned long long> (From), static_cast<unsigned long long> (To - From), m_CorruptPath.c_str());

    return true;
}


bool SimpleWAL::WriteAll (const void* pBuf, size_t Len)
{
    if (m_fd < 0)
        return false;

    if (false == WriteAllToFd (m_fd, pBuf, Len))
    {
        perror ("Cannot write to WAL");
        return false;
    }

    return true;
}

uint64_t SimpleWAL::LoadCommit()
{
    int fdCommit = ::open (m_CommitPath.c_str(), O_RDONLY);

    if (fdCommit < 0)
        return 0;

    struct stat StatBuf;
    uint64_t Offset = 0;

    /* The file is exactly one offset. Anything else is damaged (a write which was cut off, for example) and is not used:
       replaying the WAL from the start delivers a record twice at worst, and loses nothing */
    if ( (::fstat (fdCommit, &StatBuf) < 0) || (StatBuf.st_size != static_cast<off_t> (sizeof (Offset))) ||
         (sizeof (Offset) != static_cast<size_t> (::read (fdCommit, &Offset, sizeof (Offset)))) )
    {
        fprintf (stderr, "WAL: Warning - ignoring the damaged commit file %s\n", m_CommitPath.c_str());
        Offset = 0;
    }

    ::close (fdCommit);

    return Offset;
}


bool SimpleWAL::StoreCommit (uint64_t Offset)
{
    int fdCommit = ::open (m_CommitPath.c_str(), O_CREAT | O_TRUNC | O_WRONLY, WAL_FILE_MODE);

    if (fdCommit < 0)
    {
        perror ("open commit failed");
        return false;
    }

    /* A commit file of an earlier version was created with more permissions */
    if (::fchmod (fdCommit, WAL_FILE_MODE) < 0)
    {
        LogMessage ("Cannot restrict the permissions of the commit file");
    }

    bool bWritten = WriteAllToFd (fdCommit, &Offset, sizeof (Offset));

    if (false == bWritten)
    {
        perror ("Cannot write the commit file");
    }

    if (m_bCommit)
    {
        ::fsync (fdCommit);
    }

    ::close (fdCommit);
    fdCommit = -1;

    return bWritten;
}
