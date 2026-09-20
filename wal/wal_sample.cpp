
/* wal_sample.cpp - how to use SimpleWAL (simple_wal.hpp) in another program.

   Build and run:   cd wal && make && ./wal_sample        (or from the repository root: make wal_sample && ./wal/wal_sample)
   The build is in the makefile next to this file. It shows what a program needs to use the WAL: simple_wal.hpp, simple_wal.cpp, -pthread

   The program plays a small forwarder. Records ("log lines") are written to the WAL, and a "receiver" gets them. The receiver is
   down at first, so the records wait in the WAL. What it shows, in three steps:

     1. The records are kept while the receiver is down, also when the program ends
     2. After a restart Replay() delivers them, in the order they were written. Replay() is the way if the program has one place
        which sends. The function of the program decides for every record: true, it was delivered. False, try again later
     3. Peek() and Ack(): one record at a time, without a function. The way for a program with a loop of its own, which must
        not wait. The record is only acknowledged when it was delivered. Another thread appends at the same time

   Everything the program has to do: create the object, Init() it with a file name, Append() from any thread, and take the records
   out with Replay() or with Peek() and Ack(). The rest (files, locking, a crash in the middle, threads) is done by the class.
   The rules are in the header simple_wal.hpp.

   The exit code is 0 if everything happened as described, and 1 if not. So the sample also tells if the WAL still works. */

#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

#include <atomic>
#include <chrono>
#include <string>
#include <thread>
#include <vector>

#include "simple_wal.hpp"


/* The receiver: stands for whatever the program sends to (a network connection, a database ...). It takes a record or it does not */
static std::atomic<bool> g_bReceiverUp {false};
static std::atomic<int>  g_Sent {0};


static bool Send (const std::vector<uint8_t>& Record)
{
    if (false == g_bReceiverUp.load())
        return false;

    printf ("    sent: %.*s\n", static_cast<int> (Record.size()), reinterpret_cast<const char *> (Record.data()));
    g_Sent++;

    return true;
}


/* The WAL gives every message as one string, and the program decides where it goes. Without such a function the messages are written
   with printf to stdout, and SetLogTarget (SimpleWAL::LOG_STDERR) sends them to stderr instead */
static void LogFromWal (const char *pszMessage)
{
    printf ("    WAL: %s\n", pszMessage);
}


static void AppendLine (SimpleWAL& Wal, const std::string& Line)
{
    /* A record is any number of bytes. false: it was not stored (no room, no access) */
    if (false == Wal.Append (Line.data(), static_cast<uint32_t> (Line.size())))
        printf ("    the record \"%s\" could not be stored\n", Line.c_str());
}


int main (int argc, char *argv[])
{
    std::string Dir;
    bool        bOwnDir = false;

    if (argc > 1)
    {
        Dir = argv[1];
    }
    else
    {
        char szDir[] = "/tmp/wal_sample_XXXXXX";

        if (NULL == mkdtemp (szDir))
        {
            perror ("mkdtemp");
            return 1;
        }

        Dir     = szDir;
        bOwnDir = true;
    }

    std::string Path = Dir + "/sample.wal";
    int         Failed = 0;

    unlink (Path.c_str());
    unlink ((Path + ".commit").c_str());

    printf ("\n1. The receiver is down: the records stay in the WAL, also when the program ends\n\n");

    {
        SimpleWAL Wal;

        Wal.SetLogFunction (LogFromWal);

        if (false == Wal.Init (Path))
        {
            printf ("    the WAL cannot be opened\n");
            return 1;
        }

        for (int i = 1; i <= 5; i++)
            AppendLine (Wal, "log line " + std::to_string (i));

        Wal.Replay (Send);      // the function says false for every record: nothing is delivered, nothing is lost

        printf ("    pending: %s, size of the file: %llu bytes\n", Wal.IsReplayPending() ? "yes" : "no", static_cast<unsigned long long> (Wal.GetSize()));

        if (false == Wal.IsReplayPending())
            Failed++;
    }

    printf ("\n2. After a restart the receiver is up: Replay() delivers what is left, in the order it was written\n\n");

    g_bReceiverUp = true;

    {
        SimpleWAL Wal;

        Wal.SetLogFunction (LogFromWal);

        if (false == Wal.Init (Path))
            return 1;

        printf ("    pending after the restart: %s\n", Wal.IsReplayPending() ? "yes" : "no");

        Wal.Replay (Send);      // true for every record: they are delivered, and the WAL is empty afterwards

        printf ("    pending: %s\n", Wal.IsReplayPending() ? "yes" : "no");

        if (Wal.IsReplayPending() || (5 != g_Sent.load()))
            Failed++;
    }

    printf ("\n3. Peek() and Ack(): one record at a time, while another thread appends\n\n");

    g_Sent = 0;

    {
        SimpleWAL Wal;

        Wal.SetLogFunction (LogFromWal);
        Wal.SetMaxSize (1024 * 1024);           // optional: Append() says false if the file would get larger than this

        if (false == Wal.Init (Path))
            return 1;

        /* Any thread can append. This one stands for the part of the program which produces the records */
        std::thread Producer ([&Wal]
        {
            for (int i = 1; i <= 3; i++)
            {
                AppendLine (Wal, "event " + std::to_string (i));
                std::this_thread::sleep_for (std::chrono::milliseconds (50));
            }
        });

        /* The loop of the program. It never waits for the WAL: Peek() says false at once if there is nothing */
        auto Deadline = std::chrono::steady_clock::now() + std::chrono::seconds (10);

        while ( (g_Sent.load() < 3) && (std::chrono::steady_clock::now() < Deadline) )
        {
            std::vector<uint8_t> Record;

            if (false == Wal.Peek (Record))
            {
                std::this_thread::sleep_for (std::chrono::milliseconds (10));       // nothing to send now
                continue;
            }

            /* The record stays in the WAL until it is acknowledged. If the program ends between Send() and Ack(), or if
               Send() says false, it is read again: delivery is at least once, nothing is lost */
            if (Send (Record))
                Wal.Ack();
        }

        Producer.join();

        printf ("    pending: %s\n", Wal.IsReplayPending() ? "yes" : "no");

        if (Wal.IsReplayPending() || (3 != g_Sent.load()))
            Failed++;
    }

    if (bOwnDir)
        rmdir (Dir.c_str());

    printf ("\n%s\n\n", Failed ? "The sample did not work as described" : "The sample worked as described");

    return Failed ? 1 : 0;
}
