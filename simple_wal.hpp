#pragma once

#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>


class SimpleWAL
{

public:

    SimpleWAL ();
    ~SimpleWAL();

    bool Init (const std::string& Path);

    void LogMessage (const char *pszMessage);

    // Append a binary record
    bool Append (const void* pData, uint32_t Len);

    // Replay from last committed offset
    // Consume() must return true on success, false on failure
    // Returns true if at least one record was replayed and the progress was saved, or if everything was replayed already and the WAL
    // is empty now. Returns false if nothing was replayed, or if a change could not be saved (the commit position, clearing the WAL).
    // The WAL stays pending then, and records can be delivered again: at least once, never lost
    // A commit offset which is not the start of a record (damaged, or of an older WAL) is not used: the WAL is replayed from the start
    // A commit offset behind the end of the WAL is stale: the WAL is replayed from the start
    // A record which does not fit into the rest of the file (cut off by a crash, or a damaged length) and everything behind it
    // is moved to <wal>.corrupt. The WAL is not stuck at it
    bool Replay (const std::function<bool (const std::vector<uint8_t>&)>& Consume);

    bool ReplaySingleCommit (const std::function<bool (const std::vector<uint8_t>&)>& Consume);

    // Clear WAL and start again
    bool Clear();

    bool IsReplayPending();

    bool GetCommit() {return m_bCommit; };
    void SetCommit (bool bEnable) {m_bCommit = bEnable; };
    void SetLogLevel (size_t LogLevel) {m_LogLevel = LogLevel; };

    void SetWalFile (const std::string& Path) 
    {
        m_WalPath = Path;
        m_CommitPath = m_WalPath + ".commit";
        m_CorruptPath = m_WalPath + ".corrupt";
    }

private:

    std::mutex m_mutex;
    std::string m_WalPath;
    std::string m_CommitPath;
    std::string m_CorruptPath;
    int m_fd;
    bool m_bCommit;
    size_t m_LogLevel;

    bool m_PendingReplay;
    bool m_bCommitChecked;      // the commit position was checked against the records of the WAL (once, when the WAL is opened)

    bool WriteAll (const void* pBuf, size_t Len);
    uint64_t LoadCommit();
    bool StoreCommit (uint64_t Offset);
    // Is Offset the start of a record? Follows the records of the WAL file from the start, reading only their lengths
    bool IsRecordBoundary (int fd, uint64_t Offset, uint64_t FileSize);
    // Copies the bytes From..To of the WAL file to <wal>.corrupt. Used for a part of the WAL which cannot be read
    bool QuarantineTail (int fdWal, uint64_t From, uint64_t To);
    // Clear WAL and start again
    bool ClearInternal();
};

