
/* Unit test of the WAL module (simple_wal.cpp). A separate program which only links the WAL: no otelfwd, no network.

   Build and run:  cd wal && make test        (or: make wal_unit_test && ./wal_unit_test. From the repository root: make test)
   Options:        -n COUNT      number of records for the producer and consumer test and for the performance table (default 100000)
                   DIRECTORY     the parent directory of the test files (default /tmp, which can be a RAM file system). The test makes
                                 a directory of its own in it and removes it at the end. Use it to measure a real disk, for example
                                 the directory of the WAL of otelfwd

   Everything happens in a private temporary directory. Every check prints [PASS] or [FAIL]. The end of the output has a
   section "Failed checks" (only if there are any) and a section "Result" with the overall status. The exit code is 0 if every check passed and 1 if any check failed.

   Groups of tests, in the order of the output:

   - Messages of the WAL:    the messages of the WAL: the function of the application, the targets without a function, that the
                             function can call the WAL, and that only one object can use a file
   - Threads:                one WAL used by several threads: the function of a replay can call the WAL, an append does not wait for
                             a slow replay, settings can change while others append, several replaying threads. Checks named
                             "[thread]". The mistakes which only a sanitizer sees are found with "make tsan"
   - Behaviour:              what the WAL is meant to do (order, partial replay, restart, concurrency, large records ...)
   - Peek and Ack:           reading one record at a time without a function: the same record until it is acknowledged, the position
                             after a restart, records appended meanwhile, Clear() and Replay() between the read and the ack, damaged
                             files, several producers with one consumer
   - Size limit:             SetMaxSize: a record which does not fit is refused, nothing of it is left, one message
   - Damaged files:          the replay must not get stuck: a commit position at the end, behind the end or inside a record, a record
                             which was cut off by a crash, a damaged length, a damaged commit file. Nothing valid is thrown away
   - Failures while saving:  a full disk, and files which cannot be written, truncated, removed or copied. The WAL does not report
                             progress which is not saved
   - Files, settings, crashes: the modes of the files, initialized members, the sync option, and real crashes
   - Performance:            speed of appending, replaying and starting with a backlog

   The name of a check starts with what it tests, for example "torn tail:" or "stale commit:" */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/resource.h>
#include <sys/wait.h>
#include <sys/vfs.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <new>
#include <string>
#include <thread>
#include <vector>

#include "simple_wal.hpp"


/* Fault injection. The test is linked with -Wl,--wrap=ftruncate and -Wl,--wrap=unlink (see the makefile), so that every call
   of these two functions, also the calls of the WAL, goes through the functions below. They fail on request and otherwise
   call the real function. The WAL itself has no code for tests. Without the linker options the tests which use this fail */
static bool g_bFailTruncate      = false;
static bool g_bFailUnlinkCommit  = false;

extern "C" int __real_ftruncate (int fd, off_t Length);
extern "C" int __real_unlink (const char *pszPath);

extern "C" int __wrap_ftruncate (int fd, off_t Length)
{
    if (g_bFailTruncate)
    {
        errno = EIO;
        return -1;
    }

    return __real_ftruncate (fd, Length);
}

extern "C" int __wrap_unlink (const char *pszPath)
{
    size_t Len = pszPath ? strlen (pszPath) : 0;

    if (g_bFailUnlinkCommit && (Len >= 7) && (0 == strcmp (pszPath + Len - 7, ".commit")))
    {
        errno = EIO;
        return -1;
    }

    return __real_unlink (pszPath);
}


static int g_Total  = 0;
static int g_Failed = 0;
static std::string g_Dir;
static std::vector<std::string> g_FailedNames;


static void Check (bool bCondition, const char *pszName)
{
    printf ("[%s]  %s\n", bCondition ? "PASS" : "FAIL", pszName);

    g_Total++;

    if (false == bCondition)
    {
        g_Failed++;
        g_FailedNames.push_back (pszName);
    }
}


/* A header: the title between two lines of 80 characters, with an empty line before and after it */
static void Group (const char *pszTitle)
{
    std::string Line (80, '-');

    printf ("\n%s\n%s\n%s\n\n", Line.c_str(), pszTitle, Line.c_str());
}


/* --- Helpers to look at the files on disk. The tests do not trust the WAL to tell about itself --- */

static std::string WalPath (const char *pszName)
{
    return g_Dir + "/" + pszName + ".wal";
}


static long FileSize (const std::string& Path)
{
    struct stat StatBuf;

    if (0 != ::stat (Path.c_str(), &StatBuf))
        return -1;

    return static_cast<long> (StatBuf.st_size);
}


static bool FileExists (const std::string& Path)
{
    return (0 == ::access (Path.c_str(), F_OK));
}


static int FileMode (const std::string& Path)
{
    struct stat StatBuf;

    if (0 != ::stat (Path.c_str(), &StatBuf))
        return -1;

    return static_cast<int> (StatBuf.st_mode & 0777);
}


static void AppendBytes (const std::string& Path, const void *pData, size_t Len)
{
    int fd = ::open (Path.c_str(), O_CREAT | O_APPEND | O_WRONLY, 0600);

    if (fd >= 0)
    {
        if (::write (fd, pData, Len) < 0)
            perror ("write");

        ::close (fd);
    }
}


static void WriteCommitFile (const std::string& WalFile, uint64_t Offset)
{
    int fd = ::open ((WalFile + ".commit").c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);

    if (fd >= 0)
    {
        if (::write (fd, &Offset, sizeof (Offset)) < 0)
            perror ("write");

        ::close (fd);
    }
}


/* Returns false if there is no commit file or it is not exactly one offset */
static bool ReadCommitFile (const std::string& WalFile, uint64_t& retOffset)
{
    std::string Path = WalFile + ".commit";

    if (8 != FileSize (Path))
        return false;

    int fd = ::open (Path.c_str(), O_RDONLY);

    if (fd < 0)
        return false;

    bool bOk = (8 == ::read (fd, &retOffset, sizeof (retOffset)));

    ::close (fd);

    return bOk;
}


static void RemoveFiles (const std::string& WalFile)
{
    ::unlink (WalFile.c_str());
    ::unlink ((WalFile + ".commit").c_str());
    ::unlink ((WalFile + ".commit.tmp").c_str());
    ::unlink ((WalFile + ".corrupt").c_str());
}


/* --- Helpers to use the WAL --- */

static void AppendText (SimpleWAL& Wal, const char *pszText)
{
    Wal.Append (pszText, static_cast<uint32_t> (strlen (pszText)));
}


/* Replays and collects the records as text. The consumer accepts everything */
static bool ReplayAll (SimpleWAL& Wal, std::vector<std::string>& retRecords)
{
    return Wal.Replay ([&retRecords] (const std::vector<uint8_t>& Record)
    {
        retRecords.push_back (std::string (Record.begin(), Record.end()));
        return true;
    });
}


static bool StartsWith (const std::string& Text, const char *pszStart)
{
    return (0 == Text.compare (0, strlen (pszStart), pszStart));
}


static bool Contains (const std::vector<std::string>& Records, const char *pszText)
{
    for (const std::string& Record : Records)
    {
        if (Record == pszText)
            return true;
    }

    return false;
}


/* Returns what the action writes to a file descriptor: 1 is stdout, 2 is stderr. The action must not print a check: it would be
   captured too */
static std::string CaptureFd (int Fd, const std::function<void()>& Action)
{
    /* A file for each descriptor: a capture of stdout can be inside a capture of stderr */
    std::string Path   = g_Dir + "/capture" + std::to_string (Fd) + ".txt";
    std::string Result;
    char   szBuffer[512];
    size_t Read = 0;

    fflush (stdout);
    fflush (stderr);

    int fdOld = ::dup (Fd);
    int fdNew = ::open (Path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);

    ::dup2 (fdNew, Fd);
    ::close (fdNew);

    Action();

    fflush (stdout);
    fflush (stderr);
    ::dup2 (fdOld, Fd);
    ::close (fdOld);

    FILE *fp = fopen (Path.c_str(), "r");

    if (fp)
    {
        while ((Read = fread (szBuffer, 1, sizeof (szBuffer), fp)) > 0)
            Result.append (szBuffer, Read);

        fclose (fp);
    }

    ::unlink (Path.c_str());

    return Result;
}


static std::string CaptureStderr (const std::function<void()>& Action)
{
    return CaptureFd (2, Action);
}


static std::string CaptureStdout (const std::function<void()>& Action)
{
    return CaptureFd (1, Action);
}


/* ============================== Behaviour ============================== */


static void TestRoundTrip ()
{
    std::string Path = WalPath ("roundtrip");
    std::vector<std::string> Records;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Check (Wal.Init (Path), "round trip: init");
    Check (false == Wal.IsReplayPending(), "round trip: a new WAL has nothing pending");

    AppendText (Wal, "one");
    AppendText (Wal, "two");
    AppendText (Wal, "three");

    Check (Wal.IsReplayPending(), "round trip: pending after an append");
    Check (FileSize (Path) == 4 + 3 + 4 + 3 + 4 + 5, "round trip: each record is a 4 byte length and the data");
    Check (ReplayAll (Wal, Records), "round trip: replay reports success");
    Check (3 == Records.size() && "one" == Records[0] && "two" == Records[1] && "three" == Records[2], "round trip: all records, in order");
    Check (false == Wal.IsReplayPending(), "round trip: nothing pending afterwards");
    Check (0 == FileSize (Path), "round trip: the WAL file is empty afterwards");
    Check (false == FileExists (Path + ".commit"), "round trip: there is no commit file afterwards");
}


static void TestPartialReplay ()
{
    std::string Path = WalPath ("partial");
    std::vector<std::string> Records;
    int Count = 0;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "one");
    AppendText (Wal, "two");
    AppendText (Wal, "three");

    /* The receiver refuses the second record */
    bool bResult = Wal.Replay ([&Count] (const std::vector<uint8_t>&)
    {
        Count++;
        return (Count < 2);
    });

    uint64_t Commit = 0;

    Check (bResult, "partial replay: progress is reported as success");
    Check (Wal.IsReplayPending(), "partial replay: the rest is still pending");
    Check (ReadCommitFile (Path, Commit) && Commit == 4 + 3, "partial replay: the commit file holds the offset of the second record");

    Check (ReplayAll (Wal, Records), "partial replay: second replay succeeds");
    Check (2 == Records.size() && "two" == Records[0] && "three" == Records[1], "partial replay: only the records which were not replayed before");
    Check (false == Wal.IsReplayPending() && 0 == FileSize (Path) && false == FileExists (Path + ".commit"), "partial replay: cleared at the end");
}


static void TestNoProgress ()
{
    std::string Path = WalPath ("noprogress");
    std::vector<std::string> Records;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "one");
    AppendText (Wal, "two");

    bool bResult = Wal.Replay ([] (const std::vector<uint8_t>&) { return false; });

    Check (false == bResult, "no progress: reported as failure");
    Check (Wal.IsReplayPending(), "no progress: still pending");
    Check (false == FileExists (Path + ".commit"), "no progress: nothing is committed");

    Check (ReplayAll (Wal, Records) && 2 == Records.size(), "no progress: both records are delivered later");
}


static void TestAppendEdgeCases ()
{
    std::string Path = WalPath ("edge");
    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);

    Check (Wal.Append ("x", 0), "edge: an empty record is accepted");
    Check (0 == FileSize (Path) && false == Wal.IsReplayPending(), "edge: an empty record writes nothing");

    SimpleWAL Uninitialized;
    Check (false == Uninitialized.Append ("abc", 3), "edge: append is refused if the WAL was never opened");
    Check (false == Uninitialized.IsReplayPending(), "edge: nothing is pending if the WAL was never opened");

    SimpleWAL Impossible;
    Check (false == Impossible.Init (g_Dir + "/no/such/directory/x.wal"), "edge: init reports a path which cannot be opened");
    Check (false == Impossible.Append ("abc", 3), "edge: append is refused after a failed init");
}


static void TestRestart ()
{
    std::string Path = WalPath ("restart");
    std::vector<std::string> Records;

    RemoveFiles (Path);

    {
        SimpleWAL Wal;
        Wal.Init (Path);
        AppendText (Wal, "before the restart 1");
        AppendText (Wal, "before the restart 2");
    }

    Check (FileExists (Path), "restart: a WAL with records stays on disk when the program ends");

    SimpleWAL Wal2;
    Check (Wal2.Init (Path), "restart: init of the existing WAL");
    Check (Wal2.IsReplayPending(), "restart: the records are pending after the restart");

    AppendText (Wal2, "after the restart");
    Check (ReplayAll (Wal2, Records), "restart: replay succeeds");
    Check (3 == Records.size() && "before the restart 1" == Records[0] && "before the restart 2" == Records[1] && "after the restart" == Records[2],
          "restart: old and new records, in order");
}


static void TestPartialReplayRestart ()
{
    std::string Path = WalPath ("partialrestart");
    std::vector<std::string> Records;
    int Count = 0;

    RemoveFiles (Path);

    {
        SimpleWAL Wal;
        Wal.Init (Path);
        AppendText (Wal, "one");
        AppendText (Wal, "two");
        AppendText (Wal, "three");

        Wal.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return (Count < 3); });
    }

    SimpleWAL Wal2;
    Wal2.Init (Path);
    Check (ReplayAll (Wal2, Records) && 1 == Records.size() && "three" == Records[0], "partial replay and restart: only the record which was not replayed is delivered");
}


static void TestDestructor ()
{
    std::string Path = WalPath ("destructor");
    RemoveFiles (Path);

    {
        SimpleWAL Wal;
        Wal.Init (Path);
    }

    Check (false == FileExists (Path) && false == FileExists (Path + ".commit"), "destructor: an empty WAL removes its files");

    {
        SimpleWAL Wal;
        Wal.Init (Path);
        AppendText (Wal, "keep me");
    }

    Check (FileExists (Path), "destructor: a WAL with a record keeps its file");
    RemoveFiles (Path);
}


static void TestClear ()
{
    std::string Path = WalPath ("clear");
    std::vector<std::string> Records;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "one");
    Wal.Replay ([] (const std::vector<uint8_t>&) { return false; });
    AppendText (Wal, "two");

    Check (Wal.Clear(), "clear: succeeds");
    Check (0 == FileSize (Path) && false == FileExists (Path + ".commit") && false == Wal.IsReplayPending(), "clear: empty, no commit file, nothing pending");

    AppendText (Wal, "after clear");
    Check (ReplayAll (Wal, Records) && 1 == Records.size() && "after clear" == Records[0], "clear: the WAL is usable again");
}


static void TestLargeAndMany ()
{
    std::string Path = WalPath ("large");
    std::vector<std::string> Records;
    std::string Big (5 * 1024 * 1024, 'x');

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    Check (Wal.Append (Big.data(), static_cast<uint32_t> (Big.size())), "large: a record of 5 MB is accepted");
    Check (ReplayAll (Wal, Records) && 1 == Records.size() && Big == Records[0], "large: replayed unchanged");

    Records.clear();

    for (int i = 0; i < 10000; i++)
    {
        std::string Text = "record " + std::to_string (i);
        Wal.Append (Text.data(), static_cast<uint32_t> (Text.size()));
    }

    Check (ReplayAll (Wal, Records) && 10000 == Records.size(), "many: 10000 records are replayed");

    bool bOrder = true;

    for (size_t i = 0; i < Records.size(); i++)
    {
        if (Records[i] != "record " + std::to_string (i))
            bOrder = false;
    }

    Check (bOrder, "many: in the order they were appended");
}


static void TestConcurrentAppend ()
{
    std::string Path = WalPath ("concurrent");
    std::vector<std::string> Records;
    const int Threads    = 4;
    const int PerThread  = 2000;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);

    std::vector<std::thread> Workers;
    std::atomic<int> AppendFailed {0};

    for (int t = 0; t < Threads; t++)
    {
        Workers.emplace_back ([&Wal, &AppendFailed, t]
        {
            for (int i = 0; i < PerThread; i++)
            {
                std::string Text = std::to_string (t) + ":" + std::to_string (i) + ":" + std::string (200, 'a' + t);

                if (false == Wal.Append (Text.data(), static_cast<uint32_t> (Text.size())))
                    AppendFailed++;
            }
        });
    }

    for (std::thread& Worker : Workers)
        Worker.join();

    Check (0 == AppendFailed.load(), "concurrent: no append failed");
    Check (ReplayAll (Wal, Records), "concurrent: replay succeeds");
    Check (Records.size() == static_cast<size_t>(Threads * PerThread), "concurrent: every record of every thread is there");

    /* Records must be whole (not mixed with another thread) and in order per thread */
    std::vector<int> Next (Threads, 0);
    bool bIntact = true;

    for (const std::string& Record : Records)
    {
        size_t Colon1 = Record.find (':');
        size_t Colon2 = Record.find (':', Colon1 + 1);

        if (std::string::npos == Colon1 || std::string::npos == Colon2)
        {
            bIntact = false;
            continue;
        }

        int t = atoi (Record.substr (0, Colon1).c_str());
        int i = atoi (Record.substr (Colon1 + 1, Colon2 - Colon1 - 1).c_str());

        if (t < 0 || t >= Threads || i != Next[t] || Record.substr (Colon2 + 1) != std::string (200, 'a' + t))
        {
            bIntact = false;
            continue;
        }

        Next[t]++;
    }

    Check (bIntact, "concurrent: no record is mixed with another and each thread keeps its order");
}


/* One thread appends numbered records as fast as it can, another one replays what is pending, like the WAL thread of otelfwd does.
   This is what the old program wal_test did. Now it is checked: every record has to arrive exactly once and in order.
   The time is only printed for information */
static void TestProducerConsumer (size_t Records)
{
    std::string Path = WalPath ("producer");
    std::vector<uint32_t> Received;
    std::atomic<bool> bProducerDone {false};

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);

    std::chrono::steady_clock::time_point tStart = std::chrono::steady_clock::now();

    std::thread Consumer ([&Wal, &Received, &bProducerDone]
    {
        while (true)
        {
            /* Read this first: if the producer was done and nothing is pending afterwards, everything was replayed */
            bool bDone = bProducerDone.load();

            if (Wal.IsReplayPending())
            {
                Wal.Replay ([&Received] (const std::vector<uint8_t>& Record)
                {
                    uint32_t Value = 0xFFFFFFFFu;

                    if (sizeof (Value) == Record.size())
                        memcpy (&Value, Record.data(), sizeof (Value));

                    Received.push_back (Value);
                    return true;
                });
            }
            else if (bDone)
            {
                break;
            }
            else
            {
                std::this_thread::sleep_for (std::chrono::milliseconds (2));
            }
        }
    });

    size_t AppendFailed = 0;

    for (size_t i = 0; i < Records; i++)
    {
        uint32_t Value = static_cast<uint32_t> (i);

        if (false == Wal.Append (&Value, sizeof (Value)))
            AppendFailed++;
    }

    bProducerDone = true;
    Consumer.join();

    double Ms = std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now() - tStart).count();

    bool bInOrder = (Received.size() == Records);

    for (size_t i = 0; bInOrder && i < Received.size(); i++)
    {
        if (Received[i] != static_cast<uint32_t> (i))
            bInOrder = false;
    }

    Check (0 == AppendFailed, "producer and consumer: no append failed");
    Check (Received.size() == Records, "producer and consumer: every record arrives, none twice");
    Check (bInOrder, "producer and consumer: in the order they were appended");
    Check (0 == FileSize (Path) && false == Wal.IsReplayPending(), "producer and consumer: the WAL is empty at the end");

    printf ("        %zu records in %.0f ms, %.0f records/s, %.1f us per record (for information)\n",
            Records, Ms, Ms > 0 ? Records * 1000.0 / Ms : 0.0, Records > 0 ? Ms * 1000.0 / Records : 0.0);
}


/* ============================== Damaged files, failures, crashes ============================== */


/* The commit offset points exactly to the end of the WAL (everything was replayed, but the WAL was not cleared,
   for example after a crash). Replay must recognize that, clear the WAL and not stay pending forever */
static void TestCommitAtEnd ()
{
    std::string Path = WalPath ("f1end");
    int Consumed = 0;

    RemoveFiles (Path);

    {
        SimpleWAL Wal;
        Wal.Init (Path);
        AppendText (Wal, "aaaa");
        AppendText (Wal, "bbbb");
    }

    WriteCommitFile (Path, static_cast<uint64_t> (FileSize (Path)));

    SimpleWAL Wal;
    Wal.Init (Path);
    Wal.Replay ([&Consumed] (const std::vector<uint8_t>&) { Consumed++; return true; });

    Check (0 == Consumed, "commit at the end: nothing is delivered again");
    Check (false == Wal.IsReplayPending(), "commit at the end: the WAL is not pending forever");
    Check (0 == FileSize (Path), "commit at the end: the WAL file is cleared");
}


/* The WAL was truncated, but the commit file of the old WAL was not removed (crash in between). A new record which
   is appended afterwards must be replayed, not skipped because of the old offset */
static void TestStaleCommit ()
{
    std::string Path = WalPath ("f1stale");
    std::vector<std::string> Records;

    RemoveFiles (Path);
    WriteCommitFile (Path, 4096);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "new record");

    Check (ReplayAll (Wal, Records), "stale commit: replay succeeds");
    Check (Contains (Records, "new record"), "stale commit: the new record is delivered");
    Check (false == Wal.IsReplayPending(), "stale commit: nothing stays pending");
}


/* A record was cut off by a crash: a header and only a part of the data. The WAL must not stay stuck at it,
   and a record which is appended afterwards must be delivered */
static void TestTornTail ()
{
    std::string Path = WalPath ("f1torn");
    std::vector<std::string> Records;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "good");

    uint32_t Len = 100;
    AppendBytes (Path, &Len, sizeof (Len));
    AppendBytes (Path, "xx", 2);

    ReplayAll (Wal, Records);
    Check (Contains (Records, "good"), "torn tail: the good record is delivered");
    Check (6 == FileSize (Path + ".corrupt"), "torn tail: the unreadable bytes are kept in the .corrupt file, not thrown away");
    Check (0 == (FileMode (Path + ".corrupt") & 077), "torn tail: the .corrupt file is only accessible for its owner");

    AppendText (Wal, "after");
    ReplayAll (Wal, Records);
    ReplayAll (Wal, Records);

    Check (Contains (Records, "after"), "torn tail: a record appended afterwards is delivered");
    Check (false == Wal.IsReplayPending(), "torn tail: nothing stays pending");
}


/* The crash cut off even the header of a record: fewer than the 4 bytes of the length are there, and nothing else.
   The WAL must not stay pending because of two stray bytes */
static void TestTornHeader ()
{
    std::string Path = WalPath ("f1header");
    std::vector<std::string> Records;

    RemoveFiles (Path);
    AppendBytes (Path, "ab", 2);

    SimpleWAL Wal;
    Wal.Init (Path);

    Check (Wal.IsReplayPending(), "torn header: the stray bytes are pending after the start");
    Check (ReplayAll (Wal, Records) && Records.empty(), "torn header: replay succeeds, nothing is delivered");
    Check (false == Wal.IsReplayPending() && 0 == FileSize (Path), "torn header: the WAL is cleared, it does not stay stuck");
    Check (2 == FileSize (Path + ".corrupt"), "torn header: the two bytes are kept in the .corrupt file");
}


/* A damaged length of 4 GB. Replay must not try to allocate that much (and must not throw).
   The address space of this test is limited for the check, so that a real allocation fails instead of using all memory */
static void TestDamagedLength ()
{
    std::string Path = WalPath ("f1length");
    std::vector<std::string> Records;
    struct rlimit Old;
    struct rlimit Limited;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "good");

    uint32_t Len = 0xFFFFFFFFu;
    AppendBytes (Path, &Len, sizeof (Len));
    AppendBytes (Path, "xxxx", 4);

    ::getrlimit (RLIMIT_AS, &Old);
    Limited = Old;
    Limited.rlim_cur = 1024UL * 1024UL * 1024UL;
    ::setrlimit (RLIMIT_AS, &Limited);

    bool bThrown = false;

    try
    {
        ReplayAll (Wal, Records);
    }
    catch (...)
    {
        bThrown = true;
    }

    ::setrlimit (RLIMIT_AS, &Old);

    Check (false == bThrown, "damaged length: replay does not throw (no allocation of 4 GB)");
    Check (1 == Records.size() && "good" == Records[0], "damaged length: the good record is delivered, nothing from the damaged one");
    Check (false == Wal.IsReplayPending(), "damaged length: the WAL does not stay stuck");
    Check (8 == FileSize (Path + ".corrupt"), "damaged length: the unreadable bytes are kept in the .corrupt file");
}


/* The disk is full in the middle of a record. The failed append must not leave a piece of it in the WAL.
   The limit for the size of a file simulates that. Records before and after must still be readable */
static void TestAppendFailure ()
{
    std::string Path = WalPath ("f2append");
    std::vector<std::string> Records;
    struct rlimit Old;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "before");

    long SizeBefore = FileSize (Path);

    ::signal (SIGXFSZ, SIG_IGN);
    ::getrlimit (RLIMIT_FSIZE, &Old);

    struct rlimit Small = Old;
    Small.rlim_cur = static_cast<rlim_t> (SizeBefore + 10);
    ::setrlimit (RLIMIT_FSIZE, &Small);

    std::string Big (1000, 'y');
    bool bAppended = Wal.Append (Big.data(), static_cast<uint32_t> (Big.size()));

    ::setrlimit (RLIMIT_FSIZE, &Old);

    Check (false == bAppended, "append failure: reported");
    Check (SizeBefore == FileSize (Path), "append failure: no piece of the record is left in the WAL");

    AppendText (Wal, "after");
    ReplayAll (Wal, Records);

    Check (Contains (Records, "before") && Contains (Records, "after"), "append failure: the records before and after it are delivered");
}


/* The WAL contains log lines. Its files must only be accessible for the owner */
static void TestFileModes ()
{
    std::string Path = WalPath ("f2modes");
    int Count = 0;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "one");
    AppendText (Wal, "two");

    Check (0 == (FileMode (Path) & 077), "file modes: the WAL file is only accessible for its owner");

    Wal.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return (Count < 2); });

    Check (FileExists (Path + ".commit") && 0 == (FileMode (Path + ".commit") & 077), "file modes: the commit file is only accessible for its owner");
}


/* Files which an earlier version created with more permissions become private too, when the WAL is opened and when the
   commit file is written again */
static void TestOldFileModes ()
{
    std::string Path = WalPath ("f2oldmodes");
    int Count = 0;

    RemoveFiles (Path);

    {
        SimpleWAL Wal;
        Wal.Init (Path);
        AppendText (Wal, "one");
        AppendText (Wal, "two");
    }

    /* What an earlier version left behind: a WAL and a commit file (position 7, the start of the second record) for everybody */
    WriteCommitFile (Path, 7);
    ::chmod (Path.c_str(), 0644);
    ::chmod ((Path + ".commit").c_str(), 0644);

    SimpleWAL Wal;
    Wal.Init (Path);

    Check (0 == (FileMode (Path) & 077), "old file modes: an existing WAL file becomes private when it is opened");

    AppendText (Wal, "three");
    Wal.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return (Count < 2); });

    Check (0 == (FileMode (Path + ".commit") & 077), "old file modes: the commit file becomes private when it is written again");
}


/* Members must be initialized. The object is created in memory which is filled with 0xAA, so that a member which the
   constructor does not set has a value which is not zero. The log level decides if the WAL writes messages to stderr (for example
   "WAL reset" when it is cleared). A new WAL has to be silent, whatever was in the memory before */
static void TestInitialized ()
{
    std::string Path = WalPath ("f2init");
    alignas (SimpleWAL) static unsigned char Raw[sizeof (SimpleWAL)];
    volatile unsigned char *pFill = Raw;

    RemoveFiles (Path);

    for (size_t i = 0; i < sizeof (Raw); i++)
        pFill[i] = 0xAA;

    SimpleWAL *pWal = new (Raw) SimpleWAL();

    /* The messages have to be able to appear, or the check means nothing: they go to stderr here */
    SimpleWAL::SetDefaultLogTarget (SimpleWAL::LOG_STDERR);

    std::string Output = CaptureStderr ([pWal, &Path] ()
    {
        pWal->Init (Path);
        pWal->Clear();
    });

    SimpleWAL::SetDefaultLogTarget (SimpleWAL::LOG_NONE);

    pWal->~SimpleWAL();

    Check (Output.empty(), "members: a new WAL is silent, its log level does not depend on what was in the memory");
}


/* The commit file has to be read completely. A file which is not one offset is damaged and must not be used.
   The records are then replayed from the start (at worst a record twice, never lost) */
static void TestShortCommitFile ()
{
    std::string Path = WalPath ("f2commit");
    std::vector<std::string> Records;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "one");
    AppendText (Wal, "two");

    /* Two bytes, the value 7. Read as an offset it is exactly the start of the second record (the first one is 4 + 3 bytes), so a
       damaged file looks like a valid position and the first record would be skipped. A longer value would be behind the end of the
       WAL and be caught as a stale position, which is another check */
    const unsigned char Short[2] = { 7, 0 };
    AppendBytes (Path + ".commit", Short, sizeof (Short));

    Check (ReplayAll (Wal, Records), "short commit file: replay succeeds");
    Check (2 == Records.size(), "short commit file: the WAL is replayed from the start");
}


/* A commit file which is empty (the crash came between creating it and writing to it) or too long is damaged too */
static void TestDamagedCommitFile ()
{
    const char *Names[2] = { "f2empty", "f2long" };

    for (int i = 0; i < 2; i++)
    {
        std::string Path = WalPath (Names[i]);
        std::vector<std::string> Records;

        RemoveFiles (Path);

        SimpleWAL Wal;
        Wal.Init (Path);
        AppendText (Wal, "one");
        AppendText (Wal, "two");

        if (0 == i)
        {
            AppendBytes (Path + ".commit", "", 0);
        }
        else
        {
            const unsigned char Long[9] = { 7, 0, 0, 0, 0, 0, 0, 0, 0 };
            AppendBytes (Path + ".commit", Long, sizeof (Long));
        }

        std::string Name = std::string (0 == i ? "empty" : "too long") + " commit file: ";

        Check (ReplayAll (Wal, Records) && 2 == Records.size(), (Name + "the WAL is replayed from the start").c_str());
        Check (false == Wal.IsReplayPending(), (Name + "nothing stays pending").c_str());
    }
}


/* A commit file with the right size (8 bytes) but a position which is inside a record, not at the start of one. It can
   be a damaged file or the file of an older WAL. Replay must not read the middle of a record as a length, and must not skip
   records. It notices that the position is not the start of a record and replays from the start. A valid position is used */
static void TestMisalignedCommit ()
{
    /* one (bytes 0..6), two (7..13), three (14..22). The records start at 0, 7 and 14, and the WAL ends at 23 */
    struct { uint64_t Offset; const char *pszWhere; } Bad[] =
    {
        {  1, "inside the length of the first record" },
        {  4, "at the start of the data of the first record" },
        {  5, "inside the data of the first record" },
        {  6, "one byte before the second record" },
        { 13, "one byte before the third record" },
        { 22, "one byte before the end" }
    };

    std::string Path = WalPath ("r2align");

    for (const auto& Case : Bad)
    {
        std::vector<std::string> Records;

        RemoveFiles (Path);

        {
            SimpleWAL Wal;
            Wal.Init (Path);
            AppendText (Wal, "one");
            AppendText (Wal, "two");
            AppendText (Wal, "three");
        }

        WriteCommitFile (Path, Case.Offset);

        SimpleWAL Wal;
        Wal.Init (Path);
        ReplayAll (Wal, Records);

        std::string Name = "misaligned commit offset " + std::to_string (Case.Offset) + " (" + Case.pszWhere + "): ";

        Check (3 == Records.size() && "one" == Records[0] && "two" == Records[1] && "three" == Records[2], (Name + "the WAL is replayed from the start").c_str());
        Check (false == FileExists (Path + ".corrupt"), (Name + "no valid record is moved to the .corrupt file").c_str());
    }

    /* Positions which are the start of a record are used: the records before are not delivered again */
    struct { uint64_t Offset; size_t Expected; const char *pszFirst; } Good[] = { { 7, 2, "two" }, { 14, 1, "three" } };

    for (const auto& Case : Good)
    {
        std::vector<std::string> Records;

        RemoveFiles (Path);

        {
            SimpleWAL Wal;
            Wal.Init (Path);
            AppendText (Wal, "one");
            AppendText (Wal, "two");
            AppendText (Wal, "three");
        }

        WriteCommitFile (Path, Case.Offset);

        SimpleWAL Wal;
        Wal.Init (Path);
        ReplayAll (Wal, Records);

        std::string Name = "valid commit offset " + std::to_string (Case.Offset) + ": ";

        Check (Case.Expected == Records.size() && Case.pszFirst == Records[0], (Name + "the records before it are not delivered again").c_str());
    }
}


/* The commit position cannot be written (the size limit of a file stands for a full disk). The records were accepted
   by the consumer, but the progress is not saved. Replay must not report progress and the WAL stays pending. After a restart
   the records are delivered again (at least once), none disappear */
static void TestCommitWriteFailure ()
{
    std::string Path = WalPath ("r2commitwrite");
    std::vector<std::string> Records;
    struct rlimit Old;
    int Count = 0;

    RemoveFiles (Path);

    {
        SimpleWAL Wal;
        Wal.Init (Path);
        AppendText (Wal, "one");
        AppendText (Wal, "two");
        AppendText (Wal, "three");

        ::signal (SIGXFSZ, SIG_IGN);
        ::getrlimit (RLIMIT_FSIZE, &Old);

        struct rlimit Small = Old;
        Small.rlim_cur = 4;
        ::setrlimit (RLIMIT_FSIZE, &Small);

        /* The consumer accepts the first two records and refuses the third */
        bool bResult = Wal.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return (Count < 3); });

        ::setrlimit (RLIMIT_FSIZE, &Old);

        Check (false == bResult, "commit write failure: Replay reports the failure, it does not claim progress which was not saved");
        Check (Wal.IsReplayPending(), "commit write failure: the WAL stays pending");
        Check (FileSize (Path) > 0, "commit write failure: the records are still in the WAL");
    }

    SimpleWAL Wal2;
    Wal2.Init (Path);
    ReplayAll (Wal2, Records);

    Check (3 == Records.size() && "one" == Records[0] && "two" == Records[1] && "three" == Records[2],
          "commit write failure: after a restart all records are delivered again (at least once), none disappeared");
}


/* A zero length in the middle of the WAL is not the end of it. Append never writes one, so the file is damaged.
   The zero and everything behind it is kept in the .corrupt file: valid records are not silently discarded */
static void TestZeroLengthHeader ()
{
    std::string Path = WalPath ("r2zero");
    std::vector<std::string> Records;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "one");

    uint32_t Zero = 0;
    AppendBytes (Path, &Zero, sizeof (Zero));

    AppendText (Wal, "two");

    ReplayAll (Wal, Records);

    Check (1 == Records.size() && "one" == Records[0], "zero length header: the record before it is delivered");
    Check (11 == FileSize (Path + ".corrupt"), "zero length header: it is not an end marker. The zero and the record behind it (4 + 4 + 3 bytes) are kept in the .corrupt file");
    Check (false == Wal.IsReplayPending() && 0 == FileSize (Path), "zero length header: the WAL is cleared");
}


/* After a recovery the repaired state is on disk. A new WAL object, as after a restart, starts clean and works */
static void TestReopenAfterRecovery ()
{
    const char *Kinds[3] = { "torn tail", "damaged length", "zero length header" };

    for (int k = 0; k < 3; k++)
    {
        std::string Path = WalPath ((std::string ("r2reopen") + std::to_string (k)).c_str());
        std::vector<std::string> First;
        std::vector<std::string> Second;
        std::string Name = std::string ("reopen after ") + Kinds[k] + ": ";

        RemoveFiles (Path);

        {
            SimpleWAL Wal;
            Wal.Init (Path);
            AppendText (Wal, "good");

            if (0 == k)
            {
                uint32_t Len = 100;
                AppendBytes (Path, &Len, sizeof (Len));
                AppendBytes (Path, "xx", 2);
            }
            else if (1 == k)
            {
                uint32_t Len = 0xFFFFFFFFu;
                AppendBytes (Path, &Len, sizeof (Len));
                AppendBytes (Path, "xxxx", 4);
            }
            else
            {
                uint32_t Zero = 0;
                AppendBytes (Path, &Zero, sizeof (Zero));
                AppendBytes (Path, "xxxx", 4);
            }

            ReplayAll (Wal, First);
        }

        SimpleWAL Wal2;
        Wal2.Init (Path);

        Check (1 == First.size() && "good" == First[0], (Name + "the good record was delivered before the restart").c_str());
        Check (false == Wal2.IsReplayPending() && 0 == FileSize (Path), (Name + "after a restart nothing is pending and the WAL is empty").c_str());

        AppendText (Wal2, "after");
        ReplayAll (Wal2, Second);

        Check (1 == Second.size() && "after" == Second[0], (Name + "the WAL works after the restart, nothing is delivered twice").c_str());
    }
}


/* The option which calls fsync after every write. It cannot prove that data survives a power failure, but it runs the code */
static void TestSyncOption ()
{
    std::string Path = WalPath ("r2sync");
    std::vector<std::string> Records;
    int Count = 0;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.SetSync (true);
    Wal.Init (Path);

    Check (Wal.GetSync(), "sync option: it is on after SetSync(true)");

    AppendText (Wal, "one");
    AppendText (Wal, "two");
    AppendText (Wal, "three");

    Wal.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return (Count < 2); });

    uint64_t Commit = 0;

    Check (ReadCommitFile (Path, Commit) && 7 == Commit, "sync option: the commit position is written after a partial replay");

    ReplayAll (Wal, Records);

    Check (2 == Records.size() && "two" == Records[0] && "three" == Records[1], "sync option: the records which were not replayed are delivered");
    Check (false == Wal.IsReplayPending() && 0 == FileSize (Path), "sync option: the WAL is cleared");
}


/* The crash contract, with real crashes. A child process ends with _exit(), which runs no destructor and no cleanup:
   what it wrote to the WAL has to be there. And a crash inside the replay, after a record was delivered and before the position
   was saved: the record is delivered again, at least once, and none disappears */
static void TestCrash ()
{
    std::string Path = WalPath ("r2crash");
    std::vector<std::string> Records;
    int Status = 0;

    RemoveFiles (Path);

    pid_t Pid = ::fork();

    if (0 == Pid)
    {
        SimpleWAL Wal;
        Wal.Init (Path);
        AppendText (Wal, "survivor 1");
        AppendText (Wal, "survivor 2");
        ::_exit (0);
    }

    ::waitpid (Pid, &Status, 0);

    Check (WIFEXITED (Status) && 0 == WEXITSTATUS (Status), "crash after append: the child ended");

    {
        SimpleWAL Wal;
        Wal.Init (Path);

        Check (Wal.IsReplayPending(), "crash after append: the records are pending after the restart");
        ReplayAll (Wal, Records);
        Check (2 == Records.size() && "survivor 1" == Records[0] && "survivor 2" == Records[1], "crash after append: both records are there");
    }

    /* A crash inside the replay. The child writes every record it accepts to a file, which stands for the receiver */
    std::string Delivered = g_Dir + "/r2delivered.txt";
    std::vector<std::string> Again;

    RemoveFiles (Path);
    ::unlink (Delivered.c_str());

    {
        SimpleWAL Wal;
        Wal.Init (Path);
        AppendText (Wal, "one");
        AppendText (Wal, "two");
        AppendText (Wal, "three");
    }

    Pid = ::fork();

    if (0 == Pid)
    {
        SimpleWAL Wal;
        Wal.Init (Path);

        int Count = 0;

        Wal.Replay ([&Count, &Delivered] (const std::vector<uint8_t>& Record)
        {
            std::string Text (Record.begin(), Record.end());
            Text += "\n";
            AppendBytes (Delivered, Text.data(), Text.size());

            /* The crash: the second record is delivered, but the replay ends before the position is saved */
            if (++Count == 2)
                ::_exit (0);

            return true;
        });

        ::_exit (1);
    }

    ::waitpid (Pid, &Status, 0);

    Check (WIFEXITED (Status) && 0 == WEXITSTATUS (Status), "crash inside the replay: the child ended inside the callback");

    SimpleWAL Wal;
    Wal.Init (Path);
    ReplayAll (Wal, Again);

    Check (Wal.IsReplayPending() == false && 3 == Again.size() && "one" == Again[0] && "two" == Again[1] && "three" == Again[2],
          "crash inside the replay: all records are delivered again after the restart, none disappeared (at least once)");
}


/* The WAL cannot be truncated (fault injection). Replay must not claim that everything is done, the WAL stays pending
   and the records are still there. Later they are delivered again (at least once) */
static void TestTruncateFailure ()
{
    std::string Path = WalPath ("r2truncate");
    std::vector<std::string> First;
    std::vector<std::string> Second;

    RemoveFiles (Path);

    {
        SimpleWAL Wal;
        Wal.Init (Path);
        AppendText (Wal, "one");
        AppendText (Wal, "two");

        g_bFailTruncate = true;
        bool bResult = ReplayAll (Wal, First);
        g_bFailTruncate = false;

        Check (2 == First.size(), "truncate failure: the records were delivered to the consumer");
        Check (false == bResult, "truncate failure: Replay reports the failure, delivering the records is not enough");
        Check (Wal.IsReplayPending() && FileSize (Path) > 0, "truncate failure: the WAL stays pending and keeps the records");
    }

    SimpleWAL Wal2;
    Wal2.Init (Path);
    ReplayAll (Wal2, Second);

    Check (2 == Second.size(), "truncate failure: the records are delivered again later (at least once)");
    Check (false == Wal2.IsReplayPending() && 0 == FileSize (Path), "truncate failure: the WAL is cleared when the failure is gone");
}


/* The commit file cannot be removed (fault injection). If the WAL were truncated anyway, the old position would be left
   behind and does not fit to the next records. The WAL must stay as it is, pending, and report the failure */
static void TestCommitRemovalFailure ()
{
    std::string Path = WalPath ("r2unlink");
    std::vector<std::string> First;
    std::vector<std::string> Second;
    int Count = 0;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "one");
    AppendText (Wal, "two");
    AppendText (Wal, "three");

    /* A commit file exists: the first record is delivered, the second is refused */
    Wal.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return (Count < 2); });

    long SizeBefore = FileSize (Path);

    g_bFailUnlinkCommit = true;
    bool bResult = ReplayAll (Wal, First);
    g_bFailUnlinkCommit = false;

    Check (2 == First.size() && "two" == First[0] && "three" == First[1], "commit removal failure: the records after the position were delivered");
    Check (false == bResult, "commit removal failure: Replay reports the failure");
    Check (Wal.IsReplayPending(), "commit removal failure: the WAL stays pending");
    Check (SizeBefore == FileSize (Path) && FileExists (Path + ".commit"), "commit removal failure: the WAL is not truncated, the old position does not get left behind");

    ReplayAll (Wal, Second);

    Check (2 == Second.size() && false == Wal.IsReplayPending() && 0 == FileSize (Path), "commit removal failure: delivered again (at least once) and cleared when the failure is gone");
}


/* The unreadable part cannot be moved to the .corrupt file (a directory has its name, so it cannot be created).
   The WAL must not be cleared: nothing may be thrown away. What was readable is delivered. Later it is repaired */
static void TestQuarantineFailure ()
{
    std::string Path = WalPath ("r2quarantine");
    std::vector<std::string> First;
    std::vector<std::string> Second;

    RemoveFiles (Path);
    ::rmdir ((Path + ".corrupt").c_str());

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "good");

    uint32_t Len = 100;
    AppendBytes (Path, &Len, sizeof (Len));
    AppendBytes (Path, "xx", 2);

    ::mkdir ((Path + ".corrupt").c_str(), 0700);

    ReplayAll (Wal, First);

    Check (1 == First.size() && "good" == First[0], "quarantine failure: the readable record is delivered");
    Check (Wal.IsReplayPending() && FileSize (Path) == 4 + 4 + 4 + 2, "quarantine failure: the WAL is not cleared, the unreadable bytes are still in it");

    ::rmdir ((Path + ".corrupt").c_str());
    ReplayAll (Wal, Second);

    Check (Second.empty() && false == Wal.IsReplayPending() && 0 == FileSize (Path), "quarantine failure: the WAL is repaired when the failure is gone, the good record is not delivered twice");
    Check (6 == FileSize (Path + ".corrupt"), "quarantine failure: the unreadable bytes are kept in the .corrupt file");

    /* Nothing readable at all: no progress, so Replay reports the failure */
    std::string Path2 = WalPath ("r2quarantine2");
    std::vector<std::string> Third;

    RemoveFiles (Path2);
    AppendBytes (Path2, "ab", 2);

    SimpleWAL Wal2;
    Wal2.Init (Path2);
    ::mkdir ((Path2 + ".corrupt").c_str(), 0700);

    bool bResult = ReplayAll (Wal2, Third);
    ::rmdir ((Path2 + ".corrupt").c_str());

    Check (false == bResult && Wal2.IsReplayPending() && 2 == FileSize (Path2), "quarantine failure: nothing readable and nothing moved: Replay reports the failure and keeps the bytes");
}


/* ============================== Performance ============================== */


/* What kind of file system holds the files. The speed of the WAL depends on it: a RAM file system is much faster than a disk,
   and fsync does nothing there */
static std::string FileSystemName (const std::string& Path)
{
    struct statfs Fs;
    char szOther[64];

    if (0 != ::statfs (Path.c_str(), &Fs))
        return "unknown";

    switch (static_cast<unsigned long> (Fs.f_type))
    {
        case 0x01021994UL: return "tmpfs (memory, not a disk: fsync does nothing, speeds are much higher than on a disk)";
        case 0xEF53UL:     return "ext2/ext3/ext4 (disk)";
        case 0x58465342UL: return "xfs (disk)";
        case 0x9123683EUL: return "btrfs (disk)";
        case 0x01021997UL: return "9p (Windows drive in WSL, slow)";
        case 0x794c7630UL: return "overlayfs";
        case 0x6969UL:     return "nfs (network)";
        case 0x65735546UL: return "fuse";
        default:
            snprintf (szOther, sizeof (szOther), "other (type 0x%lx)", static_cast<unsigned long> (Fs.f_type));
            return szOther;
    }
}


static double NowMs ()
{
    return std::chrono::duration<double, std::milli> (std::chrono::steady_clock::now().time_since_epoch()).count();
}


/* One line of the table. Bytes is what was written to or read from the WAL file, including the 4 bytes length of every record */
static double PerfRow (const char *pszName, size_t Records, double Bytes, double Ms)
{
    double PerSecond = (Ms > 0) ? (Records * 1000.0 / Ms) : 0.0;
    double MbPerSec  = (Ms > 0) ? (Bytes / 1000000.0 * 1000.0 / Ms) : 0.0;

    printf ("%-52s %9zu %10.1f %12.0f %9.1f\n", pszName, Records, Ms, PerSecond, MbPerSec);

    return PerSecond;
}


/* Speed of the WAL: appending, replaying, records of the size of a real push request, the check of the commit position when a
   program starts with a big backlog, and the sync option. The numbers depend on the machine and on its load. The limits of the
   checks are very low on purpose: they only catch something which is dramatically wrong, they do not measure.
   The table is written to the output every time, to compare runs and versions */
static void TestPerformance (size_t Records)
{
    std::string Path      = WalPath ("perf");
    std::string PathBig   = WalPath ("perfbig");
    std::string PathSync  = WalPath ("perfsync");
    std::string Small (100, 'x');
    std::string Big (65536, 'y');
    size_t BigRecords     = (Records / 100 > 100) ? (Records / 100) : 100;
    size_t SyncRecords    = (Records > 2000) ? 2000 : Records;
    size_t Count          = 0;
    size_t AppendFailed   = 0;
    double t0             = 0;

    printf ("Directory:   %s\n", g_Dir.c_str());
    printf ("File system: %s\n\n", FileSystemName (g_Dir).c_str());
    printf ("%-52s %9s %10s %12s %9s\n", "Measurement", "Records", "Time (ms)", "Records/s", "MB/s");

    RemoveFiles (Path);
    RemoveFiles (PathBig);
    RemoveFiles (PathSync);

    /* Small records, like the lines of a log */
    SimpleWAL Wal;
    Wal.Init (Path);

    t0 = NowMs();

    for (size_t i = 0; i < Records; i++)
    {
        if (false == Wal.Append (Small.data(), static_cast<uint32_t> (Small.size())))
            AppendFailed++;
    }

    double AppendRate = PerfRow ("append, 100 byte records, one thread", Records, Records * 104.0, NowMs() - t0);

    t0 = NowMs();
    Wal.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return true; });
    double ReplayRate = PerfRow ("replay everything, 100 byte records", Records, Records * 104.0, NowMs() - t0);

    bool bSmallOk = (0 == AppendFailed && Count == Records);

    /* Records of the size of a push request with 100 log lines */
    SimpleWAL WalBig;
    WalBig.Init (PathBig);
    AppendFailed = 0;
    Count        = 0;

    t0 = NowMs();

    for (size_t i = 0; i < BigRecords; i++)
    {
        if (false == WalBig.Append (Big.data(), static_cast<uint32_t> (Big.size())))
            AppendFailed++;
    }

    double BigAppendRate = PerfRow ("append, 64 KB records (a push request)", BigRecords, BigRecords * 65540.0, NowMs() - t0);

    t0 = NowMs();
    WalBig.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return true; });
    double BigReplayRate = PerfRow ("replay everything, 64 KB records", BigRecords, BigRecords * 65540.0, NowMs() - t0);

    bool bBigOk = (0 == AppendFailed && Count == BigRecords);

    /* A program which starts with a backlog: the commit position is in the middle. Before the first replay, the position is checked
       against the records of the WAL. The consumer refuses the first record, so the time is the check and not the replay */
    {
        SimpleWAL Filler;
        Filler.Init (Path);

        for (size_t i = 0; i < Records; i++)
            Filler.Append (Small.data(), static_cast<uint32_t> (Small.size()));
    }

    size_t Half = Records / 2;
    WriteCommitFile (Path, static_cast<uint64_t> (Half) * 104);

    SimpleWAL Restarted;
    Restarted.Init (Path);

    t0 = NowMs();
    Restarted.Replay ([] (const std::vector<uint8_t>&) { return false; });
    double CheckRate = PerfRow ("start with a backlog: check of the commit position", Half, Half * 104.0, NowMs() - t0);

    /* The sync option: every record is written to the disk before Append returns. It depends on the disk, so there is no limit */
    SimpleWAL WalSync;
    WalSync.SetSync (true);
    WalSync.Init (PathSync);

    t0 = NowMs();

    for (size_t i = 0; i < SyncRecords; i++)
        WalSync.Append (Small.data(), static_cast<uint32_t> (Small.size()));

    PerfRow ("append with fsync, 100 byte records (no limit)", SyncRecords, SyncRecords * 104.0, NowMs() - t0);

    /* The checks come after the table. The limits are very low: only something which is dramatically wrong fails */
    printf ("\n");
    Check (bSmallOk, "performance: all small records were appended and replayed");
    Check (bBigOk, "performance: all large records were appended and replayed");
    Check (AppendRate >= 20000, "performance: append of small records is faster than 20000 records/s");
    Check (ReplayRate >= 20000, "performance: replay of small records is faster than 20000 records/s");
    Check (BigAppendRate >= 200, "performance: append of 64 KB records is faster than 200 records/s (13 MB/s)");
    Check (BigReplayRate >= 200, "performance: replay of 64 KB records is faster than 200 records/s (13 MB/s)");
    Check (CheckRate >= 50000, "performance: the check of the commit position at start walks more than 50000 records/s");
}


/* ============================== Peek and Ack ============================== */

static std::string AsText (const std::vector<uint8_t>& Record)
{
    return std::string (Record.begin(), Record.end());
}


/* Reads the oldest record with Peek. True if there is one and it is this one */
static bool PeekIs (SimpleWAL& Wal, const char *pszExpected)
{
    std::vector<uint8_t> Record;

    return Wal.Peek (Record) && (AsText (Record) == pszExpected);
}


/* One record at a time, without a function of the application: the oldest record is read, and it stays in the WAL until it is
   acknowledged */
static void TestPeekAckBasics ()
{
    std::string Path = WalPath ("peek1");
    std::vector<uint8_t> Record;
    uint64_t Commit = 0;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);

    Check (false == Wal.Peek (Record), "peek: an empty WAL has nothing to read");
    Check (false == Wal.Ack(), "peek: there is nothing to acknowledge before a record was read");

    AppendText (Wal, "first");
    AppendText (Wal, "second");
    AppendText (Wal, "third");

    Check (PeekIs (Wal, "first"), "peek: the oldest record is read first");
    Check (PeekIs (Wal, "first"), "peek: without an ack the same record is read again");
    Check (Wal.Ack(), "peek: the record is acknowledged");
    Check (false == Wal.Ack(), "peek: a record can only be acknowledged once");
    Check (ReadCommitFile (Path, Commit) && (4 + 5 == Commit), "peek: the position behind the acknowledged record is in the commit file");
    Check (PeekIs (Wal, "second"), "peek: after an ack the next record is read");
    Check (Wal.Ack(), "peek: the second record is acknowledged");
    Check (PeekIs (Wal, "third"), "peek: the third record is read");
    Check (Wal.IsReplayPending(), "peek: the last record is pending until it is acknowledged");
    Check (Wal.Ack(), "peek: the last record is acknowledged");
    Check ((false == Wal.IsReplayPending()) && (0 == FileSize (Path)) && (false == FileExists (Path + ".commit")), "peek: after the last ack the WAL is empty and there is no commit file");
    Check (false == Wal.Peek (Record), "peek: nothing is left");
    Check (Record.empty(), "peek: no record is returned when there is none");
}


/* After a restart: what was acknowledged is not delivered again, what was read without an ack is (at least once) */
static void TestPeekAckRestart ()
{
    std::string Path = WalPath ("peek2");

    RemoveFiles (Path);

    {
        SimpleWAL Wal;

        Wal.Init (Path);
        AppendText (Wal, "first");
        AppendText (Wal, "second");
        AppendText (Wal, "third");

        Check (PeekIs (Wal, "first"), "peek restart: the first record is read");
        Check (Wal.Ack(), "peek restart: and acknowledged");
        Check (PeekIs (Wal, "second"), "peek restart: the second record is read, and the program ends without an ack");
    }

    SimpleWAL Wal2;
    Wal2.Init (Path);

    Check (PeekIs (Wal2, "second"), "peek restart: after a restart the record without an ack is delivered again, the acknowledged one is not");
    Check (Wal2.Ack(), "peek restart: acknowledged now");
    Check (PeekIs (Wal2, "third") && Wal2.Ack(), "peek restart: the third record follows");
    Check (false == Wal2.IsReplayPending(), "peek restart: nothing stays pending");
}


/* A record which is appended while another one is read: it is not lost, and the WAL is not emptied by the ack of the first one */
static void TestPeekAckAppend ()
{
    std::string Path = WalPath ("peek3");

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);

    AppendText (Wal, "old");
    Check (PeekIs (Wal, "old"), "peek append: the record is read");

    AppendText (Wal, "new");

    Check (Wal.Ack(), "peek append: the ack works while a record was appended");
    Check (Wal.IsReplayPending() && (0 != FileSize (Path)), "peek append: the appended record keeps the WAL: it is not emptied");
    Check (PeekIs (Wal, "new") && Wal.Ack(), "peek append: the appended record is delivered");
    Check (false == Wal.IsReplayPending(), "peek append: then the WAL is empty");
}


/* Another thread empties the WAL between the read and the ack: the ack must not acknowledge a record of the new WAL */
static void TestPeekAckClear ()
{
    std::string Path = WalPath ("peek4");

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);

    AppendText (Wal, "old");
    Check (PeekIs (Wal, "old"), "peek clear: the record is read");

    Wal.Clear();
    AppendText (Wal, "new");

    Check (false == Wal.Ack(), "peek clear: the ack of a record of the WAL which was emptied does nothing");
    Check (PeekIs (Wal, "new"), "peek clear: the record which was appended after it is not lost");
}


/* A replay which ran between the read and the ack has moved the position: the ack does nothing. And no read during a replay */
static void TestPeekAckReplay ()
{
    std::string Path = WalPath ("peek5");
    std::vector<std::string> Records;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);

    AppendText (Wal, "one");
    AppendText (Wal, "two");

    Check (PeekIs (Wal, "one"), "peek replay: the record is read");
    ReplayAll (Wal, Records);
    Check (2 == Records.size(), "peek replay: a replay delivers both records");
    Check (false == Wal.Ack(), "peek replay: the ack of a record which the replay delivered does nothing");

    AppendText (Wal, "three");

    bool bPeekDuring = true;
    std::vector<uint8_t> Record;

    Wal.Replay ([&] (const std::vector<uint8_t>&) { bPeekDuring = Wal.Peek (Record); return true; });

    Check (false == bPeekDuring, "peek replay: no read while a replay runs");

    /* A replay which stops half way saves the position: the record which was read before is not the one at the position any more */
    std::string PathPartial = WalPath ("peek5b");
    std::vector<std::string> Delivered;

    RemoveFiles (PathPartial);

    SimpleWAL WalPartial;
    WalPartial.Init (PathPartial);

    AppendText (WalPartial, "a");
    AppendText (WalPartial, "b");
    AppendText (WalPartial, "c");

    Check (PeekIs (WalPartial, "a"), "peek replay: the first record is read");

    WalPartial.Replay ([&Delivered] (const std::vector<uint8_t>& Rec)
    {
        Delivered.push_back (std::string (Rec.begin(), Rec.end()));
        return (1 == Delivered.size());             // accepts the first record, refuses the second
    });

    Check (false == WalPartial.Ack(), "peek replay: the ack after a replay which saved the position does nothing");
    Check (PeekIs (WalPartial, "b"), "peek replay: the next read is the record after what the replay accepted");
}


/* The same repairs as in a replay: a record which was cut off, and a position which is not right */
static void TestPeekAckDamaged ()
{
    std::string Path = WalPath ("peek6");
    std::vector<uint8_t> Record;

    RemoveFiles (Path);

    {
        SimpleWAL Wal;

        Wal.Init (Path);
        AppendText (Wal, "good");

        uint32_t Len = 100;
        AppendBytes (Path, &Len, sizeof (Len));
        AppendBytes (Path, "xx", 2);

        Check (PeekIs (Wal, "good") && Wal.Ack(), "peek damaged: the good record in front of a record which was cut off is delivered");
        Check (false == Wal.Peek (Record), "peek damaged: then there is nothing more to read");
        Check (6 == FileSize (Path + ".corrupt"), "peek damaged: the bytes which were cut off are kept in the .corrupt file");
        Check ((false == Wal.IsReplayPending()) && (0 == FileSize (Path)), "peek damaged: the WAL is empty afterwards, it does not stay stuck");
    }

    /* The position of a WAL which was emptied earlier is behind the end: read from the start */
    std::string PathStale = WalPath ("peek7");

    RemoveFiles (PathStale);
    WriteCommitFile (PathStale, 4096);

    SimpleWAL WalStale;
    WalStale.Init (PathStale);
    AppendText (WalStale, "new record");

    Check (PeekIs (WalStale, "new record") && WalStale.Ack(), "peek damaged: a commit position behind the end is not used, the record is read");
    Check (false == WalStale.IsReplayPending(), "peek damaged: nothing stays pending");
}


/* Producers append while one consumer reads and acknowledges. Every record arrives, in the order of its producer, once */
static void TestPeekAckThreads ()
{
    std::string Path = WalPath ("peek8");
    const int Producers   = 3;
    const int PerProducer = 1000;
    const int Total       = Producers * PerProducer;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);

    std::vector<std::thread> Threads;

    for (int p = 0; p < Producers; p++)
    {
        Threads.emplace_back ([&Wal, p]
        {
            for (int i = 0; i < PerProducer; i++)
            {
                std::string Text = "p" + std::to_string (p) + "-" + std::to_string (i);

                Wal.Append (Text.data(), static_cast<uint32_t> (Text.size()));
            }
        });
    }

    std::vector<int> Next (static_cast<size_t> (Producers), 0);
    bool bInOrder = true;
    int  Delivered = 0;
    auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds (30);

    while ( (Delivered < Total) && (std::chrono::steady_clock::now() < Deadline) )
    {
        std::vector<uint8_t> Record;

        if (false == Wal.Peek (Record))
        {
            std::this_thread::sleep_for (std::chrono::milliseconds (1));
            continue;
        }

        int p = -1;
        int i = -1;

        if ( (2 != sscanf (AsText (Record).c_str(), "p%d-%d", &p, &i)) || (p < 0) || (p >= Producers) || (i != Next[static_cast<size_t> (p)]) )
            bInOrder = false;
        else
            Next[static_cast<size_t> (p)] = i + 1;

        if (Wal.Ack())
            Delivered++;
    }

    for (std::thread& Thread : Threads)
        Thread.join();

    Check (Total == Delivered, "[thread] peek: producers append while a consumer reads and acknowledges: every record arrives");
    Check (bInOrder, "[thread] peek: the records of every producer arrive in their order, each once");
    Check (false == Wal.IsReplayPending(), "[thread] peek: nothing stays pending at the end");
}


/* ============================== Size limit ============================== */

/* SetMaxSize: a record which would make the file larger than the limit is not stored. Nothing of it is left in the file */
static void TestSizeLimit ()
{
    std::string Path = WalPath ("limit");
    std::vector<std::string> Messages;
    std::vector<std::string> Records;
    const char *pszRecord = "0123456789";          // 10 bytes and the length of 4 bytes: 14 bytes in the file

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.SetLogFunction ([&Messages] (const char *pszMessage) { Messages.push_back (pszMessage); });

    Check (0 == Wal.GetSize(), "size limit: a WAL which is not opened has the size 0");

    Wal.Init (Path);

    Check (0 == Wal.GetSize(), "size limit: an empty WAL has the size 0");

    Wal.SetMaxSize (100);

    int Accepted = 0;

    while ( (Accepted < 1000) && Wal.Append (pszRecord, 10) )
        Accepted++;

    Check (7 == Accepted, "size limit: 7 records of 14 bytes fit into 100 bytes");
    Check ((98 == Wal.GetSize()) && (98 == FileSize (Path)), "size limit: the file is not larger than the limit, and nothing of the refused record is in it");
    Check (false == Wal.Append (pszRecord, 10), "size limit: the next record is refused too");
    Check (false == Wal.Append (std::string (200, 'x').data(), 200), "size limit: a record which is larger than the whole limit never fits");
    Check ((1 == Messages.size()) && StartsWith (Messages[0], "[Warning] WAL: ") && (std::string::npos != Messages[0].find ("is full")),
           "size limit: any number of refused records makes one message, a warning");

    ReplayAll (Wal, Records);

    Check ((7 == Records.size()) && ("0123456789" == Records[0]) && ("0123456789" == Records[6]), "size limit: the records which were stored are delivered, all 7");
    Check (0 == Wal.GetSize(), "size limit: after the replay the file is empty again");
    Check (Wal.Append (pszRecord, 10), "size limit: there is room again");

    while ( (Accepted < 2000) && Wal.Append (pszRecord, 10) )
        Accepted++;

    Check (2 == Messages.size(), "size limit: when the WAL is full again the message is written again");

    Wal.SetMaxSize (0);

    Check (Wal.Append (pszRecord, 10), "size limit: the limit 0 is no limit");

    /* The exact limit: 7 records are 98 bytes. A limit of 98 lets the 7th one make the file exactly as large as the limit, a limit of
       97 does not */
    for (int Case = 0; Case < 2; Case++)
    {
        uint64_t Limit = (0 == Case) ? 98 : 97;
        std::string PathExact = WalPath ((0 == Case) ? "limit98" : "limit97");

        RemoveFiles (PathExact);

        SimpleWAL WalExact;
        WalExact.Init (PathExact);
        WalExact.SetMaxSize (Limit);

        int Fit = 0;

        while ( (Fit < 100) && WalExact.Append (pszRecord, 10) )
            Fit++;

        Check (Fit == ((0 == Case) ? 7 : 6),
               (0 == Case) ? "size limit: a record which makes the file exactly as large as the limit fits"
                           : "size limit: a record which makes the file one byte larger than the limit does not fit");
    }
}


/* ============================== Descriptors ============================== */

/* The numbers of the open file descriptors of this process which are open on a file with this path */
static std::vector<int> FindOpenFds (const std::string& Path)
{
    std::vector<int> Fds;
    DIR *pDir = ::opendir ("/proc/self/fd");

    if (NULL == pDir)
        return Fds;

    struct dirent *pEntry = NULL;

    while (NULL != (pEntry = ::readdir (pDir)))
    {
        char szLink[512] = {0};
        std::string Name = std::string ("/proc/self/fd/") + pEntry->d_name;
        ssize_t Len = ::readlink (Name.c_str(), szLink, sizeof (szLink) - 1);

        if ( (Len > 0) && (Path == std::string (szLink, static_cast<size_t> (Len))) )
            Fds.push_back (atoi (pEntry->d_name));
    }

    ::closedir (pDir);

    return Fds;
}


static bool AllCloseOnExec (const std::vector<int>& Fds)
{
    for (int Fd : Fds)
    {
        int Flags = ::fcntl (Fd, F_GETFD);

        if ( (Flags < 0) || (0 == (Flags & FD_CLOEXEC)) )
            return false;
    }

    return true;
}


/* A program which starts other programs must not hand them the files of the WAL. A child which is started later would keep the lock
   on the file after the program is gone: the next start could not open the WAL. So every file which the WAL opens is closed when
   another program is started */
static void TestCloseOnExec ()
{
    std::string Path = WalPath ("cloexec");
    size_t OpenDuringReplay = 0;
    bool   bFlagDuringReplay = false;

    RemoveFiles (Path);

    SimpleWAL Wal;

    Wal.Init (Path);

    std::vector<int> Opened = FindOpenFds (Path);

    Check (1 == Opened.size(), "close on exec: the WAL file is open, once");
    Check (false == Opened.empty() && AllCloseOnExec (Opened), "close on exec: the file which the WAL appends to is closed when another program is started");

    /* During a replay the WAL reads through a second descriptor */
    AppendText (Wal, "record");

    /* The flag is read inside the function of the replay: the descriptor for reading is closed when the replay ends */
    Wal.Replay ([&] (const std::vector<uint8_t>&)
    {
        std::vector<int> Fds = FindOpenFds (Path);

        OpenDuringReplay  = Fds.size();
        bFlagDuringReplay = (false == Fds.empty()) && AllCloseOnExec (Fds);

        return true;
    });

    Check (2 == OpenDuringReplay, "close on exec: during a replay the WAL file is open twice: to append and to read");
    Check (bFlagDuringReplay, "close on exec: the file which the replay reads is closed when another program is started");
}


/* ============================== Threads ============================== */

/* One WAL is used by several threads: this is what the WAL has to allow. A test which would deadlock lets its thread and the
   WAL object live on (the object is never deleted): a thread which waits for a mutex of a destroyed object is undefined behaviour.
   The mistakes which are found with a sanitizer, not with a check, are found with "make tsan" */

/* Waits up to Seconds for a flag. Returns the flag */
static bool WaitFor (const std::atomic<bool>& bFlag, int Seconds)
{
    for (int i = 0; (i < Seconds * 100) && (false == bFlag.load()); i++)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    return bFlag.load();
}


/* The function which is called for every record of a replay may use the WAL. An application puts a record back which it could
   not send, or writes a message. A mutex which is held during the call is a deadlock */
struct ThreadTestState
{
    SimpleWAL         Wal;
    std::atomic<bool> bStarted {false};
    std::atomic<bool> bDone {false};
    std::atomic<bool> bAppended {false};
    std::atomic<int>  Calls {0};
    std::vector<std::string> Delivered;      // what the function of the replay received. Only read after the thread is done
};


static void TestThreadCallbackCallsWal ()
{
    std::string Path = WalPath ("thcallback");

    RemoveFiles (Path);

    /* On the heap, and not deleted if the thread does not return: it must not use variables of a function which has returned */
    ThreadTestState *pState = new ThreadTestState;

    pState->Wal.Init (Path);
    AppendText (pState->Wal, "first");

    /* The function appends once, on its first call. A record which is appended during a replay is part of it, so the replay
       delivers it too: a function which appends on every call would never end */
    std::thread Worker ([pState]
    {
        pState->Wal.Replay ([pState] (const std::vector<uint8_t>& Record)
        {
            pState->Delivered.push_back (std::string (Record.begin(), Record.end()));

            if (1 == ++pState->Calls)
                pState->bAppended = pState->Wal.Append ("again", 5);

            return true;
        });

        pState->bDone = true;
    });

    bool bReturned = WaitFor (pState->bDone, 3);

    Check (bReturned, "[thread] the function of a replay can call the WAL: no deadlock");

    if (false == bReturned)
    {
        Worker.detach();
        return;
    }

    Worker.join();

    Check (pState->bAppended.load(), "[thread] the function of a replay can append a record");
    Check (Contains (pState->Delivered, "again"), "[thread] the record which the function of a replay appended is delivered by the same replay");

    std::vector<std::string> Records;

    ReplayAll (pState->Wal, Records);
    Check (Records.empty(), "[thread] the record which the function of a replay appended is not delivered a second time");
    Check (false == pState->Wal.IsReplayPending(), "[thread] the function of a replay can call the WAL: nothing stays pending");

    delete pState;
}


/* A receiver which is slow makes the function of a replay slow. Another thread which appends must not wait for it (in otelfwd
   this thread pushes the log lines, and it would stop). And a record which was appended during a replay must not be lost when the
   replay ends: the WAL is only emptied if nothing was appended */
static void TestThreadAppendDuringReplay ()
{
    std::string Path = WalPath ("thslow");

    RemoveFiles (Path);

    ThreadTestState *pState = new ThreadTestState;
    SimpleWAL *pWal = &pState->Wal;
    long long Waited = -1;

    pWal->Init (Path);

    for (int i = 0; i < 3; i++)
        AppendText (*pWal, "old");

    std::thread Replayer ([pState]
    {
        pState->Wal.Replay ([pState] (const std::vector<uint8_t>& Record)
        {
            pState->Delivered.push_back (std::string (Record.begin(), Record.end()));
            pState->bStarted = true;
            std::this_thread::sleep_for (std::chrono::milliseconds (300));
            return true;
        });

        pState->bDone = true;
    });

    if (false == WaitFor (pState->bStarted, 3))
    {
        Check (false, "[thread] append during a replay: the replay did not start");
        Replayer.detach();
        return;
    }

    auto Start = std::chrono::steady_clock::now();
    bool bAppended = pWal->Append ("during", 6);

    Waited = std::chrono::duration_cast<std::chrono::milliseconds> (std::chrono::steady_clock::now() - Start).count();

    Replayer.join();

    Check (bAppended, "[thread] append during a replay: the record is accepted");
    Check (Waited < 150, "[thread] append during a replay: it does not wait for the slow function of the replay");

    if (Waited >= 150)
        printf ("        the append waited %lld ms while 3 records of 300 ms were replayed\n", Waited);

    /* Whatever the replay did with the record: it is delivered by this or the next replay, and nothing is lost. A record which is
       appended during a replay is part of it, so it is expected in the list of the replay itself */
    std::vector<std::string> Records = pState->Delivered;

    for (int i = 0; i < 3; i++)
        ReplayAll (*pWal, Records);

    int Count = 0;

    for (const std::string& Record : Records)
    {
        if ("during" == Record)
            Count++;
    }

    Check (Count >= 1, "[thread] append during a replay: the record is not lost when the replay ends");
    Check (1 == Count, "[thread] append during a replay: the record is delivered once, not twice");
    Check (false == pWal->IsReplayPending(), "[thread] append during a replay: nothing stays pending afterwards");

    delete pState;
}


/* Settings are changed while other threads append and log. Nothing may be lost, and a sanitizer must not find a data race:
   "make tsan" runs this test with ThreadSanitizer. Without it this check can only see that the records arrive */
static void TestThreadSettings ()
{
    std::string Path = WalPath ("thsettings");
    const int Count = 2000;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);

    /* The messages go to stderr when the level is set: not into the output of the test */
    std::string Output = CaptureStderr ([&]
    {
        std::thread Appender ([&] { for (int i = 0; i < Count; i++) Wal.Append ("record", 6); });
        std::thread Syncer ([&] { for (int i = 0; i < Count; i++) Wal.SetSync ((i & 1) != 0); });
        std::thread Logger ([&] { for (int i = 0; i < Count; i++) Wal.LogMessage ("message"); });
        std::thread Leveler ([&] { for (int i = 0; i < Count; i++) Wal.SetLogLevel (static_cast<size_t> (i & 1)); });

        Appender.join();
        Syncer.join();
        Logger.join();
        Leveler.join();
    });

    std::vector<std::string> Records;

    /* A report of ThreadSanitizer also goes to stderr: it must not disappear in the captured text */
    if (std::string::npos != Output.find ("ThreadSanitizer"))
        fputs (Output.c_str(), stderr);

    /* The last level which the other thread set can be 1: no message into the output of the test */
    Wal.SetLogLevel (0);

    ReplayAll (Wal, Records);
    Check (static_cast<int> (Records.size()) == Count, "[thread] settings changed while other threads append and log: every record is delivered");
}


/* Several threads replay and append at the same time: every record is delivered at least once and none is lost. Only one thread
   replays at a time, the others wait or get false, and a record is never delivered to two of them at the same moment */
static void TestThreadManyConsumers ()
{
    std::string Path = WalPath ("thmany");
    const int Producers = 3;
    const int PerProducer = 1000;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);

    std::atomic<int> Delivered {0};
    std::atomic<int> Active {0};
    std::atomic<int> MaxActive {0};
    std::atomic<bool> bStop {false};
    std::vector<std::thread> Threads;

    for (int p = 0; p < Producers; p++)
        Threads.emplace_back ([&] { for (int i = 0; i < PerProducer; i++) Wal.Append ("record", 6); });

    for (int c = 0; c < 2; c++)
    {
        Threads.emplace_back ([&]
        {
            while (false == bStop.load())
            {
                Wal.Replay ([&] (const std::vector<uint8_t>&)
                {
                    int Now = ++Active;
                    int Max = MaxActive.load();

                    while ( (Now > Max) && (false == MaxActive.compare_exchange_weak (Max, Now)) )
                    {
                    }

                    Delivered++;
                    --Active;
                    return true;
                });

                std::this_thread::sleep_for (std::chrono::milliseconds (1));
            }
        });
    }

    for (int p = 0; p < Producers; p++)
        Threads[static_cast<size_t> (p)].join();

    std::vector<std::string> Rest;

    for (int i = 0; (i < 200) && Wal.IsReplayPending(); i++)
        std::this_thread::sleep_for (std::chrono::milliseconds (10));

    bStop = true;

    for (size_t t = static_cast<size_t> (Producers); t < Threads.size(); t++)
        Threads[t].join();

    ReplayAll (Wal, Rest);

    Check (Delivered.load() + static_cast<int> (Rest.size()) >= Producers * PerProducer, "[thread] appends and two replaying threads: no record is lost");
    Check (MaxActive.load() <= 1, "[thread] appends and two replaying threads: only one function of a replay is active at a time");
}


/* ============================== Messages of the WAL ============================== */

/* A WAL with a good record and a record which was cut off by a crash: the replay moves the unreadable bytes to the .corrupt file
   and writes an error message */
static void ReplayTornWal (SimpleWAL& Wal, const char *pszName)
{
    std::string Path = WalPath (pszName);
    std::vector<std::string> Records;

    RemoveFiles (Path);
    Wal.Init (Path);
    AppendText (Wal, "good");

    uint32_t Len = 100;
    AppendBytes (Path, &Len, sizeof (Len));
    AppendBytes (Path, "xx", 2);

    ReplayAll (Wal, Records);
}


/* The function of the application gets every message as one string. It replaces the output */
static void TestMessagesFunction ()
{
    std::vector<std::string> Messages;
    std::string Out;
    std::string Err;
    SimpleWAL Wal;

    Wal.SetLogFunction ([&Messages] (const char *pszMessage) { Messages.push_back (pszMessage); });

    Err = CaptureStderr ([&] { Out = CaptureStdout ([&] { ReplayTornWal (Wal, "msgfunc1"); }); });

    Check (1 == Messages.size(), "messages: one problem is one message");
    Check (Messages.size() >= 1 && StartsWith (Messages[0], "[Error] WAL: unreadable data at offset "), "messages: an error has the level and the text of the problem");
    Check (Messages.size() >= 1 && (std::string::npos != Messages[0].find (".corrupt")) && ('\n' != Messages[0].back()), "messages: the text names the .corrupt file, and has no new line at the end");
    Check (Out.empty() && Err.empty(), "messages: with a function nothing is written to stdout or stderr");

    /* A warning: a commit position of an older WAL, behind the end of this one */
    std::string PathStale = WalPath ("msgfunc2");
    std::vector<std::string> Records;
    SimpleWAL WalStale;

    Messages.clear();
    WalStale.SetLogFunction ([&Messages] (const char *pszMessage) { Messages.push_back (pszMessage); });

    RemoveFiles (PathStale);
    WriteCommitFile (PathStale, 4096);
    WalStale.Init (PathStale);
    AppendText (WalStale, "new record");
    ReplayAll (WalStale, Records);

    Check (1 == Messages.size(), "messages: a warning is one message");
    Check (Messages.size() >= 1 && StartsWith (Messages[0], "[Warning] WAL: commit offset 4096 is behind the end of the WAL ("), "messages: a warning has the level [Warning]");

    /* Information: only with a log level above 0, and without a level in the text */
    Messages.clear();
    ReplayTornWal (Wal, "msgfunc3");
    Check (false == Contains (Messages, "WAL reset"), "messages: information is not written with the log level 0");

    Wal.SetLogLevel (1);
    Messages.clear();
    ReplayTornWal (Wal, "msgfunc4");
    Check (Contains (Messages, "WAL reset"), "messages: information is written with the log level 1, as it is, without a level");

    /* An empty function: back to the target */
    Wal.SetLogLevel (0);
    Wal.SetLogFunction (SimpleWAL::LogFunction());
    Wal.SetLogTarget (SimpleWAL::LOG_STDERR);
    Messages.clear();
    Err = CaptureStderr ([&] { ReplayTornWal (Wal, "msgfunc5"); });

    Check (Messages.empty() && (std::string::npos != Err.find ("[Error] WAL: unreadable data at offset ")), "messages: an empty function is the same as none: the message goes to the target");
}


/* Without a function the messages are written to stdout (printf). A target changes that without a function */
static void TestMessagesTargets ()
{
    std::string Out;
    std::string Err;

    /* The default of a new program: stdout */
    SimpleWAL::SetDefaultLogTarget (SimpleWAL::LOG_STDOUT);

    {
        SimpleWAL Wal;

        Err = CaptureStderr ([&] { Out = CaptureStdout ([&] { ReplayTornWal (Wal, "msgtarget1"); }); });
    }

    Check (StartsWith (Out, "[Error] WAL: unreadable data at offset ") && ('\n' == Out.back()), "targets: by default the message is a line on stdout");
    Check (Err.empty(), "targets: by default nothing is written to stderr");

    /* One object to stderr, without a function */
    {
        SimpleWAL Wal;

        Wal.SetLogTarget (SimpleWAL::LOG_STDERR);
        Err = CaptureStderr ([&] { Out = CaptureStdout ([&] { ReplayTornWal (Wal, "msgtarget2"); }); });
    }

    Check (StartsWith (Err, "[Error] WAL: unreadable data at offset ") && Out.empty(), "targets: SetLogTarget (LOG_STDERR) writes to stderr and not to stdout");

    /* One object silent */
    {
        SimpleWAL Wal;

        Wal.SetLogTarget (SimpleWAL::LOG_NONE);
        Err = CaptureStderr ([&] { Out = CaptureStdout ([&] { ReplayTornWal (Wal, "msgtarget3"); }); });
    }

    Check (Out.empty() && Err.empty(), "targets: SetLogTarget (LOG_NONE) writes nothing");

    /* All objects to stderr: the default changes, an object with a target of its own keeps it */
    SimpleWAL::SetDefaultLogTarget (SimpleWAL::LOG_STDERR);

    {
        SimpleWAL Wal;
        SimpleWAL WalOwn;

        WalOwn.SetLogTarget (SimpleWAL::LOG_STDOUT);

        Err = CaptureStderr ([&] { ReplayTornWal (Wal, "msgtarget4"); });
        Check (StartsWith (Err, "[Error] WAL: unreadable data at offset "), "targets: SetDefaultLogTarget (LOG_STDERR) changes the target of objects which have none");

        Err = CaptureStderr ([&] { Out = CaptureStdout ([&] { ReplayTornWal (WalOwn, "msgtarget5"); }); });
        Check (StartsWith (Out, "[Error] WAL: unreadable data at offset ") && Err.empty(), "targets: an object with a target of its own does not follow the default");

        WalOwn.SetLogTarget (SimpleWAL::LOG_DEFAULT);
        Err = CaptureStderr ([&] { ReplayTornWal (WalOwn, "msgtarget6"); });
        Check (StartsWith (Err, "[Error] WAL: unreadable data at offset "), "targets: LOG_DEFAULT follows the default again");
    }

    /* The test is quiet again */
    SimpleWAL::SetDefaultLogTarget (SimpleWAL::LOG_NONE);
}


/* The messages are given out after the mutex is released: the function can call the WAL. A message which is written while the mutex is
   held would be a deadlock here */
static void TestMessagesFunctionCallsWal ()
{
    ThreadTestState *pState = new ThreadTestState;

    pState->Wal.SetLogFunction ([pState] (const char *)
    {
        pState->Calls++;
        pState->bAppended = pState->Wal.Append ("from the log function", 21);
        pState->Wal.IsReplayPending();
    });

    std::thread Worker ([pState]
    {
        ReplayTornWal (pState->Wal, "msgcalls");
        pState->bDone = true;
    });

    bool bReturned = WaitFor (pState->bDone, 3);

    Check (bReturned, "[thread] messages: the log function can call the WAL: no deadlock");

    if (false == bReturned)
    {
        Worker.detach();
        return;
    }

    Worker.join();

    std::vector<std::string> Records;

    ReplayAll (pState->Wal, Records);

    Check ((pState->Calls.load() >= 1) && pState->bAppended.load(), "[thread] messages: the log function was called, and could append a record");
    Check (Contains (Records, "from the log function"), "[thread] messages: the record of the log function is in the WAL");

    delete pState;
}


/* Only one WAL object can use a file. This is checked when it is opened */
static void TestLifecycle ()
{
    std::string Path = WalPath ("lifecycle");
    std::vector<std::string> Messages;
    std::vector<std::string> Records;

    RemoveFiles (Path);

    SimpleWAL *pFirst = new SimpleWAL;

    Check (pFirst->Init (Path), "lifecycle: the first object opens the WAL");

    {
        SimpleWAL Second;

        Second.SetLogFunction ([&Messages] (const char *pszMessage) { Messages.push_back (pszMessage); });

        Check (false == Second.Init (Path), "lifecycle: a second object cannot open a WAL which is in use");
        Check (1 == Messages.size() && StartsWith (Messages[0], "[Error] WAL: ") && (std::string::npos != Messages[0].find ("is in use")), "lifecycle: the message says that the WAL is in use");
        Check (false == Second.Append ("x", 1), "lifecycle: an object which could not open the WAL does not take records");
    }

    AppendText (*pFirst, "kept");
    Check (pFirst->IsReplayPending(), "lifecycle: the second object did not change the WAL of the first");

    delete pFirst;

    SimpleWAL Third;

    Check (Third.Init (Path), "lifecycle: when the first object is gone the WAL can be opened again");
    ReplayAll (Third, Records);
    Check (Contains (Records, "kept"), "lifecycle: the record is still there");

    /* The same object opens it again: the old file is closed first, which also ends its lock */
    AppendText (Third, "second");
    Check (Third.Init (Path), "lifecycle: the same object can open the WAL again");

    Records.clear();
    ReplayAll (Third, Records);
    Check (Contains (Records, "second"), "lifecycle: the record which was there before is still there");
}


int main (int argc, char *argv[])
{
    std::string Parent = "/tmp";
    size_t Records = 100000;

    ::signal (SIGPIPE, SIG_IGN);

    /* The result must not depend on the umask of whoever runs the test. This is the usual one */
    ::umask (022);

    /* Line buffered, so that the messages of the WAL on stderr appear where they belong when the output is piped */
    setvbuf (stdout, NULL, _IOLBF, 0);

    /* The WAL writes its messages to stdout by default. Here stdout is the report of the test, and many tests cause messages on purpose
       (a record which was cut off, a commit file which is damaged ...): between the checks they would look like failures, and they are
       no result. So the test is quiet. The messages are checked in the group "Messages of the WAL", with a function which collects them.
       A test which needs another target sets it and puts this one back */
    SimpleWAL::SetDefaultLogTarget (SimpleWAL::LOG_NONE);

    for (int i = 1; i < argc; i++)
    {
        if ( (0 == strcmp (argv[i], "-n")) && (i + 1 < argc) )
            Records = static_cast<size_t> (strtoul (argv[++i], NULL, 10));
        else
            Parent = argv[i];
    }

    /* The test works in a directory of its own inside the parent directory, and removes it at the end */
    std::string Template = Parent + "/wal_unit_test_XXXXXX";
    std::vector<char> TemplateBuffer (Template.begin(), Template.end());
    TemplateBuffer.push_back ('\0');

    char *pszDir = ::mkdtemp (TemplateBuffer.data());

    if (NULL == pszDir)
    {
        perror ("Cannot create the working directory");
        return 2;
    }

    g_Dir = pszDir;

    Group ("WAL unit test");
    printf ("Working directory: %s\n", g_Dir.c_str());
    printf ("File system:       %s\n", FileSystemName (g_Dir).c_str());

    Group ("Messages of the WAL: the log function, the targets, the lock on the file");
    TestMessagesFunction();
    TestMessagesTargets();
    TestMessagesFunctionCallsWal();
    TestLifecycle();

    Group ("Threads: one WAL used by several threads");
    TestThreadCallbackCallsWal();
    TestThreadAppendDuringReplay();
    TestThreadSettings();
    TestThreadManyConsumers();

    Group ("Behaviour");
    TestRoundTrip();
    TestPartialReplay();
    TestNoProgress();
    TestAppendEdgeCases();
    TestRestart();
    TestPartialReplayRestart();
    TestDestructor();
    TestClear();
    TestLargeAndMany();
    TestConcurrentAppend();
    TestProducerConsumer (Records);

    Group ("Peek and Ack: one record at a time");
    TestPeekAckBasics();
    TestPeekAckRestart();
    TestPeekAckAppend();
    TestPeekAckClear();
    TestPeekAckReplay();
    TestPeekAckDamaged();
    TestPeekAckThreads();

    Group ("Size limit");
    TestSizeLimit();

    Group ("Damaged files: the replay must not get stuck");
    TestCommitAtEnd();
    TestStaleCommit();
    TestTornTail();
    TestTornHeader();
    TestDamagedLength();
    TestShortCommitFile();
    TestDamagedCommitFile();
    TestMisalignedCommit();
    TestZeroLengthHeader();
    TestReopenAfterRecovery();

    Group ("Failures while saving: a full disk, and files which cannot be changed");
    TestAppendFailure();
    TestCommitWriteFailure();
    TestTruncateFailure();
    TestCommitRemovalFailure();
    TestQuarantineFailure();

    Group ("Files, settings and crashes");
    TestFileModes();
    TestOldFileModes();
    TestCloseOnExec();
    TestInitialized();
    TestSyncOption();
    TestCrash();

    Group ("Performance");
    TestPerformance (Records);

    /* The directory was created by this test and only contains its files */
    std::string Command = "rm -rf '" + g_Dir + "'";

    if (0 != ::system (Command.c_str()))
        fprintf (stderr, "could not remove %s\n", g_Dir.c_str());

    if (false == g_FailedNames.empty())
    {
        Group ("Failed checks");

        for (const std::string& Name : g_FailedNames)
            printf ("[FAIL]  %s\n", Name.c_str());
    }

    Group ("Result");
    printf ("%s  %d of %d checks passed, %d failed\n\n", g_Failed ? "[FAIL]" : "[PASS]", g_Total - g_Failed, g_Total, g_Failed);

    return g_Failed ? 1 : 0;
}
