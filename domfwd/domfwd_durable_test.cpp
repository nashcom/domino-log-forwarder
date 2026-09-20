
/* Unit test of the durable line sender (domfwd_durable.hpp): the socket sender of domfwd (domfwd_socket.hpp) together with the WAL,
   so that lines wait on disk while the forwarder is not there. It is also the test of the socket sender.

   The test starts a small server of its own on a UNIX socket in a private directory. It can be stopped and started again, which is
   what happens to otelfwd, and it can stop reading, which keeps lines in the queue of the sender. No Domino, no otelfwd.

   Build and run:  make domfwd_durable_test && ./domfwd_durable_test        (or: make test, in the repository root)

   Every check prints [PASS] or [FAIL]. The end of the output has a section "Failed checks" (only if there are any) and a
   section "Result". The exit code is 0 if every check passed and 1 if any check failed. */

/* Short times, so that the test does not wait. These are the settings of the sender and of the durable sender */
#define DOMFWD_SOCKET_RECONNECT_MS   100
#define DOMFWD_DURABLE_LOW_WATER     4
#define DOMFWD_DURABLE_DRAIN_MAX     10

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "domfwd_durable.hpp"


static int g_Total  = 0;
static int g_Failed = 0;
static std::vector<std::string> g_FailedNames;
static std::string g_Dir;


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


/* The messages of the senders */
static std::vector<std::string> g_Log;

static void TestLog (const char *pszMessage)
{
    g_Log.push_back (pszMessage);
}


static bool LogHas (const char *pszPart)
{
    for (const std::string& Message : g_Log)
    {
        if (std::string::npos != Message.find (pszPart))
            return true;
    }

    return false;
}


/* The other end: stands for otelfwd. Lines are read, split at the new line, and kept. A part of a line which was cut off by a
   closed connection is thrown away, as otelfwd does */
class TestServer
{
public:

    explicit TestServer (const std::string& Path) : m_Path (Path) {}

    ~TestServer()
    {
        Stop();
    }

    /* The socket exists when this returns: a client can connect at once */
    bool Start()
    {
        Stop();

        ::unlink (m_Path.c_str());

        struct sockaddr_un Addr;

        memset (&Addr, 0, sizeof (Addr));
        Addr.sun_family = AF_UNIX;
        snprintf (Addr.sun_path, sizeof (Addr.sun_path), "%s", m_Path.c_str());

        m_ListenFd = ::socket (AF_UNIX, SOCK_STREAM, 0);

        if ( (m_ListenFd < 0) || (::bind (m_ListenFd, reinterpret_cast<struct sockaddr *> (&Addr), sizeof (Addr)) < 0) || (::listen (m_ListenFd, 8) < 0) )
        {
            if (m_ListenFd >= 0)
                ::close (m_ListenFd);

            m_ListenFd = -1;
            return false;
        }

        m_bRun = true;
        m_Thread = std::thread ([this] { Run(); });

        return true;
    }

    /* All connections are closed, and the socket is gone: the server is down */
    void Stop()
    {
        if (false == m_Thread.joinable())
            return;

        m_bRun = false;
        m_Thread.join();

        for (int Fd : m_Conns)
            ::close (Fd);

        m_Conns.clear();
        m_Buffers.clear();

        ::close (m_ListenFd);
        m_ListenFd = -1;

        ::unlink (m_Path.c_str());
    }

    /* Without reading the connection stays open, and the socket fills up: the sender cannot send more */
    void SetReading (bool bReading)
    {
        m_bReading = bReading;
    }

    void ForgetLines()
    {
        std::lock_guard<std::mutex> Lock (m_Mutex);

        m_Lines.clear();
    }

    std::vector<std::string> Lines()
    {
        std::lock_guard<std::mutex> Lock (m_Mutex);

        return m_Lines;
    }

    bool WaitForLines (size_t Count, int TimeoutMs)
    {
        auto End = std::chrono::steady_clock::now() + std::chrono::milliseconds (TimeoutMs);

        while (std::chrono::steady_clock::now() < End)
        {
            if (Lines().size() >= Count)
                return true;

            std::this_thread::sleep_for (std::chrono::milliseconds (5));
        }

        return Lines().size() >= Count;
    }

private:

    std::string               m_Path;
    int                       m_ListenFd = -1;
    std::thread               m_Thread;
    std::atomic<bool>         m_bRun {false};
    std::atomic<bool>         m_bReading {true};
    std::vector<int>          m_Conns;
    std::vector<std::string>  m_Buffers;
    std::mutex                m_Mutex;
    std::vector<std::string>  m_Lines;

    void Run()
    {
        while (m_bRun.load())
        {
            std::vector<struct pollfd> Fds;
            struct pollfd Listen;

            Listen.fd      = m_ListenFd;
            Listen.events  = POLLIN;
            Listen.revents = 0;
            Fds.push_back (Listen);

            if (m_bReading.load())
            {
                for (int Fd : m_Conns)
                {
                    struct pollfd Conn;

                    Conn.fd      = Fd;
                    Conn.events  = POLLIN;
                    Conn.revents = 0;
                    Fds.push_back (Conn);
                }
            }

            ::poll (Fds.data(), static_cast<nfds_t> (Fds.size()), 10);

            std::vector<size_t> Closed;

            for (size_t i = 1; i < Fds.size(); i++)
            {
                if (0 == (Fds[i].revents & (POLLIN | POLLHUP)))
                    continue;

                char    szBuffer[65536];
                ssize_t Read = ::read (Fds[i].fd, szBuffer, sizeof (szBuffer));

                if (Read <= 0)
                {
                    Closed.push_back (i - 1);
                    continue;
                }

                std::string& Buffer = m_Buffers[i - 1];

                Buffer.append (szBuffer, static_cast<size_t> (Read));

                size_t Pos = 0;
                size_t End = 0;

                while (std::string::npos != (End = Buffer.find ('\n', Pos)))
                {
                    std::lock_guard<std::mutex> Lock (m_Mutex);

                    m_Lines.push_back (Buffer.substr (Pos, End - Pos));
                    Pos = End + 1;
                }

                Buffer.erase (0, Pos);
            }

            for (size_t k = Closed.size(); k > 0; k--)
            {
                size_t Index = Closed[k - 1];

                ::close (m_Conns[Index]);
                m_Conns.erase (m_Conns.begin() + static_cast<long> (Index));
                m_Buffers.erase (m_Buffers.begin() + static_cast<long> (Index));
            }

            if (Fds[0].revents & POLLIN)
            {
                int Conn = ::accept (m_ListenFd, NULL, NULL);

                if (Conn >= 0)
                {
                    m_Conns.push_back (Conn);
                    m_Buffers.push_back (std::string());
                }
            }
        }
    }
};


/* --- Helpers --- */

/* "line-0007" and, for the tests of large lines, the same padded to a length */
static std::string MakeLine (int Number, size_t Length = 0)
{
    char szNumber[32];

    snprintf (szNumber, sizeof (szNumber), "line-%04d", Number);

    std::string Line = szNumber;

    if (Length > Line.size())
        Line.append (Length - Line.size(), 'x');

    return Line;
}


/* Are the lines from First on exactly the lines First ... First + Count - 1, in this order? */
static bool IsSequence (const std::vector<std::string>& Lines, int First, size_t Count, size_t Length = 0)
{
    if (Lines.size() != Count)
        return false;

    for (size_t i = 0; i < Count; i++)
    {
        if (Lines[i] != MakeLine (First + static_cast<int> (i), Length))
            return false;
    }

    return true;
}


/* Pumps the sender, like the main loop of domfwd, until the condition is true */
static bool PumpUntil (DomfwdDurableSender& Sender, const std::function<bool()>& Done, int TimeoutMs)
{
    auto End = std::chrono::steady_clock::now() + std::chrono::milliseconds (TimeoutMs);

    while (std::chrono::steady_clock::now() < End)
    {
        Sender.Pump();

        if (Done())
            return true;

        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    return Done();
}


static std::string SocketPath (const char *pszName)
{
    return g_Dir + "/" + pszName + ".sock";
}


static std::string WalPath (const char *pszName)
{
    return g_Dir + "/" + pszName + ".wal";
}


static void RemoveWal (const std::string& Path)
{
    ::unlink (Path.c_str());
    ::unlink ((Path + ".commit").c_str());
    ::unlink ((Path + ".corrupt").c_str());
}


/* A configured sender. pszWalPath NULL: without a WAL */
static std::unique_ptr<DomfwdDurableSender> MakeSender (const std::string& Socket, const char *pszWalPath, uint64_t WalMaxBytes = 0,
                                                        size_t MaxLines = 2000, size_t MaxBytes = 4 * 1024 * 1024)
{
    std::unique_ptr<DomfwdDurableSender> pSender (new DomfwdDurableSender);
    char szError[256] = {0};

    if (false == pSender->Configure (("unix:" + Socket).c_str(), TestLog, MaxLines, MaxBytes, pszWalPath, WalMaxBytes, szError, sizeof (szError)))
        printf ("        Configure failed: %s\n", szError);

    return pSender;
}


/* --- The tests --- */

/* Nothing new for a receiver which is there: the lines are sent at once, and the WAL is not used */
static void TestReceiverUp ()
{
    TestServer Server (SocketPath ("up"));
    std::string Wal = WalPath ("up");

    RemoveWal (Wal);
    Server.Start();

    auto pSender = MakeSender (SocketPath ("up"), Wal.c_str());

    Check (pSender->HasWal(), "receiver up: the WAL is open");

    /* The first Send() comes before the sender was connected: it waits in the WAL. So connect first */
    PumpUntil (*pSender, [&] { return pSender->IsConnected(); }, 3000);

    for (int i = 1; i <= 5; i++)
        Check (pSender->Send (MakeLine (i).data(), MakeLine (i).size()), "receiver up: a line is accepted");

    Check (Server.WaitForLines (5, 3000) || PumpUntil (*pSender, [&] { return Server.Lines().size() >= 5; }, 3000), "receiver up: the 5 lines arrive");
    Check (IsSequence (Server.Lines(), 1, 5), "receiver up: in the order they were sent");
    Check ((0 == pSender->GetSpilled()) && (0 == pSender->GetWalSize()), "receiver up: nothing went to the WAL");
    Check (5 == pSender->GetSent(), "receiver up: the sender counts 5 lines as sent");
}


/* The receiver is not there: the lines wait in the WAL, not in memory. When it comes, they arrive in order */
static void TestReceiverDown ()
{
    TestServer Server (SocketPath ("down"));
    std::string Wal = WalPath ("down");

    RemoveWal (Wal);

    auto pSender = MakeSender (SocketPath ("down"), Wal.c_str());

    for (int i = 1; i <= 10; i++)
        pSender->Send (MakeLine (i).data(), MakeLine (i).size());

    Check (10 == pSender->GetSpilled(), "receiver down: all 10 lines went to the WAL");
    Check (0 == pSender->GetQueuedLines(), "receiver down: none of them waits in memory, where a crash would lose it");
    Check (pSender->GetWalSize() > 0, "receiver down: the WAL file holds them");

    /* The sender tries to connect and cannot */
    for (int i = 0; i < 5; i++)
    {
        pSender->Pump();
        std::this_thread::sleep_for (std::chrono::milliseconds (5));
    }

    Check (false == pSender->IsConnected(), "receiver down: the sender is not connected");
    Check (0 == pSender->GetSent(), "receiver down: nothing was sent");

    Server.Start();

    Check (PumpUntil (*pSender, [&] { return Server.Lines().size() >= 10; }, 5000), "receiver back: the 10 lines arrive");
    Check (IsSequence (Server.Lines(), 1, 10), "receiver back: in the order they were written");
    Check (10 == pSender->GetDrained(), "receiver back: they were moved from the WAL to the sender");
    Check (0 == pSender->GetWalSize(), "receiver back: the WAL is empty again");

    /* Now the fast path again */
    pSender->Send (MakeLine (11).data(), MakeLine (11).size());
    Check (10 == pSender->GetSpilled(), "receiver back: a new line does not go to the WAL any more");
    Check (PumpUntil (*pSender, [&] { return Server.Lines().size() >= 11; }, 3000), "receiver back: the new line arrives too");
}


/* The order: while lines wait in the WAL, a new line must not overtake them. It goes to the WAL too, also when the sender is
   connected already */
static void TestOrder ()
{
    TestServer Server (SocketPath ("order"));
    std::string Wal = WalPath ("order");

    RemoveWal (Wal);

    auto pSender = MakeSender (SocketPath ("order"), Wal.c_str());

    for (int i = 1; i <= 100; i++)
        pSender->Send (MakeLine (i).data(), MakeLine (i).size());

    Server.Start();
    PumpUntil (*pSender, [&] { return pSender->IsConnected(); }, 3000);

    Check (pSender->GetWalSize() > 0, "order: the sender is connected and the WAL still holds lines (10 lines are moved for every Pump)");

    for (int i = 101; i <= 103; i++)
        pSender->Send (MakeLine (i).data(), MakeLine (i).size());

    Check (103 == pSender->GetSpilled(), "order: the new lines went to the WAL, behind the old ones");
    Check (PumpUntil (*pSender, [&] { return Server.Lines().size() >= 103; }, 8000), "order: all 103 lines arrive");
    Check (IsSequence (Server.Lines(), 1, 103), "order: 1 to 100 and then 101 to 103, no line overtook another");
}


/* A queue which is smaller than the low water mark: the queue is full before the sender has sent anything. A record which the queue
   cannot take stays in the WAL. It must not be taken out and then be refused by the queue: that would lose the line */
static void TestSmallQueue ()
{
    TestServer Server (SocketPath ("small"));
    std::string Wal = WalPath ("small");

    RemoveWal (Wal);

    auto pSender = MakeSender (SocketPath ("small"), Wal.c_str(), 0, 2);

    for (int i = 1; i <= 20; i++)
        pSender->Send (MakeLine (i).data(), MakeLine (i).size());

    Server.Start();

    Check (PumpUntil (*pSender, [&] { return Server.Lines().size() >= 20; }, 10000), "small queue: all 20 lines arrive through a queue of 2");
    Check (IsSequence (Server.Lines(), 1, 20), "small queue: in order, none was lost");
    Check (0 == pSender->GetDropped(), "small queue: nothing was dropped");
}


/* The queue of the sender stays short while a backlog is moved: a line which is in the queue is not in the WAL any more, so after
   a crash only the lines of the queue are lost. The server does not read: the socket is full, and the sender cannot send more */
static void TestLowWater ()
{
    TestServer Server (SocketPath ("lowwater"));
    std::string Wal = WalPath ("lowwater");
    const size_t Length = 20 * 1024;

    RemoveWal (Wal);

    auto pSender = MakeSender (SocketPath ("lowwater"), Wal.c_str());

    for (int i = 1; i <= 100; i++)
        pSender->Send (MakeLine (i, Length).data(), MakeLine (i, Length).size());

    Server.SetReading (false);
    Server.Start();

    for (int i = 0; i < 100; i++)
    {
        pSender->Pump();
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    }

    Check (pSender->GetQueuedLines() > 0, "low water: the socket is full, lines wait in the queue");
    Check (pSender->GetQueuedLines() <= DOMFWD_DURABLE_LOW_WATER, "low water: the queue holds at most DOMFWD_DURABLE_LOW_WATER lines");
    Check (pSender->GetWalSize() > 0, "low water: the rest of the backlog is still in the WAL, where it is safe");
}


/* Room in the queue: HasRoom() must say what Enqueue() does, because the durable sender takes a line out of the WAL before it puts it
   into the queue. And the lines which were not sent can be taken out again, oldest first */
static void TestSenderRoom ()
{
    /* At most 3 lines */
    {
        DomfwdSocketSender Sender;
        char szError[128] = {0};

        Sender.Configure (("unix:" + SocketPath ("room1")).c_str(), TestLog, 3, 4 * 1024, szError, sizeof (szError));

        bool bAgree = true;

        for (int i = 1; i <= 5; i++)
        {
            bool bRoom     = Sender.HasRoom (20);
            bool bAccepted = Sender.Enqueue ("01234567890123456789", 20);

            if (bRoom != bAccepted)
                bAgree = false;
        }

        Check (bAgree, "sender room: with a limit of lines HasRoom() says what Enqueue() does");
        Check (3 == Sender.GetQueuedLines(), "sender room: 3 lines fit");
    }

    /* A limit of 1024 bytes for the queue. A line of 1000 bytes and its new line use 1001. So 22 bytes and the new line fit exactly
       (1001 + 23 = 1024), and 23 bytes do not */
    for (int Case = 0; Case < 2; Case++)
    {
        DomfwdSocketSender Sender;
        char szError[128] = {0};
        std::string Long (1000, 'x');
        size_t Next = (0 == Case) ? 22 : 23;

        Sender.Configure (("unix:" + SocketPath ((0 == Case) ? "room2" : "room3")).c_str(), TestLog, 1000, 1024, szError, sizeof (szError));

        Check (Sender.HasRoom (1000) && Sender.Enqueue (Long.data(), 1000), "sender room: a line of 1000 bytes fits into 1024");

        bool bRoom     = Sender.HasRoom (Next);
        bool bAccepted = Sender.Enqueue (Long.data(), Next);

        Check ((bRoom == bAccepted) && (bRoom == (0 == Case)),
               (0 == Case) ? "sender room: 22 bytes more fit exactly, and HasRoom() and Enqueue() agree"
                           : "sender room: 23 bytes more do not fit, and HasRoom() and Enqueue() agree");
    }

    /* Lines which were not sent, oldest first */
    {
        DomfwdSocketSender Sender;
        char szError[128] = {0};

        Sender.Configure (("unix:" + SocketPath ("room4")).c_str(), TestLog, 10, 4 * 1024, szError, sizeof (szError));

        Sender.Enqueue ("first", 5);
        Sender.Enqueue ("second", 6);
        Sender.Enqueue ("third", 5);

        const char *pLine = NULL;
        size_t      Len   = 0;

        Check (Sender.PeekUnsent (&pLine, &Len) && (5 == Len) && (0 == strncmp (pLine, "first", 5)), "unsent lines: the oldest line first");
        Check (Sender.PeekUnsent (&pLine, &Len) && (5 == Len) && (0 == strncmp (pLine, "first", 5)), "unsent lines: it stays until it is dropped");

        Sender.DropUnsent();

        Check (2 == Sender.GetQueuedLines(), "unsent lines: dropped, 2 are left");
        Check (Sender.PeekUnsent (&pLine, &Len) && (6 == Len) && (0 == strncmp (pLine, "second", 6)), "unsent lines: then the next one");

        Sender.DropUnsent();
        Sender.DropUnsent();

        Check ((0 == Sender.GetQueuedLines()) && (false == Sender.PeekUnsent (&pLine, &Len)), "unsent lines: nothing is left after the last one");
        Check ((0 == Sender.GetSent()) && (0 == Sender.GetDropped()) && (0 == Sender.GetRejected()), "unsent lines: they are not counted as sent, dropped or rejected");
        Check (Sender.Enqueue ("again", 5) && (1 == Sender.GetQueuedLines()), "unsent lines: the queue can be used again");
    }
}


/* Only some work for one Pump: the main loop of a servertask must not be held up by a backlog */
static void TestBoundedWork ()
{
    TestServer Server (SocketPath ("bounded"));
    std::string Wal = WalPath ("bounded");

    RemoveWal (Wal);

    auto pSender = MakeSender (SocketPath ("bounded"), Wal.c_str());

    for (int i = 1; i <= 1000; i++)
        pSender->Send (MakeLine (i).data(), MakeLine (i).size());

    Server.Start();
    PumpUntil (*pSender, [&] { return pSender->IsConnected(); }, 3000);

    uint64_t Before = pSender->GetDrained();

    pSender->Pump();

    Check (pSender->GetDrained() - Before <= DOMFWD_DURABLE_DRAIN_MAX, "bounded work: one Pump moves at most DOMFWD_DURABLE_DRAIN_MAX lines");

    Check (PumpUntil (*pSender, [&] { return Server.Lines().size() >= 1000; }, 20000), "bounded work: the whole backlog of 1000 lines arrives");
    Check (IsSequence (Server.Lines(), 1, 1000), "bounded work: in order");
}


/* The program ends while the receiver is down and starts again: the lines are still there */
static void TestRestart ()
{
    TestServer Server (SocketPath ("restart"));
    std::string Wal = WalPath ("restart");

    RemoveWal (Wal);

    {
        auto pSender = MakeSender (SocketPath ("restart"), Wal.c_str());

        for (int i = 1; i <= 5; i++)
            pSender->Send (MakeLine (i).data(), MakeLine (i).size());

        Check (5 == pSender->GetSpilled(), "restart: 5 lines went to the WAL");
    }   // the program ends

    auto pSender = MakeSender (SocketPath ("restart"), Wal.c_str());

    Check (pSender->GetWalSize() > 0, "restart: the new sender finds the lines of the earlier run in the WAL");

    Server.Start();

    Check (PumpUntil (*pSender, [&] { return Server.Lines().size() >= 5; }, 5000), "restart: the lines of the earlier run arrive");
    Check (IsSequence (Server.Lines(), 1, 5), "restart: in order");
    Check (0 == pSender->GetWalSize(), "restart: the WAL is empty afterwards");
}


/* Shutdown: what is still in the memory of the sender when the program ends goes to the WAL, and is sent at the next start.
   The server does not read, so the socket fills up and lines stay in the queue */
static void TestShutdown ()
{
    TestServer Server (SocketPath ("shutdown"));
    std::string Wal = WalPath ("shutdown");
    const int    Lines  = 60;
    const size_t Length = 20 * 1024;

    RemoveWal (Wal);

    Server.SetReading (false);
    Server.Start();

    int Spilled = 0;

    {
        auto pSender = MakeSender (SocketPath ("shutdown"), Wal.c_str());

        PumpUntil (*pSender, [&] { return pSender->IsConnected(); }, 3000);

        for (int i = 1; i <= Lines; i++)
            pSender->Send (MakeLine (i, Length).data(), MakeLine (i, Length).size());

        for (int i = 0; i < 20; i++)
        {
            pSender->Pump();
            std::this_thread::sleep_for (std::chrono::milliseconds (2));
        }

        Check (pSender->GetQueuedLines() > 0, "shutdown: the socket is full, lines wait in memory");

        bool bDone = pSender->Flush (100);

        Check (bDone, "shutdown: Flush says that nothing is left in memory");
        Check (0 == pSender->GetQueuedLines(), "shutdown: the queue is empty");
        Check (pSender->GetSpilled() > 0, "shutdown: the lines which could not be sent went to the WAL");
        Check (pSender->GetSent() + pSender->GetSpilled() == static_cast<uint64_t> (Lines), "shutdown: every line was either sent or is in the WAL, none is missing");

        Spilled = static_cast<int> (pSender->GetSpilled());
    }

    Server.Stop();
    Server.ForgetLines();
    Server.SetReading (true);
    Server.Start();

    auto pSender = MakeSender (SocketPath ("shutdown"), Wal.c_str());

    Check (PumpUntil (*pSender, [&] { return Server.Lines().size() >= static_cast<size_t> (Spilled); }, 10000), "shutdown: after the restart the lines from the WAL arrive");
    Check (IsSequence (Server.Lines(), Lines - Spilled + 1, static_cast<size_t> (Spilled), Length), "shutdown: they are the last lines, in order (the lines which the sender had written into the full socket are gone, as with any sender without an acknowledgement)");
}


/* The WAL has a size limit: what does not fit is refused, and what was stored arrives */
static void TestWalFull ()
{
    TestServer Server (SocketPath ("full"));
    std::string Wal = WalPath ("full");

    RemoveWal (Wal);

    /* A line of 46 bytes is 50 bytes in the WAL: 20 fit into 1000 bytes */
    auto pSender = MakeSender (SocketPath ("full"), Wal.c_str(), 1000);

    int Accepted = 0;

    for (int i = 1; i <= 100; i++)
    {
        if (pSender->Send (MakeLine (i, 46).data(), MakeLine (i, 46).size()))
            Accepted++;
    }

    Check (20 == Accepted, "WAL full: 20 lines were accepted, the others were refused");
    Check ((20 == pSender->GetSpilled()) && (80 == pSender->GetWalRefused()), "WAL full: the sender counts 20 stored and 80 refused");
    Check (pSender->GetDropped() >= 80, "WAL full: the refused lines are part of the dropped lines");
    Check (LogHas ("is full"), "WAL full: the WAL said so once");

    Server.Start();

    Check (PumpUntil (*pSender, [&] { return Server.Lines().size() >= 20; }, 5000), "WAL full: the 20 lines which were stored arrive");
    Check (IsSequence (Server.Lines(), 1, 20, 46), "WAL full: in order, the first 20");
}


/* A line which is not a line: with a new line inside, or empty. It is refused, and it does not get into the WAL, where it would
   block the lines behind it */
static void TestInvalidLines ()
{
    std::string Wal = WalPath ("invalid");

    RemoveWal (Wal);

    auto pSender = MakeSender (SocketPath ("invalid"), Wal.c_str());

    Check (false == pSender->Send ("two\nlines", 9), "invalid lines: a line with a new line is refused");
    Check (false == pSender->Send ("", 0), "invalid lines: an empty line is refused");
    Check (false == pSender->Send (NULL, 5), "invalid lines: no line is refused");
    Check (3 == pSender->GetRejected(), "invalid lines: all three are counted as rejected");
    Check ((0 == pSender->GetSpilled()) && (0 == pSender->GetWalSize()), "invalid lines: none of them is in the WAL");
}


/* A WAL which somebody else wrote and which holds a line with a new line inside: it is dropped, the line behind it is sent */
static void TestPoisonRecord ()
{
    TestServer Server (SocketPath ("poison"));
    std::string Wal = WalPath ("poison");

    RemoveWal (Wal);

    {
        SimpleWAL Plain;

        Plain.SetLogTarget (SimpleWAL::LOG_NONE);
        Plain.Init (Wal);
        Plain.Append ("bad\nline", 8);
        Plain.Append ("good", 4);
    }

    auto pSender = MakeSender (SocketPath ("poison"), Wal.c_str());

    Server.Start();

    Check (PumpUntil (*pSender, [&] { return Server.Lines().size() >= 1; }, 5000), "poison record: the line behind it arrives");
    Check ((1 == Server.Lines().size()) && ("good" == Server.Lines()[0]), "poison record: and only that line");
    Check (1 == pSender->GetRejected(), "poison record: the bad record is counted as rejected");
    Check (PumpUntil (*pSender, [&] { return 0 == pSender->GetWalSize(); }, 3000), "poison record: it did not stay in the WAL, which is empty now");
}


/* Without a WAL nothing changes: the lines wait in the memory queue of the sender, as before */
static void TestWithoutWal ()
{
    TestServer Server (SocketPath ("nowal"));

    auto pSender = MakeSender (SocketPath ("nowal"), NULL);

    Check (false == pSender->HasWal(), "without a WAL: there is none");

    for (int i = 1; i <= 5; i++)
        pSender->Send (MakeLine (i).data(), MakeLine (i).size());

    Check ((5 == pSender->GetQueuedLines()) && (0 == pSender->GetSpilled()), "without a WAL: the lines wait in memory, as before");

    Server.Start();

    Check (PumpUntil (*pSender, [&] { return Server.Lines().size() >= 5; }, 5000), "without a WAL: they arrive when the receiver is there");
    Check (IsSequence (Server.Lines(), 1, 5), "without a WAL: in order");
}


/* A WAL which cannot be opened is no reason to stop: it is reported, and the sender works as it did before */
static void TestWalCannotBeOpened ()
{
    g_Log.clear();

    auto pSender = MakeSender (SocketPath ("broken"), (g_Dir + "/no/such/directory/x.wal").c_str());

    Check (false == pSender->HasWal(), "WAL cannot be opened: the sender has none");
    Check (LogHas ("open wal failed"), "WAL cannot be opened: the reason is in the log");

    pSender->Send ("line", 4);

    Check (1 == pSender->GetQueuedLines(), "WAL cannot be opened: the line waits in memory, as without a WAL");
}


int main ()
{
    setvbuf (stdout, NULL, _IOLBF, 0);

    /* The messages of the WAL are checked where they matter. Otherwise the test is quiet */
    SimpleWAL::SetDefaultLogTarget (SimpleWAL::LOG_NONE);

    char szDir[] = "/tmp/domfwd_durable_test_XXXXXX";

    if (NULL == ::mkdtemp (szDir))
    {
        perror ("Cannot create the working directory");
        return 2;
    }

    g_Dir = szDir;

    Group ("Durable line sender test");
    printf ("Working directory: %s\n", g_Dir.c_str());

    Group ("A receiver which is there");
    TestReceiverUp();

    Group ("A receiver which is not there, and which comes back");
    TestReceiverDown();
    TestOrder();
    TestBoundedWork();
    TestSmallQueue();

    Group ("The program ends and starts again");
    TestRestart();
    TestShutdown();

    Group ("The socket sender: room, and the lines which were not sent");
    TestSenderRoom();
    TestLowWater();

    Group ("Limits and lines which are not valid");
    TestWalFull();
    TestInvalidLines();
    TestPoisonRecord();

    Group ("Without a WAL, and a WAL which cannot be opened");
    TestWithoutWal();
    TestWalCannotBeOpened();

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
