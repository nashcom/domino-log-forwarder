
/* FileReader - follows a text file which grows, and hands out its lines one at a time. It remembers how far the lines were delivered,
   so that a restart continues where the last one stopped. For one process, and for one thread: the thread which reads is the thread
   which commits. It has no other dependency than the C++ standard library and POSIX (Linux).
   README.md describes the module.

   How it works
   ------------
   * ReadLine() returns the next line which is complete: it ends with a new line. What the writer has written of a line which is not
     finished stays in a buffer, and it is returned when the rest arrives. ReadLine() never waits: it returns false if there is nothing
     complete now, and the application asks again later (a polling loop with a short sleep is fine).
   * A line is returned without its new line, and without a carriage return before it (Windows files). Every line has the
     Generation of the file it comes from and the EndOffset: the position in the file behind the line.
   * Commit() tells the reader that the lines up to an EndOffset were delivered for good (accepted by the receiver, or safe in a WAL).
     Only then the position is saved in the state file. So a crash or a restart repeats lines which were read and not committed: delivery
     is at least once, and no line is lost. A program which does not care can commit after every line, but committing once for a
     batch of lines is much cheaper: the state file is written and flushed to disk for every commit which moves the position.
   * The state file (<state path>) holds the identity of the file (device and inode), the committed position, and a fingerprint: a
     hash of the first bytes of the file. A restart continues at the position only if the file is still the same: a file which was
     replaced or which has other content at its start is read from the beginning.
   * The file does not have to exist. The reader waits for it, and reads it from the beginning when it appears (a file which
     appears is new).

   What happens to the file
   ------------------------
   * Rotation (the path now leads to another file, as with logrotate without copytruncate): the old file is read to its end first,
     including a last line which has no new line, then the reader continues with the new file from its beginning. The reader waits
     one call of ReadLine() before it lets go of the old file, for a writer which is still busy with it. What a program writes to
     the old file later than that is lost. The Generation of the lines changes.
   * Truncation (copytruncate, or a file which is emptied): the size is smaller than the position, or the start of the file has
     changed. The reader starts again at the beginning, the line which was not finished is dropped, and the Generation changes. A file
     which was truncated and written again to the same size or more, with the same first bytes, cannot be told from a file which was
     only appended to. That is the limit of every reader which does not own the file.
   * Deleted: the reader keeps reading what is left of the file, and waits for a new one.
   * Generation: a number which counts the files (and the restarts from the beginning) since Open(). A Commit() for an older Generation
     is ignored: the state file already belongs to the new file.

   Long lines
   ----------
   * A line longer than SetMaxLineLength() (1 MB by default) is returned cut to that length, with bTruncated set. The rest of it is
     dropped up to the next new line. The EndOffset of a line which was cut in the middle of the file stops right behind the
     part which was returned: a restart at that position would return the rest as a line of its own. (If the new line is
     already there, the EndOffset is behind it.)

   Where to start
   --------------
   * With a valid state file the reader continues at the saved position. Without one (the first start, or a damaged file) it starts at
     the beginning of the file, or at its end with SetStartAtEnd(true): only lines which are written later. The position is saved
     at once, so that a restart before the first Commit() does not start again at the new end.

   Incomplete last line
   --------------------
   * By default the reader waits for the new line, as long as it takes. A program which does not always end its last line can set
     SetIncompleteLineSeconds(): a line which did not grow for that time is returned as it is. The rest of it, when it comes,
     is a line of its own. Time is a parameter of ReadLine(), so the application decides what the time is.

   Messages
   --------
   * The reader writes messages for what an operator has to know, for example "[Warning] FileReader: /var/log/app.log was truncated".
     A message is one line of text without the new line. By default they are written to stdout with printf. SetLogFunction()
     gives them to a function of the application instead.
   * A file which does not exist is no message: that is normal for a log which is written later. An error which is repeated (the
     state file cannot be written) is a message once, and again after it went away. */

#pragma once

#include <cstdint>
#include <ctime>
#include <functional>
#include <string>
#include <sys/stat.h>
#include <sys/types.h>


class FileReader
{

public:

    /* Receives a message: one line, complete with the level, without the new line */
    typedef std::function<void (const char *)> LogFunction;

    /* A line of the file */
    struct Line
    {
        std::string Text;                   // the line without the new line
        uint64_t    Generation = 0;         // the file it comes from (see Commit)
        uint64_t    EndOffset  = 0;         // the position in the file behind the line: what Commit() takes
        bool        bTruncated = false;     // the line was longer than the maximum and was cut
    };

    FileReader();
    ~FileReader();

    FileReader (const FileReader&)            = delete;
    FileReader& operator= (const FileReader&) = delete;

    /* Settings. Before Open() */

    /* The longest line which is returned as a whole. Longer lines are cut, see above. 1 MB by default. 0 sets the default */
    void SetMaxLineLength (size_t Bytes);

    /* Where to start without a state file: false is the beginning of the file (the default), true is its end */
    void SetStartAtEnd (bool bStartAtEnd);

    /* Seconds after which a last line without a new line is returned as it is. 0 (the default) waits for the new line */
    void SetIncompleteLineSeconds (unsigned Seconds);

    /* Gives every message to the function, instead of stdout. Without a function the messages are written to stdout */
    void SetLogFunction (LogFunction Function);

    /* Sets the file and the state file. The file does not have to exist. Returns false if a path is empty. Nothing is read or
       written here: the file is opened by the first ReadLine(). Calling it again starts from scratch with other paths */
    bool Open (const std::string& Path, const std::string& StatePath);

    /* Lets go of the file. What was committed stays in the state file */
    void Close();

    /* The next complete line. Returns false if there is none now (the end of the file, or the file does not exist yet). tNow is only
       used for SetIncompleteLineSeconds(): pass time (NULL) */
    bool ReadLine (Line& retLine, time_t tNow);

    /* The lines up to EndOffset (of a line of this Generation) were delivered: saves the position. Returns false if it could not
       be saved (the next Commit() tries again with the same or a higher position) or if EndOffset is behind what was returned.
       An older Generation, and a position which was committed already, are ignored and return true */
    bool Commit (uint64_t Generation, uint64_t EndOffset);

    /* Information */
    bool     IsFileOpen()    const { return m_Fd >= 0; }
    uint64_t GetGeneration() const { return m_Generation; }
    uint64_t GetCommitted()  const { return m_Committed; }      // the position in the state file (of the current Generation)

private:

    enum StateResult
    {
        STATE_NONE,         // there is no state file
        STATE_INVALID,      // there is one, and it cannot be used
        STATE_OK
    };

    struct SavedState
    {
        uint64_t Device   = 0;
        uint64_t Inode    = 0;
        uint64_t Offset   = 0;
        uint64_t HeadLen  = 0;
        uint64_t HeadHash = 0;
    };

    /* Settings */
    std::string  m_Path;
    std::string  m_StatePath;
    size_t       m_MaxLineLength  = 0;
    bool         m_bStartAtEnd    = false;
    unsigned     m_IncompleteSec  = 0;
    LogFunction  m_Log;

    /* The file which is read */
    bool         m_bOpened        = false;      // Open() was called
    bool         m_bFirstOpen     = true;       // the file was not opened yet: the state file decides where to start
    int          m_Fd             = -1;
    dev_t        m_Device         = 0;
    ino_t        m_Inode          = 0;
    uint64_t     m_Generation     = 0;
    std::string  m_Head;                        // the first bytes of the file: its fingerprint

    /* What was read and is not returned yet is m_Buffer from the index m_Pos on. Its first byte is at the position m_BufferStart in the
       file. Lines which were returned are only left behind m_Pos: the buffer is compacted once when more is read, not for every line */
    std::string  m_Buffer;
    size_t       m_Pos            = 0;
    uint64_t     m_BufferStart    = 0;
    size_t       m_ScanFrom       = 0;          // there is no new line in the first m_ScanFrom bytes of what is not returned yet
    bool         m_bSkipping      = false;      // the rest of a line which was too long is dropped up to the next new line
    bool         m_bRotationSeen  = false;
    time_t       m_tIncompleteSince = 0;

    /* What was saved */
    uint64_t     m_Committed      = 0;
    bool         m_bSaveFailed    = false;      // the message about a state file which cannot be written was given
    int          m_LastOpenError  = 0;          // the message about this error of open() was given

    /* Helpers which do not need the state of an object */
    static uint64_t HashBytes (const char *pData, size_t Len);
    static ssize_t  ReadAt (int fd, char *pBuffer, size_t Len, uint64_t Offset);
    static bool     WriteAll (int fd, const char *pData, size_t Len);

    void Log (const char *pszFormat, ...) __attribute__ ((format (printf, 2, 3)));

    size_t Pending() const { return m_Buffer.size() - m_Pos; }      // bytes which were read and are not returned yet
    void   ClearBuffer();

    bool OpenFile();
    void CloseFile();
    void ResolveStart (const struct stat& FileStat, uint64_t& retOffset);
    bool ExtractLine (Line& retLine);
    void EmitRemainder (Line& retLine);
    bool CheckFile();
    bool RefreshHead();
    void ResetFile (const char *pszReason);
    bool PathReplaced();
    ssize_t FillBuffer();

    StateResult LoadState (SavedState& retState);
    bool        SaveState();
};
