
/* SimpleWAL - a write ahead log for records of any kind of bytes, for one process, used from any number of threads.

   Used by otelfwd for the push requests which could not be delivered. It has no other dependency than the C++ standard library
   and POSIX (Linux). The module does not know what a record is. A complete example is wal_sample.cpp,
   and README.md describes the module.

   How it works
   ------------
   * Append() adds a record at the end of the file <path>. A record is a 4 byte length and the data.
   * The records are taken out in the order they were written, in one of two ways:
       Replay()      hands every record to a function of the application, until the function says stop
       Peek(), Ack() reads the oldest record, and it stays in the WAL until it is acknowledged. No function, no waiting:
                     the way for an application which has a loop of its own and must not block
   * What was accepted (Replay: the function returned true. Ack) is not delivered again. <path>.commit holds the position of
     the first record which was not accepted yet. When everything was accepted, the WAL is emptied.
   * Delivery is at least once. After a crash, or when the position could not be saved, a record can be delivered again. None is lost.
   * <path>.corrupt receives data which cannot be read (a record which was cut off by a crash, or a damaged length).
   * SetMaxSize() limits the size of the file. A record which does not fit is refused: Append() returns false. The file only
     gets smaller when the WAL is emptied, so a consumer which is slow can fill it up to the limit.
   * The files are only accessible for their owner (0600), and they are not inherited by programs which are started.
   * One WAL object at a time can use a file. A second object, in this or another process, cannot open it (flock). The lock ends
     with the object.
   * The files hold numbers in the byte order of the machine. They are not meant to be moved to another kind of machine.

   Threads
   -------
   * Init() is called before the object is used by other threads, and the object is destroyed after they are done. That is all
     which the application has to take care of. Every other function can be called from any thread at any time.
   * One mutex protects everything, the settings too. The application's functions (the function of Replay, the log function) are never
     called while it is held. They can call the WAL. A slow function of Replay (a slow receiver) does not stop Append().
   * Only one thread takes records out at a time. A second Replay() returns false at once, and so does Peek() while a Replay() runs.
     Records which are appended during a Replay() are part of it, and they are not lost when it ends: the WAL is only emptied
     if nothing was appended.
   * Peek() and Ack() belong together: one consumer, one record at a time. Ack() only acknowledges the record which Peek() returned.
     If the WAL was emptied (Clear) or a Replay() moved the position in between, Ack() returns false and does nothing: the
     record is read again, at least once.

   Messages
   --------
   * The WAL writes messages for what an operator has to know: errors and warnings ("[Error] WAL: unreadable data at offset ...").
     With SetLogLevel() above 0 also information ("WAL reset"). A message is one line of text without the new line.
   * By default they are written to stdout with printf. SetLogTarget() changes that for one object, SetDefaultLogTarget() for all
     objects which have no target of their own: stderr, or nothing. No function is needed for that.
   * SetLogFunction() gives every message to a function of the application: void (const char *pszMessage). It replaces the target.
     It can be called from any thread, also from two at the same time. It is called without a lock held. */

#pragma once

#include <atomic>
#include <cstdint>
#include <sys/types.h>
#include <functional>
#include <mutex>
#include <string>
#include <vector>


class SimpleWAL
{

public:

    /* Receives a message: one line, complete with the level, without the new line */
    typedef std::function<void (const char *)> LogFunction;

    /* Receives the records of a replay. Returns true if the record was accepted, false to stop (the record stays in the WAL) */
    typedef std::function<bool (const std::vector<uint8_t>&)> Consumer;

    /* Where the messages go if there is no function. LOG_DEFAULT: what SetDefaultLogTarget() says (stdout at start) */
    enum LogTarget
    {
        LOG_DEFAULT = 0,
        LOG_STDOUT,
        LOG_STDERR,
        LOG_NONE
    };

    SimpleWAL ();
    ~SimpleWAL();

    /* Opens the WAL. Not for use from other threads while it runs. Returns false if the file cannot be opened or is in use */
    bool Init (const std::string& Path);

    /* Append a binary record. A record of 0 bytes is not written. Returns false if it could not be stored: the WAL is not
       opened, the disk is full, or the size limit is reached. Nothing of a record which was refused is left in the file */
    bool Append (const void* pData, uint32_t Len);

    /* Replay from last committed offset. Consume() must return true on success, false on failure
       Returns true if at least one record was replayed and the progress was saved, or if everything was replayed already and the WAL
       is empty now. Returns false if nothing was replayed, or if a change could not be saved (the commit position, clearing the WAL),
       or if another thread takes records out at the moment. The WAL stays pending then, and records can be delivered again: at least
       once, never lost
       A commit offset which is not the start of a record (damaged, or of an older WAL) is not used: the WAL is replayed from the start
       A commit offset behind the end of the WAL is stale: the WAL is replayed from the start
       A record which does not fit into the rest of the file (cut off by a crash, or a damaged length) and everything behind it
       is moved to <wal>.corrupt. The WAL is not stuck at it */
    bool Replay (const Consumer& Consume);

    /* Reads the oldest record which was not acknowledged. Returns false if there is none, or if another thread replays. The record
       stays in the WAL: without an Ack() the next Peek() returns the same one. The checks and repairs are those of Replay() */
    bool Peek (std::vector<uint8_t>& Record);

    /* The record which Peek() returned is done: the position behind it is saved, and the WAL is emptied if it was the last one.
       This is one small write of a file for every record. For many records a second, use Replay(). Returns false if there is
       nothing to acknowledge, or if the WAL was emptied or replayed since Peek() */
    bool Ack();

    /* Clear WAL and start again */
    bool Clear();

    bool IsReplayPending();

    /* The size of the file in bytes, the size limit (0: no limit) */
    uint64_t GetSize();
    void SetMaxSize (uint64_t Bytes);

    /* Write the files to the disk after every record (fsync). Off by default */
    bool GetSync();
    void SetSync (bool bEnable);

    /* Above 0: also the messages with information, not only errors and warnings */
    void SetLogLevel (size_t LogLevel);

    /* A message of the application, in the same way as the messages of the WAL. Only written if the log level is above 0 */
    void LogMessage (const char *pszMessage);

    void SetLogFunction (const LogFunction& Function);         // an empty function: back to the target
    void SetLogTarget (LogTarget Target);
    static void SetDefaultLogTarget (LogTarget Target);        // for all objects which have no target of their own

private:

    class Locked;

    enum ReadResult
    {
        READ_RECORD = 0,    // a record was read
        READ_EMPTY,         // nothing is left: the WAL was emptied
        READ_DAMAGED,       // the rest was cut off or damaged: moved to <wal>.corrupt, and the WAL was emptied
        READ_CHANGED,       // the position is behind the end of the file: another thread emptied the WAL
        READ_STOP           // a failure (read, memory, moving the damaged part)
    };

    std::mutex m_mutex;
    std::string m_WalPath;
    std::string m_CommitPath;
    std::string m_CorruptPath;
    int m_fd;
    bool m_bSync;
    size_t m_LogLevel;
    uint64_t m_MaxSize;         // 0: no limit
    bool m_bFullLogged;         // the message that the WAL is full was written, and nothing was stored since

    bool m_PendingReplay;
    bool m_bCommitChecked;      // the commit position was checked against the records of the WAL (once, when the WAL is opened)
    bool m_bReplaying;          // a thread replays: no second one at the same time

    /* The record which Peek() returned and Ack() has to acknowledge. m_PositionEpoch counts the changes of the position on
       disk (saved, or the WAL emptied): if it is not the number of Peek(), somebody else moved it */
    bool     m_bPeeked;
    uint64_t m_PeekOffset;
    uint32_t m_PeekLen;
    uint64_t m_PeekEpoch;
    uint64_t m_PositionEpoch;

    LogFunction m_LogFunction;
    LogTarget   m_LogTarget;
    std::vector<std::string> m_LogQueue;    // messages which were made while the mutex was held. Given out when it is released

    static std::atomic<int> s_DefaultLogTarget;

    SimpleWAL (const SimpleWAL&) = delete;
    SimpleWAL& operator= (const SimpleWAL&) = delete;

    // These are only called with the mutex held
    void SetPaths (const std::string& Path);
    void QueueLine (const char *pszLevel, const std::string& Text);
    void QueueInfo (const std::string& Text);
    void QueueWarning (const std::string& Text);
    void QueueError (const std::string& Text);
    void QueueErrno (const char *pszText);          // an error with the text of errno. Call it before anything can change errno
    LogTarget GetEffectiveTarget();

    // The size of an open file. 0 for no descriptor, -1 if it cannot be read
    static off_t GetFileSize (int fd);

    bool WriteAll (const void* pBuf, size_t Len);
    uint64_t LoadCommit();
    bool StoreCommit (uint64_t Offset);
    // Where a replay starts: the position of the commit file, and repaired if it is stale or not the start of a record
    uint64_t GetReplayPosition (int fd, off_t FileSize);
    // Reads the record which starts at Offset, and repairs what is damaged. retClearOk: the result of emptying the WAL, if it was emptied
    ReadResult ReadRecord (int fd, uint64_t Offset, std::vector<uint8_t>& Record, uint32_t& retLen, bool& retClearOk);
    // Is Offset the start of a record? Follows the records of the WAL file from the start, reading only their lengths
    bool IsRecordBoundary (int fd, uint64_t Offset, uint64_t FileSize);
    // Copies the bytes From..To of the WAL file to <wal>.corrupt. Used for a part of the WAL which cannot be read
    bool QuarantineTail (int fdWal, uint64_t From, uint64_t To);
    // Clear WAL and start again
    bool ClearInternal();
};
