/* domfwd_durable.hpp - a line sender which keeps the lines in a WAL while the forwarder is not there

   The socket sender of domfwd (domfwd_socket.hpp) keeps lines in a queue in memory. When the forwarder (otelfwd) is not reachable
   and the queue is full, lines are dropped, and lines which are still in the queue when the program ends are lost. This class
   puts the WAL (../wal) behind the sender, so that lines wait on disk instead:

   * Send() is the fast path when the forwarder is there: the line goes into the queue of the sender, as before. If the sender is not
     connected, or lines wait in the WAL, or the queue is full, the line is appended to the WAL. While lines wait in the WAL a new line
     never overtakes them: it goes to the WAL too. So the lines arrive in the order of the calls.
   * Pump() is called in the main loop. When the sender is connected, it moves lines from the WAL into the queue, a few at a time and
     never more than DOMFWD_DURABLE_DRAIN_MAX in one call, and only while the queue is short (DOMFWD_DURABLE_LOW_WATER lines).
     The main loop is never held up: nothing here waits.
   * Flush() at the end tries to send what is in the queue. What cannot be sent goes to the WAL. Lines which are in the WAL stay there
     and are sent at the next start. (The lines of the queue are older than those in the WAL, and they are appended behind them: at the next
     start they arrive after them.)
   * A line which is invalid (empty, or with a new line inside) is refused by Send() and never stored. If a WAL holds such a record
     nevertheless, it is dropped and counted as rejected: it must not block the lines behind it.

   What it protects, and what it does not. The forwarder can be down or restarted, and this program or the server can stop, and the
   lines are still there. The transport has no acknowledgement, so lines which the sender has written into the socket and which the
   forwarder never read are lost. That is a few lines around a connection which breaks, not the lines of an outage.

   Without a WAL (no path, or on Windows, or the WAL cannot be opened) it is the plain sender. Not thread safe, like the sender: one
   thread, the main loop. The WAL itself does not need that, but the rules above do. */

#ifndef DOMFWD_DURABLE_HPP
#define DOMFWD_DURABLE_HPP

#include "domfwd_socket.hpp"

#if !defined (_WIN32)

    #include <memory>
    #include <vector>

    #include "simple_wal.hpp"

#endif

/* Can be overridden at compile time (the tests use smaller values) */
#ifndef DOMFWD_DURABLE_LOW_WATER
    #define DOMFWD_DURABLE_LOW_WATER   16
#endif

#ifndef DOMFWD_DURABLE_DRAIN_MAX
    #define DOMFWD_DURABLE_DRAIN_MAX   200
#endif


class DomfwdDurableSender
{

public:

    typedef DomfwdSocketSender::LogFunc LogFunc;

    DomfwdDurableSender() {}

    ~DomfwdDurableSender()
    {
        Close();
    }

    /* Like DomfwdSocketSender::Configure. pszWalPath: the file of the WAL, NULL or empty for none. WalMaxBytes: the largest size of
       the WAL, 0 for no limit. Returns false with an error text if the sender cannot be configured. A WAL which cannot be opened is
       no reason to fail: it is reported with the log function, and the sender works without it */
    bool Configure (const char *pszSpec, LogFunc pLog, size_t MaxLines, size_t MaxBytes,
                    const char *pszWalPath, uint64_t WalMaxBytes, char *retszError, size_t ErrorLen)
    {
        if (false == m_Sender.Configure (pszSpec, pLog, MaxLines, MaxBytes, retszError, ErrorLen))
            return false;

        m_pLog = pLog;

        if ( (NULL != pszWalPath) && ('\0' != *pszWalPath) )
            OpenWal (pszWalPath, WalMaxBytes);

        return true;
    }

    bool IsConfigured() const
    {
        return m_Sender.IsConfigured();
    }

    bool IsConnected() const
    {
        return m_Sender.IsConnected();
    }

    bool HasWal() const
    {
#if !defined (_WIN32)
        return (NULL != m_pWal.get());
#else
        return false;
#endif
    }

    /* Takes one line. Never waits. Returns false if the line was not kept: it is invalid, or there is no room in the queue and none in
       the WAL. The line must not contain a new line, a new line is added */
    bool Send (const char *pszLine, size_t Len)
    {
#if !defined (_WIN32)
        if ( m_pWal && IsValidLine (pszLine, Len) )
        {
            /* The forwarder is there, nothing waits in the WAL, and there is room: the fast path, as without a WAL */
            if ( m_Sender.IsConnected() && (false == m_pWal->IsReplayPending()) && m_Sender.HasRoom (Len) )
                return m_Sender.Enqueue (pszLine, Len);

            return Spill (pszLine, Len);
        }
#endif
        return m_Sender.Enqueue (pszLine, Len);
    }

    /* The main loop: connect, send, and move lines from the WAL to the sender. Never waits */
    void Pump()
    {
        m_Sender.Pump();

#if !defined (_WIN32)
        if (m_pWal)
            Drain();
#endif
    }

    /* The end of the program: tries to send the queue for up to TimeoutMs. What cannot be sent goes to the WAL. Returns true if
       nothing is left in memory: it was sent, or it is in the WAL now. Lines in the WAL stay there for the next start */
    bool Flush (unsigned TimeoutMs)
    {
        if (m_Sender.Flush (TimeoutMs))
            return true;

#if !defined (_WIN32)
        if (m_pWal)
        {
            const char *pLine = NULL;
            size_t      Len   = 0;

            while (m_Sender.PeekUnsent (&pLine, &Len))
            {
                /* The WAL is full or cannot be written: this line and the ones behind it stay in memory, and are lost */
                if (false == m_pWal->Append (pLine, static_cast<uint32_t> (Len)))
                {
                    m_WalRefused++;
                    break;
                }

                m_Sender.DropUnsent();
                m_Spilled++;
            }

            return (0 == m_Sender.GetQueuedLines());
        }
#endif
        return false;
    }

    /* Closes the sender and the WAL. A WAL which is empty leaves no files behind */
    void Close()
    {
        m_Sender.Close();

#if !defined (_WIN32)
        m_pWal.reset();
#endif
    }

    size_t   GetQueuedLines() const { return m_Sender.GetQueuedLines(); }
    uint64_t GetSent()        const { return m_Sender.GetSent(); }
    uint64_t GetRejected()    const { return m_Sender.GetRejected(); }
    uint64_t GetConnects()    const { return m_Sender.GetConnects(); }
    const char *GetSpec()     const { return m_Sender.GetSpec(); }

    /* Lines which were lost: the queue was full and so was the WAL (or there is none) */
    uint64_t GetDropped()     const { return m_Sender.GetDropped() + m_WalRefused; }

    uint64_t GetSpilled()     const { return m_Spilled; }       // lines which were written to the WAL
    uint64_t GetDrained()     const { return m_Drained; }       // lines which were moved from the WAL to the sender
    uint64_t GetWalRefused()  const { return m_WalRefused; }    // lines which the WAL did not take (full, or a failure)

    /* The size of the WAL file in bytes */
    uint64_t GetWalSize()
    {
#if !defined (_WIN32)
        if (m_pWal)
            return m_pWal->GetSize();
#endif
        return 0;
    }


private:

    DomfwdDurableSender (const DomfwdDurableSender&);
    DomfwdDurableSender& operator= (const DomfwdDurableSender&);

    DomfwdSocketSender m_Sender;
    LogFunc            m_pLog       = NULL;
    uint64_t           m_Spilled    = 0;
    uint64_t           m_Drained    = 0;
    uint64_t           m_WalRefused = 0;

#if !defined (_WIN32)

    std::unique_ptr<SimpleWAL> m_pWal;

#endif

    /* What the sender accepts: not empty, no new line. A line like that must not get into the WAL */
    static bool IsValidLine (const char *pszLine, size_t Len)
    {
        return (NULL != pszLine) && (Len > 0) && (NULL == memchr (pszLine, '\n', Len));
    }

    void Log (const char *pszMessage)
    {
        if (m_pLog)
            m_pLog (pszMessage);
    }

    void OpenWal (const char *pszWalPath, uint64_t WalMaxBytes)
    {
#if !defined (_WIN32)

        std::unique_ptr<SimpleWAL> pWal (new SimpleWAL);
        LogFunc pLog = m_pLog;

        /* The messages of the WAL are lines like "[Error] WAL: ..." and go to the same log function as those of the sender */
        pWal->SetLogFunction ([pLog] (const char *pszMessage) { if (pLog) pLog (pszMessage); });
        pWal->SetMaxSize (WalMaxBytes);

        if (false == pWal->Init (pszWalPath))
        {
            Log ("The WAL cannot be opened. Lines are kept in memory only");
            return;
        }

        m_pWal = std::move (pWal);

#else

        (void) pszWalPath;
        (void) WalMaxBytes;

        Log ("A WAL is not available on Windows. Lines are kept in memory only");

#endif
    }

#if !defined (_WIN32)

    /* Appends a line to the WAL */
    bool Spill (const char *pszLine, size_t Len)
    {
        if ( (Len <= 0xFFFFFFFFu) && m_pWal->Append (pszLine, static_cast<uint32_t> (Len)) )
        {
            m_Spilled++;
            return true;
        }

        m_WalRefused++;
        return false;
    }

    /* Moves lines from the WAL to the queue of the sender. A line which is in the queue is not in the WAL any more, so the queue
       is kept short: after a crash only these lines are lost. At most DOMFWD_DURABLE_DRAIN_MAX lines for one call */
    void Drain()
    {
        unsigned Moved = 0;

        if ( (false == m_Sender.IsConnected()) || (false == m_pWal->IsReplayPending()) )
            return;

        while (Moved < DOMFWD_DURABLE_DRAIN_MAX)
        {
            while ( (Moved < DOMFWD_DURABLE_DRAIN_MAX) && (m_Sender.GetQueuedLines() < DOMFWD_DURABLE_LOW_WATER) )
            {
                std::vector<uint8_t> Record;

                /* Nothing more, or the WAL cannot be read now: the next call tries again */
                if (false == m_pWal->Peek (Record))
                {
                    m_Sender.Pump();
                    return;
                }

                const char *pLine  = reinterpret_cast<const char *> (Record.data());
                bool        bValid = IsValidLine (pLine, Record.size());

                /* No room: the record stays in the WAL. (An invalid record needs none: the sender refuses it, and counts it) */
                if ( bValid && (false == m_Sender.HasRoom (Record.size())) )
                {
                    m_Sender.Pump();
                    return;
                }

                /* First the ack, then the queue: if the ack fails the record is still in the WAL and is not in the queue as well.
                   Otherwise a failing ack would send the same line again with every call. The room was checked above */
                if (false == m_pWal->Ack())
                {
                    m_Sender.Pump();
                    return;
                }

                m_Sender.Enqueue (pLine, Record.size());
                m_Drained++;
                Moved++;
            }

            m_Sender.Pump();

            /* The socket is full for now, or the connection is gone */
            if ( (m_Sender.GetQueuedLines() >= DOMFWD_DURABLE_LOW_WATER) || (false == m_Sender.IsConnected()) )
                return;
        }
    }

#endif
};

#endif /* DOMFWD_DURABLE_HPP */
