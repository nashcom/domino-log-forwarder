
/* Unit test of the WAL module (simple_wal.cpp). A separate program which only links the WAL: no otelfwd, no network.

   Build and run:  make wal_unit_test && ./wal_unit_test        (or: make test)
   Options:        -n COUNT      number of records for the producer and consumer test and for the performance table (default 100000)
                   DIRECTORY     the parent directory of the test files (default /tmp, which can be a RAM file system). The test makes
                                 a directory of its own in it and removes it at the end. Use it to measure a real disk, for example
                                 the directory of the WAL of otelfwd

   Everything happens in a private temporary directory. Every check prints [PASS] or [FAIL]. The end of the output has a
   section "Failed checks" (only if there are any) and a section "Result" with the overall status. The exit code is 0 if every check passed and 1 if any check failed.

   Two groups of tests:

   - Behaviour: what the WAL is meant to do (order, partial replay, restart, concurrency, large records ...)
   - Findings:  one test for each problem which the code review found. They are named "[finding N]". A finding test fails
                as long as the problem is not fixed, and passes once it is. The numbers are those of the review:
                  1  WAL replay can get stuck (commit offset at the end or behind the end, torn or damaged record)
                  2  WAL creation and append failures (partial record after a failed append, file modes, uninitialized
                     members, commit file read and write) */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <signal.h>
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


static bool Contains (const std::vector<std::string>& Records, const char *pszText)
{
    for (const std::string& Record : Records)
    {
        if (Record == pszText)
            return true;
    }

    return false;
}


/* Returns what the action writes to stderr */
static std::string CaptureStderr (const std::function<void()>& Action)
{
    std::string Path   = g_Dir + "/stderr.txt";
    std::string Result;
    char   szBuffer[512];
    size_t Read = 0;

    fflush (stderr);

    int fdOld = ::dup (2);
    int fdNew = ::open (Path.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);

    ::dup2 (fdNew, 2);
    ::close (fdNew);

    Action();

    fflush (stderr);
    ::dup2 (fdOld, 2);
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


static void TestReplaySingleCommit ()
{
    std::string Path = WalPath ("single");
    std::vector<std::string> Records;
    int Count = 0;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "one");
    AppendText (Wal, "two");
    AppendText (Wal, "three");

    Wal.ReplaySingleCommit ([&Count, &Records] (const std::vector<uint8_t>& Record)
    {
        Count++;
        Records.push_back (std::string (Record.begin(), Record.end()));
        return (Count < 3);
    });

    uint64_t Commit = 0;

    Check (3 == Count, "single commit: stops at the record which is refused");
    Check (ReadCommitFile (Path, Commit) && Commit == 4 + 3 + 4 + 3, "single commit: the commit is after every record which was accepted");
}


/* ============================== Findings ============================== */


/* Finding 1: the commit offset points exactly to the end of the WAL (everything was replayed, but the WAL was not cleared,
   for example after a crash). Replay must recognize that, clear the WAL and not stay pending forever */
static void TestFinding1CommitAtEnd ()
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

    Check (0 == Consumed, "[finding 1] commit at the end: nothing is delivered again");
    Check (false == Wal.IsReplayPending(), "[finding 1] commit at the end: the WAL is not pending forever");
    Check (0 == FileSize (Path), "[finding 1] commit at the end: the WAL file is cleared");
}


/* Finding 1: the WAL was truncated, but the commit file of the old WAL was not removed (crash in between). A new record which
   is appended afterwards must be replayed, not skipped because of the old offset */
static void TestFinding1StaleCommit ()
{
    std::string Path = WalPath ("f1stale");
    std::vector<std::string> Records;

    RemoveFiles (Path);
    WriteCommitFile (Path, 4096);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "new record");

    Check (ReplayAll (Wal, Records), "[finding 1] stale commit: replay succeeds");
    Check (Contains (Records, "new record"), "[finding 1] stale commit: the new record is delivered");
    Check (false == Wal.IsReplayPending(), "[finding 1] stale commit: nothing stays pending");
}


/* Finding 1: a record was cut off by a crash: a header and only a part of the data. The WAL must not stay stuck at it,
   and a record which is appended afterwards must be delivered */
static void TestFinding1TornTail ()
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
    Check (Contains (Records, "good"), "[finding 1] torn tail: the good record is delivered");
    Check (6 == FileSize (Path + ".corrupt"), "[finding 1] torn tail: the unreadable bytes are kept in the .corrupt file, not thrown away");
    Check (0 == (FileMode (Path + ".corrupt") & 077), "[finding 1] torn tail: the .corrupt file is only accessible for its owner");

    AppendText (Wal, "after");
    ReplayAll (Wal, Records);
    ReplayAll (Wal, Records);

    Check (Contains (Records, "after"), "[finding 1] torn tail: a record appended afterwards is delivered");
    Check (false == Wal.IsReplayPending(), "[finding 1] torn tail: nothing stays pending");
}


/* Finding 1: the crash cut off even the header of a record: fewer than the 4 bytes of the length are there, and nothing else.
   The WAL must not stay pending because of two stray bytes */
static void TestFinding1TornHeader ()
{
    std::string Path = WalPath ("f1header");
    std::vector<std::string> Records;

    RemoveFiles (Path);
    AppendBytes (Path, "ab", 2);

    SimpleWAL Wal;
    Wal.Init (Path);

    Check (Wal.IsReplayPending(), "[finding 1] torn header: the stray bytes are pending after the start");
    Check (ReplayAll (Wal, Records) && Records.empty(), "[finding 1] torn header: replay succeeds, nothing is delivered");
    Check (false == Wal.IsReplayPending() && 0 == FileSize (Path), "[finding 1] torn header: the WAL is cleared, it does not stay stuck");
    Check (2 == FileSize (Path + ".corrupt"), "[finding 1] torn header: the two bytes are kept in the .corrupt file");
}


/* Finding 1 / 2: a damaged length of 4 GB. Replay must not try to allocate that much (and must not throw).
   The address space of this test is limited for the check, so that a real allocation fails instead of using all memory */
static void TestFinding1DamagedLength ()
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

    Check (false == bThrown, "[finding 1] damaged length: replay does not throw (no allocation of 4 GB)");
    Check (1 == Records.size() && "good" == Records[0], "[finding 1] damaged length: the good record is delivered, nothing from the damaged one");
    Check (false == Wal.IsReplayPending(), "[finding 1] damaged length: the WAL does not stay stuck");
    Check (8 == FileSize (Path + ".corrupt"), "[finding 1] damaged length: the unreadable bytes are kept in the .corrupt file");
}


/* Finding 2: the disk is full in the middle of a record. The failed append must not leave a piece of it in the WAL.
   The limit for the size of a file simulates that. Records before and after must still be readable */
static void TestFinding2AppendFailure ()
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

    Check (false == bAppended, "[finding 2] append failure: reported");
    Check (SizeBefore == FileSize (Path), "[finding 2] append failure: no piece of the record is left in the WAL");

    AppendText (Wal, "after");
    ReplayAll (Wal, Records);

    Check (Contains (Records, "before") && Contains (Records, "after"), "[finding 2] append failure: the records before and after it are delivered");
}


/* Finding 2: the WAL contains log lines. Its files must only be accessible for the owner */
static void TestFinding2FileModes ()
{
    std::string Path = WalPath ("f2modes");
    int Count = 0;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.Init (Path);
    AppendText (Wal, "one");
    AppendText (Wal, "two");

    Check (0 == (FileMode (Path) & 077), "[finding 2] file modes: the WAL file is only accessible for its owner");

    Wal.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return (Count < 2); });

    Check (FileExists (Path + ".commit") && 0 == (FileMode (Path + ".commit") & 077), "[finding 2] file modes: the commit file is only accessible for its owner");
}


/* Finding 2: files which an earlier version created with more permissions become private too, when the WAL is opened and when the
   commit file is written again */
static void TestFinding2OldFileModes ()
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

    Check (0 == (FileMode (Path) & 077), "[finding 2] old file modes: an existing WAL file becomes private when it is opened");

    AppendText (Wal, "three");
    Wal.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return (Count < 2); });

    Check (0 == (FileMode (Path + ".commit") & 077), "[finding 2] old file modes: the commit file becomes private when it is written again");
}


/* Finding 2: members must be initialized. The object is created in memory which is filled with 0xAA, so that a member which the
   constructor does not set has a value which is not zero. The log level decides if the WAL writes messages to stderr (for example
   "WAL reset" when it is cleared). A new WAL has to be silent, whatever was in the memory before */
static void TestFinding2Initialized ()
{
    std::string Path = WalPath ("f2init");
    alignas (SimpleWAL) static unsigned char Raw[sizeof (SimpleWAL)];
    volatile unsigned char *pFill = Raw;

    RemoveFiles (Path);

    for (size_t i = 0; i < sizeof (Raw); i++)
        pFill[i] = 0xAA;

    SimpleWAL *pWal = new (Raw) SimpleWAL();

    std::string Output = CaptureStderr ([pWal, &Path] ()
    {
        pWal->Init (Path);
        pWal->Clear();
    });

    pWal->~SimpleWAL();

    Check (Output.empty(), "[finding 2] members: a new WAL is silent, its log level does not depend on what was in the memory");
}


/* Finding 2: the commit file has to be read completely. A file which is not one offset is damaged and must not be used.
   The records are then replayed from the start (at worst a record twice, never lost) */
static void TestFinding2ShortCommitFile ()
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

    Check (ReplayAll (Wal, Records), "[finding 2] short commit file: replay succeeds");
    Check (2 == Records.size(), "[finding 2] short commit file: the WAL is replayed from the start");
}


/* Finding 2: a commit file which is empty (the crash came between creating it and writing to it) or too long is damaged too */
static void TestFinding2DamagedCommitFile ()
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

        std::string Name = std::string ("[finding 2] ") + (0 == i ? "empty" : "too long") + " commit file: ";

        Check (ReplayAll (Wal, Records) && 2 == Records.size(), (Name + "the WAL is replayed from the start").c_str());
        Check (false == Wal.IsReplayPending(), (Name + "nothing stays pending").c_str());
    }
}


/* ============================== Second review ============================== */


/* Review 2: a commit file with the right size (8 bytes) but a position which is inside a record, not at the start of one. It can
   be a damaged file or the file of an older WAL. Replay must not read the middle of a record as a length, and must not skip
   records. It notices that the position is not the start of a record and replays from the start. A valid position is used */
static void TestReview2MisalignedCommit ()
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

        std::string Name = "[review 2] misaligned commit offset " + std::to_string (Case.Offset) + " (" + Case.pszWhere + "): ";

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

        std::string Name = "[review 2] valid commit offset " + std::to_string (Case.Offset) + ": ";

        Check (Case.Expected == Records.size() && Case.pszFirst == Records[0], (Name + "the records before it are not delivered again").c_str());
    }
}


/* Review 2: the commit position cannot be written (the size limit of a file stands for a full disk). The records were accepted
   by the consumer, but the progress is not saved. Replay must not report progress and the WAL stays pending. After a restart
   the records are delivered again (at least once), none disappear */
static void TestReview2CommitWriteFailure ()
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

        Check (false == bResult, "[review 2] commit write failure: Replay reports the failure, it does not claim progress which was not saved");
        Check (Wal.IsReplayPending(), "[review 2] commit write failure: the WAL stays pending");
        Check (FileSize (Path) > 0, "[review 2] commit write failure: the records are still in the WAL");
    }

    SimpleWAL Wal2;
    Wal2.Init (Path);
    ReplayAll (Wal2, Records);

    Check (3 == Records.size() && "one" == Records[0] && "two" == Records[1] && "three" == Records[2],
          "[review 2] commit write failure: after a restart all records are delivered again (at least once), none disappeared");
}


/* Review 2: a zero length in the middle of the WAL is not the end of it. Append never writes one, so the file is damaged.
   The zero and everything behind it is kept in the .corrupt file: valid records are not silently discarded */
static void TestReview2ZeroLengthHeader ()
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

    Check (1 == Records.size() && "one" == Records[0], "[review 2] zero length header: the record before it is delivered");
    Check (11 == FileSize (Path + ".corrupt"), "[review 2] zero length header: it is not an end marker. The zero and the record behind it (4 + 4 + 3 bytes) are kept in the .corrupt file");
    Check (false == Wal.IsReplayPending() && 0 == FileSize (Path), "[review 2] zero length header: the WAL is cleared");
}


/* Review 2: after a recovery the repaired state is on disk. A new WAL object, as after a restart, starts clean and works */
static void TestReview2ReopenAfterRecovery ()
{
    const char *Kinds[3] = { "torn tail", "damaged length", "zero length header" };

    for (int k = 0; k < 3; k++)
    {
        std::string Path = WalPath ((std::string ("r2reopen") + std::to_string (k)).c_str());
        std::vector<std::string> First;
        std::vector<std::string> Second;
        std::string Name = std::string ("[review 2] reopen after ") + Kinds[k] + ": ";

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


/* Review 2: the option which calls fsync after every write. It cannot prove that data survives a power failure, but it runs the code */
static void TestReview2SyncOption ()
{
    std::string Path = WalPath ("r2sync");
    std::vector<std::string> Records;
    int Count = 0;

    RemoveFiles (Path);

    SimpleWAL Wal;
    Wal.SetCommit (true);
    Wal.Init (Path);

    Check (Wal.GetCommit(), "[review 2] sync option: it is on after SetCommit(true)");

    AppendText (Wal, "one");
    AppendText (Wal, "two");
    AppendText (Wal, "three");

    Wal.Replay ([&Count] (const std::vector<uint8_t>&) { Count++; return (Count < 2); });

    uint64_t Commit = 0;

    Check (ReadCommitFile (Path, Commit) && 7 == Commit, "[review 2] sync option: the commit position is written after a partial replay");

    ReplayAll (Wal, Records);

    Check (2 == Records.size() && "two" == Records[0] && "three" == Records[1], "[review 2] sync option: the records which were not replayed are delivered");
    Check (false == Wal.IsReplayPending() && 0 == FileSize (Path), "[review 2] sync option: the WAL is cleared");
}


/* Review 2: the crash contract, with real crashes. A child process ends with _exit(), which runs no destructor and no cleanup:
   what it wrote to the WAL has to be there. And a crash inside the replay, after a record was delivered and before the position
   was saved: the record is delivered again, at least once, and none disappears */
static void TestReview2Crash ()
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

    Check (WIFEXITED (Status) && 0 == WEXITSTATUS (Status), "[review 2] crash after append: the child ended");

    {
        SimpleWAL Wal;
        Wal.Init (Path);

        Check (Wal.IsReplayPending(), "[review 2] crash after append: the records are pending after the restart");
        ReplayAll (Wal, Records);
        Check (2 == Records.size() && "survivor 1" == Records[0] && "survivor 2" == Records[1], "[review 2] crash after append: both records are there");
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

    Check (WIFEXITED (Status) && 0 == WEXITSTATUS (Status), "[review 2] crash inside the replay: the child ended inside the callback");

    SimpleWAL Wal;
    Wal.Init (Path);
    ReplayAll (Wal, Again);

    Check (Wal.IsReplayPending() == false && 3 == Again.size() && "one" == Again[0] && "two" == Again[1] && "three" == Again[2],
          "[review 2] crash inside the replay: all records are delivered again after the restart, none disappeared (at least once)");
}


/* Review 2: the WAL cannot be truncated (fault injection). Replay must not claim that everything is done, the WAL stays pending
   and the records are still there. Later they are delivered again (at least once) */
static void TestReview2TruncateFailure ()
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

        Check (2 == First.size(), "[review 2] truncate failure: the records were delivered to the consumer");
        Check (false == bResult, "[review 2] truncate failure: Replay reports the failure, delivering the records is not enough");
        Check (Wal.IsReplayPending() && FileSize (Path) > 0, "[review 2] truncate failure: the WAL stays pending and keeps the records");
    }

    SimpleWAL Wal2;
    Wal2.Init (Path);
    ReplayAll (Wal2, Second);

    Check (2 == Second.size(), "[review 2] truncate failure: the records are delivered again later (at least once)");
    Check (false == Wal2.IsReplayPending() && 0 == FileSize (Path), "[review 2] truncate failure: the WAL is cleared when the failure is gone");
}


/* Review 2: the commit file cannot be removed (fault injection). If the WAL were truncated anyway, the old position would be left
   behind and does not fit to the next records. The WAL must stay as it is, pending, and report the failure */
static void TestReview2CommitRemovalFailure ()
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

    Check (2 == First.size() && "two" == First[0] && "three" == First[1], "[review 2] commit removal failure: the records after the position were delivered");
    Check (false == bResult, "[review 2] commit removal failure: Replay reports the failure");
    Check (Wal.IsReplayPending(), "[review 2] commit removal failure: the WAL stays pending");
    Check (SizeBefore == FileSize (Path) && FileExists (Path + ".commit"), "[review 2] commit removal failure: the WAL is not truncated, the old position does not get left behind");

    ReplayAll (Wal, Second);

    Check (2 == Second.size() && false == Wal.IsReplayPending() && 0 == FileSize (Path), "[review 2] commit removal failure: delivered again (at least once) and cleared when the failure is gone");
}


/* Review 2: the unreadable part cannot be moved to the .corrupt file (a directory has its name, so it cannot be created).
   The WAL must not be cleared: nothing may be thrown away. What was readable is delivered. Later it is repaired */
static void TestReview2QuarantineFailure ()
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

    Check (1 == First.size() && "good" == First[0], "[review 2] quarantine failure: the readable record is delivered");
    Check (Wal.IsReplayPending() && FileSize (Path) == 4 + 4 + 4 + 2, "[review 2] quarantine failure: the WAL is not cleared, the unreadable bytes are still in it");

    ::rmdir ((Path + ".corrupt").c_str());
    ReplayAll (Wal, Second);

    Check (Second.empty() && false == Wal.IsReplayPending() && 0 == FileSize (Path), "[review 2] quarantine failure: the WAL is repaired when the failure is gone, the good record is not delivered twice");
    Check (6 == FileSize (Path + ".corrupt"), "[review 2] quarantine failure: the unreadable bytes are kept in the .corrupt file");

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

    Check (false == bResult && Wal2.IsReplayPending() && 2 == FileSize (Path2), "[review 2] quarantine failure: nothing readable and nothing moved: Replay reports the failure and keeps the bytes");
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
    WalSync.SetCommit (true);
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


int main (int argc, char *argv[])
{
    std::string Parent = "/tmp";
    size_t Records = 100000;

    ::signal (SIGPIPE, SIG_IGN);

    /* The result must not depend on the umask of whoever runs the test. This is the usual one */
    ::umask (022);

    /* Line buffered, so that the messages of the WAL on stderr appear where they belong when the output is piped */
    setvbuf (stdout, NULL, _IOLBF, 0);

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
    TestReplaySingleCommit();

    Group ("Findings of the review (fail until the finding is fixed)");
    TestFinding1CommitAtEnd();
    TestFinding1StaleCommit();
    TestFinding1TornTail();
    TestFinding1TornHeader();
    TestFinding1DamagedLength();
    TestFinding2AppendFailure();
    TestFinding2FileModes();
    TestFinding2OldFileModes();
    TestFinding2Initialized();
    TestFinding2ShortCommitFile();
    TestFinding2DamagedCommitFile();

    Group ("Second review: commit position, failures while saving, crashes");
    TestReview2MisalignedCommit();
    TestReview2CommitWriteFailure();
    TestReview2ZeroLengthHeader();
    TestReview2ReopenAfterRecovery();
    TestReview2SyncOption();
    TestReview2Crash();
    TestReview2TruncateFailure();
    TestReview2CommitRemovalFailure();
    TestReview2QuarantineFailure();

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
