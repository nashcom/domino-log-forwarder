/* domfwd_socket.hpp - Non-blocking line sender for the Domino Log Forwarder (domfwd pipe tool)

   Sends one line (a flat JSON record) per event to the forwarder over a UNIX socket (Linux) or
   a loopback TCP connection (Linux and Windows). No Domino API, no libcurl and no C++ standard
   library (STL) are used here: the Windows build links without the C++ runtime library.

   Design rules - this runs inside the servertask's main loop (AddInMain), which also has to keep
   up with the Domino event queue:

   * Nothing here ever blocks. connect() and send() are non-blocking. Pump() only does what can be
     done right now and returns.
   * Lines wait in a bounded queue (line count and bytes). When the queue is full, the newest line
     is dropped and counted. Older lines are kept.
   * A lost connection is detected (peer closed) and re-established every DOMFWD_SOCKET_RECONNECT_MS.
     A line which was only partly sent is sent again in full on the new connection.
   * Delivery is at-most-once per connection, without acknowledgements. Lines sent shortly before the
     forwarder stops can be lost. That is the same limit as any fire and forget log transport.
   * TCP only connects to loopback addresses, because the transport is unauthenticated and unencrypted.

   Target specification:  unix:/path/to/socket   (Linux only)
                          tcp:127.0.0.1:4319 or tcp:[::1]:4319
*/

#ifndef DOMFWD_SOCKET_HPP
#define DOMFWD_SOCKET_HPP

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>

#if defined (_WIN32)

    #include <winsock2.h>
    #include <ws2tcpip.h>

    typedef SOCKET DomfwdSocketHandle;

    #define DOMFWD_SOCK_INVALID        INVALID_SOCKET
    #define DOMFWD_SOCK_CLOSE(s)       closesocket (s)
    #define DOMFWD_SOCK_ERROR()        WSAGetLastError()
    #define DOMFWD_SOCK_WOULD_BLOCK(e) ((WSAEWOULDBLOCK == (e)) || (WSAEINPROGRESS == (e)))
    #define DOMFWD_SOCK_POLL(p, n, t)  WSAPoll ((p), (ULONG)(n), (t))
    #define DOMFWD_SOCK_POLLFD         WSAPOLLFD
    #define DOMFWD_SOCK_SEND_FLAGS     0

#else

    #include <errno.h>
    #include <fcntl.h>
    #include <poll.h>
    #include <time.h>
    #include <arpa/inet.h>
    #include <netinet/in.h>
    #include <sys/socket.h>
    #include <sys/types.h>
    #include <sys/un.h>
    #include <unistd.h>

    typedef int DomfwdSocketHandle;

    #define DOMFWD_SOCK_INVALID        (-1)
    #define DOMFWD_SOCK_CLOSE(s)       close (s)
    #define DOMFWD_SOCK_ERROR()        errno
    #define DOMFWD_SOCK_WOULD_BLOCK(e) ((EAGAIN == (e)) || (EWOULDBLOCK == (e)) || (EINPROGRESS == (e)) || (EINTR == (e)))
    #define DOMFWD_SOCK_POLL(p, n, t)  poll ((p), (nfds_t)(n), (t))
    #define DOMFWD_SOCK_POLLFD         struct pollfd

    #if defined (MSG_NOSIGNAL)
        #define DOMFWD_SOCK_SEND_FLAGS MSG_NOSIGNAL
    #else
        #define DOMFWD_SOCK_SEND_FLAGS 0
    #endif

#endif

/* Can be overridden at compile time (the tests use shorter values) */
#ifndef DOMFWD_SOCKET_RECONNECT_MS
    #define DOMFWD_SOCKET_RECONNECT_MS   5000
#endif

#ifndef DOMFWD_SOCKET_CONNECT_MAX_MS
    #define DOMFWD_SOCKET_CONNECT_MAX_MS 5000
#endif

#ifndef DOMFWD_SOCKET_DROP_LOG_MS
    #define DOMFWD_SOCKET_DROP_LOG_MS    60000
#endif

#define DOMFWD_SOCKET_SEND_CHUNK        65536
#define DOMFWD_SOCKET_SPEC_MAX          512


class DomfwdSocketSender
{

public:

    typedef void (*LogFunc) (const char *pszMessage);

    DomfwdSocketSender() {}

    ~DomfwdSocketSender()
    {
        Close();
    }

    /* Parses the target and prepares the sender. Does not connect yet. Returns false with an error text. */
    bool Configure (const char *pszSpec, LogFunc pLog, size_t MaxLines, size_t MaxBytes, char *retszError, size_t ErrorLen)
    {
        bool bOk = false;

        if ( (NULL == retszError) || (0 == ErrorLen) )
            return false;

        retszError[0] = '\0';

        if ( (NULL == pszSpec) || (strlen (pszSpec) >= DOMFWD_SOCKET_SPEC_MAX) )
        {
            snprintf (retszError, ErrorLen, "target is missing or too long");
            return false;
        }

        if ( (0 == MaxLines) || (MaxBytes < 1024) )
        {
            snprintf (retszError, ErrorLen, "invalid queue limits");
            return false;
        }

        m_pLog     = pLog;
        m_MaxLines = MaxLines;
        m_MaxBytes = MaxBytes;

        if (0 == strncmp (pszSpec, "unix:", 5))
            bOk = ConfigureUnix (pszSpec + 5, retszError, ErrorLen);
        else if (0 == strncmp (pszSpec, "tcp:", 4))
            bOk = ConfigureTcp (pszSpec + 4, retszError, ErrorLen);
        else
            snprintf (retszError, ErrorLen, "target must start with unix: or tcp:");

        if (false == bOk)
            return false;

        if (false == PlatformInit (retszError, ErrorLen))
            return false;

        m_pBuffer = (char *) malloc (m_MaxBytes);

        if (NULL == m_pBuffer)
        {
            snprintf (retszError, ErrorLen, "out of memory");
            PlatformCleanup();
            m_bPlatformInit = false;
            return false;
        }

        snprintf (m_szSpec, sizeof (m_szSpec), "%s", pszSpec);

        m_bConfigured = true;
        m_NextAttempt = NowMs();

        return true;
    }

    bool IsConfigured() const
    {
        return m_bConfigured;
    }

    bool IsConnected() const
    {
        return (State_Connected == m_State);
    }

    /* Queues one line. The line must not contain a new line, a new line is added. Returns false if the line was dropped. */
    bool Enqueue (const char *pszLine, size_t Len)
    {
        if (false == m_bConfigured)
            return false;

        if ( (NULL == pszLine) || (0 == Len) || (NULL != memchr (pszLine, '\n', Len)) )
        {
            m_Rejected++;
            return false;
        }

        if ( (m_Lines >= m_MaxLines) || (((m_Tail - m_Head) + Len + 1) > m_MaxBytes) )
        {
            m_Dropped++;
            LogDrop();
            return false;
        }

        /* No room left at the end of the buffer: move the pending data to the start */
        if ((m_Tail + Len + 1) > m_MaxBytes)
        {
            memmove (m_pBuffer, m_pBuffer + m_Head, m_Tail - m_Head);
            m_SendPos -= m_Head;
            m_Tail    -= m_Head;
            m_Head     = 0;
        }

        memcpy (m_pBuffer + m_Tail, pszLine, Len);
        m_pBuffer[m_Tail + Len] = '\n';

        m_Tail += Len + 1;
        m_Lines++;

        return true;
    }

    /* Does what can be done without waiting: connect, detect a closed connection, send queued lines */
    void Pump()
    {
        if (false == m_bConfigured)
            return;

        if (State_Disconnected == m_State)
        {
            if (NowMs() >= m_NextAttempt)
                StartConnect();
        }

        if (State_Connecting == m_State)
            CheckConnect();

        if (State_Connected == m_State)
        {
            CheckPeerClosed();

            if (State_Connected == m_State)
                SendQueued();
        }
    }

    /* Tries to send the remaining lines for up to TimeoutMs. Used at shutdown. Returns true if the queue is empty. */
    bool Flush (unsigned TimeoutMs)
    {
        uint64_t End = NowMs() + TimeoutMs;

        while (true)
        {
            Pump();

            if (0 == m_Lines)
                return true;

            if (NowMs() >= End)
                return false;

            SleepShort();
        }
    }

    void Close()
    {
        CloseSocket();

        m_State = State_Disconnected;

        if (m_pBuffer)
        {
            free (m_pBuffer);
            m_pBuffer = NULL;
        }

        m_Head = m_SendPos = m_Tail = 0;
        m_Lines = 0;

        if (m_bPlatformInit)
        {
            PlatformCleanup();
            m_bPlatformInit = false;
        }

        m_bConfigured = false;
    }

    size_t   GetQueuedLines() const { return m_Lines; }
    uint64_t GetSent()        const { return m_Sent; }
    uint64_t GetDropped()     const { return m_Dropped; }
    uint64_t GetRejected()    const { return m_Rejected; }
    uint64_t GetConnects()    const { return m_Connects; }
    const char *GetSpec()     const { return m_szSpec; }


private:

    DomfwdSocketSender (const DomfwdSocketSender&);
    DomfwdSocketSender& operator= (const DomfwdSocketSender&);

    enum State
    {
        State_Disconnected = 0,
        State_Connecting,
        State_Connected
    };

    bool                 m_bConfigured   = false;
    bool                 m_bPlatformInit = false;
    State                m_State         = State_Disconnected;
    DomfwdSocketHandle   m_Sock          = DOMFWD_SOCK_INVALID;
    LogFunc              m_pLog          = NULL;
    char                 m_szSpec[DOMFWD_SOCKET_SPEC_MAX] = {0};

    int                  m_Family        = 0;
    struct sockaddr_storage m_Addr;
    int                  m_AddrLen       = 0;

    /* Queue: complete lines, each ending with a new line, in one buffer.
       m_Head: start of the oldest line that is not completely sent yet
       m_SendPos: next byte to send (m_Head <= m_SendPos <= m_Tail)
       m_Tail: end of the queued data */
    char                *m_pBuffer       = NULL;
    size_t               m_Head          = 0;
    size_t               m_SendPos       = 0;
    size_t               m_Tail          = 0;
    size_t               m_Lines         = 0;
    size_t               m_MaxLines      = 0;
    size_t               m_MaxBytes      = 0;

    uint64_t             m_NextAttempt   = 0;
    uint64_t             m_ConnectStart  = 0;
    uint64_t             m_LastDropLog   = 0;
    bool                 m_bLoggedDown   = false;

    uint64_t             m_Sent          = 0;
    uint64_t             m_Dropped       = 0;
    uint64_t             m_Rejected      = 0;
    uint64_t             m_Connects      = 0;


    static uint64_t NowMs()
    {
#if defined (_WIN32)
        return (uint64_t) GetTickCount64();
#else
        struct timespec ts;

        clock_gettime (CLOCK_MONOTONIC, &ts);
        return ((uint64_t) ts.tv_sec * 1000) + ((uint64_t) ts.tv_nsec / 1000000);
#endif
    }

    static void SleepShort()
    {
#if defined (_WIN32)
        Sleep (10);
#else
        struct timespec ts;

        ts.tv_sec  = 0;
        ts.tv_nsec = 10 * 1000000L;
        nanosleep (&ts, NULL);
#endif
    }

    static void ErrorText (int Err, char *retszText, size_t TextLen)
    {
#if defined (_WIN32)
        snprintf (retszText, TextLen, "error %d", Err);
#else
        snprintf (retszText, TextLen, "%s", strerror (Err));
#endif
    }

    void Log (const char *pszFormat, ...)
    {
        char szMessage[512] = {0};
        va_list Args;

        if (NULL == m_pLog)
            return;

        va_start (Args, pszFormat);
        vsnprintf (szMessage, sizeof (szMessage), pszFormat, Args);
        va_end (Args);

        m_pLog (szMessage);
    }

    /* At most one message per minute, no matter how many lines are dropped */
    void LogDrop()
    {
        uint64_t Now = NowMs();

        if ( (0 == m_LastDropLog) || ((Now - m_LastDropLog) >= DOMFWD_SOCKET_DROP_LOG_MS) )
        {
            m_LastDropLog = Now;
            Log ("Queue is full, dropping lines (dropped so far: %llu)", (unsigned long long) m_Dropped);
        }
    }

    bool PlatformInit (char *retszError, size_t ErrorLen)
    {
#if defined (_WIN32)
        WSADATA WsaData;

        if (0 != WSAStartup (MAKEWORD (2, 2), &WsaData))
        {
            snprintf (retszError, ErrorLen, "WSAStartup failed");
            return false;
        }
#else
        (void) retszError;
        (void) ErrorLen;
#endif
        m_bPlatformInit = true;
        return true;
    }

    void PlatformCleanup()
    {
#if defined (_WIN32)
        WSACleanup();
#endif
    }

    bool ConfigureUnix (const char *pszPath, char *retszError, size_t ErrorLen)
    {
#if defined (_WIN32)
        (void) pszPath;
        snprintf (retszError, ErrorLen, "unix: targets are not supported on Windows, use tcp:");
        return false;
#else
        struct sockaddr_un *pAddr = (struct sockaddr_un *) &m_Addr;

        if ('\0' == *pszPath)
        {
            snprintf (retszError, ErrorLen, "unix: needs a socket path");
            return false;
        }

        if (strlen (pszPath) >= sizeof (pAddr->sun_path))
        {
            snprintf (retszError, ErrorLen, "unix socket path is too long");
            return false;
        }

        memset (&m_Addr, 0, sizeof (m_Addr));
        pAddr->sun_family = AF_UNIX;
        snprintf (pAddr->sun_path, sizeof (pAddr->sun_path), "%s", pszPath);

        m_Family  = AF_UNIX;
        m_AddrLen = (int) sizeof (struct sockaddr_un);

        return true;
#endif
    }

    bool ConfigureTcp (const char *pszSpec, char *retszError, size_t ErrorLen)
    {
        char szHost[DOMFWD_SOCKET_SPEC_MAX] = {0};
        struct in_addr  Addr4;
        struct in6_addr Addr6;
        char *pEnd = NULL;
        unsigned long Port = 0;
        size_t HostLen = 0;

        const char *pColon = strrchr (pszSpec, ':');

        if (NULL == pColon)
        {
            snprintf (retszError, ErrorLen, "tcp: needs address:port");
            return false;
        }

        HostLen = (size_t) (pColon - pszSpec);

        if (HostLen >= sizeof (szHost))
        {
            snprintf (retszError, ErrorLen, "tcp: address is too long");
            return false;
        }

        memcpy (szHost, pszSpec, HostLen);
        szHost[HostLen] = '\0';

        /* [::1] -> ::1 */
        if ( (HostLen >= 2) && ('[' == szHost[0]) && (']' == szHost[HostLen - 1]) )
        {
            memmove (szHost, szHost + 1, HostLen - 2);
            szHost[HostLen - 2] = '\0';
        }

        Port = strtoul (pColon + 1, &pEnd, 10);

        if ( ('\0' == pColon[1]) || (NULL == pEnd) || ('\0' != *pEnd) || (0 == Port) || (Port > 65535) )
        {
            snprintf (retszError, ErrorLen, "invalid tcp port");
            return false;
        }

        memset (&m_Addr, 0, sizeof (m_Addr));

        if (1 == inet_pton (AF_INET, szHost, &Addr4))
        {
            struct sockaddr_in *pAddr = (struct sockaddr_in *) &m_Addr;

            if (127 != (ntohl (Addr4.s_addr) >> 24))
            {
                snprintf (retszError, ErrorLen, "tcp: only loopback addresses (127.0.0.0/8 or ::1) are allowed");
                return false;
            }

            pAddr->sin_family = AF_INET;
            pAddr->sin_port   = htons ((unsigned short) Port);
            pAddr->sin_addr   = Addr4;

            m_Family  = AF_INET;
            m_AddrLen = (int) sizeof (struct sockaddr_in);
        }
        else if (1 == inet_pton (AF_INET6, szHost, &Addr6))
        {
            struct sockaddr_in6 *pAddr = (struct sockaddr_in6 *) &m_Addr;

            if (0 == IN6_IS_ADDR_LOOPBACK (&Addr6))
            {
                snprintf (retszError, ErrorLen, "tcp: only loopback addresses (127.0.0.0/8 or ::1) are allowed");
                return false;
            }

            pAddr->sin6_family = AF_INET6;
            pAddr->sin6_port   = htons ((unsigned short) Port);
            pAddr->sin6_addr   = Addr6;

            m_Family  = AF_INET6;
            m_AddrLen = (int) sizeof (struct sockaddr_in6);
        }
        else
        {
            snprintf (retszError, ErrorLen, "tcp: address must be an IP address");
            return false;
        }

        return true;
    }

    bool SetNonBlocking (DomfwdSocketHandle Sock)
    {
#if defined (_WIN32)
        u_long Mode = 1;
        return (0 == ioctlsocket (Sock, FIONBIO, &Mode));
#else
        int Flags = fcntl (Sock, F_GETFL, 0);

        if (Flags < 0)
            return false;

        if (0 != fcntl (Sock, F_SETFL, Flags | O_NONBLOCK))
            return false;

        fcntl (Sock, F_SETFD, FD_CLOEXEC);
        return true;
#endif
    }

    void CloseSocket()
    {
        if (DOMFWD_SOCK_INVALID != m_Sock)
        {
            DOMFWD_SOCK_CLOSE (m_Sock);
            m_Sock = DOMFWD_SOCK_INVALID;
        }
    }

    /* The connection is gone. A line that was partly sent starts again from its beginning on the next connection */
    void Disconnect (const char *pszReason)
    {
        bool bWasConnected = (State_Connected == m_State);

        CloseSocket();

        m_State       = State_Disconnected;
        m_SendPos     = m_Head;
        m_NextAttempt = NowMs() + DOMFWD_SOCKET_RECONNECT_MS;

        if (bWasConnected)
        {
            Log ("Connection to %s lost: %s", m_szSpec, pszReason);
            m_bLoggedDown = true;
        }
        else if (false == m_bLoggedDown)
        {
            Log ("Cannot connect to %s: %s. Retrying every %d ms", m_szSpec, pszReason, (int) DOMFWD_SOCKET_RECONNECT_MS);
            m_bLoggedDown = true;
        }
    }

    void DisconnectError (int Err)
    {
        char szText[128] = {0};

        ErrorText (Err, szText, sizeof (szText));
        Disconnect (szText);
    }

    void Connected()
    {
        m_State = State_Connected;
        m_Connects++;
        m_bLoggedDown = false;

        Log ("Connected to %s", m_szSpec);
    }

    void StartConnect()
    {
        int rc  = 0;
        int Err = 0;

        m_Sock = socket (m_Family, SOCK_STREAM, 0);

        if (DOMFWD_SOCK_INVALID == m_Sock)
        {
            Disconnect ("cannot create socket");
            return;
        }

        if (false == SetNonBlocking (m_Sock))
        {
            Disconnect ("cannot set non-blocking mode");
            return;
        }

        rc = connect (m_Sock, (struct sockaddr *) &m_Addr, (socklen_t) m_AddrLen);

        if (0 == rc)
        {
            Connected();
            return;
        }

        Err = DOMFWD_SOCK_ERROR();

        if (DOMFWD_SOCK_WOULD_BLOCK (Err))
        {
            m_State        = State_Connecting;
            m_ConnectStart = NowMs();
            return;
        }

        DisconnectError (Err);
    }

    void CheckConnect()
    {
        DOMFWD_SOCK_POLLFD Pfd;
        int rc = 0;
        int SoError = 0;
        socklen_t SoLen = (socklen_t) sizeof (SoError);

        memset (&Pfd, 0, sizeof (Pfd));
        Pfd.fd     = m_Sock;
        Pfd.events = POLLOUT;

        rc = DOMFWD_SOCK_POLL (&Pfd, 1, 0);

        if (rc < 0)
        {
            Disconnect ("poll failed");
            return;
        }

        if (0 == rc)
        {
            if ((NowMs() - m_ConnectStart) >= DOMFWD_SOCKET_CONNECT_MAX_MS)
                Disconnect ("connect timeout");

            return;
        }

        if (0 != getsockopt (m_Sock, SOL_SOCKET, SO_ERROR, (char *) &SoError, &SoLen))
        {
            Disconnect ("getsockopt failed");
            return;
        }

        if (0 != SoError)
        {
            DisconnectError (SoError);
            return;
        }

        Connected();
    }

    /* The forwarder never sends data. A readable socket means the peer closed the connection (or an error) */
    void CheckPeerClosed()
    {
        DOMFWD_SOCK_POLLFD Pfd;
        char szBuffer[256];
        int  rc = 0;
        int  n  = 0;

        memset (&Pfd, 0, sizeof (Pfd));
        Pfd.fd     = m_Sock;
        Pfd.events = POLLIN;

        rc = DOMFWD_SOCK_POLL (&Pfd, 1, 0);

        if (rc <= 0)
            return;

        n = (int) recv (m_Sock, szBuffer, (int) sizeof (szBuffer), 0);

        if (0 == n)
        {
            Disconnect ("closed by the forwarder");
        }
        else if (n < 0)
        {
            if (false == DOMFWD_SOCK_WOULD_BLOCK (DOMFWD_SOCK_ERROR()))
                Disconnect ("connection error");
        }
    }

    /* Moves m_Head behind all lines that have been sent completely */
    void CompleteSentLines()
    {
        const char *pNewLine = NULL;

        while ( (m_Head < m_SendPos) && (NULL != (pNewLine = (const char *) memchr (m_pBuffer + m_Head, '\n', m_SendPos - m_Head))) )
        {
            m_Head = (size_t) (pNewLine - m_pBuffer) + 1;
            m_Sent++;
            m_Lines--;
        }

        if (m_Head == m_Tail)
            m_Head = m_SendPos = m_Tail = 0;
    }

    void SendQueued()
    {
        while (m_SendPos < m_Tail)
        {
            size_t Chunk = m_Tail - m_SendPos;
            int    n     = 0;

            if (Chunk > DOMFWD_SOCKET_SEND_CHUNK)
                Chunk = DOMFWD_SOCKET_SEND_CHUNK;

            n = (int) send (m_Sock, m_pBuffer + m_SendPos, (int) Chunk, DOMFWD_SOCK_SEND_FLAGS);

            if (n > 0)
            {
                m_SendPos += (size_t) n;
                CompleteSentLines();
                continue;
            }

            if ( (0 == n) || DOMFWD_SOCK_WOULD_BLOCK (DOMFWD_SOCK_ERROR()) )
                return;

            DisconnectError (DOMFWD_SOCK_ERROR());
            return;
        }
    }
};

#endif /* DOMFWD_SOCKET_HPP */
