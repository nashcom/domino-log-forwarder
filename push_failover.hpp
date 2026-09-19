
#pragma once

#include <atomic>
#include <cstdint>
#include <ctime>
#include <functional>

#include "push_status.hpp"


/* Which of the two OTLP endpoints. The backup is optional. */
enum PushEndpoint
{
    PUSH_PRIMARY = 0,
    PUSH_BACKUP  = 1
};


/* Decides which endpoint a push request goes to. It does not send anything: the caller gives it a function which sends the
   request to one endpoint and tells what came out (PushAction, see push_status.hpp).

   Without a backup endpoint the request goes to the primary, and nothing else happens.

   With a backup endpoint:

   - The primary is used first. If the request is not delivered (PUSH_RETRY), the backup is tried with the same request.
     If that is delivered, the backup stays in use: the next requests go to the backup and do not wait for a failing primary.
   - While the backup is in use, the primary is tried again every FailbackSec seconds (a "probe", with the real request).
     If the primary delivers, it is used again.
   - If the backup fails while it is in use, the primary is tried at once, without waiting for the next probe.
   - PUSH_REJECTED (the receiver says that the data itself is bad) ends the try. The other endpoint would say the same.
   - If both endpoints fail, the result is PUSH_RETRY: the caller keeps the request in the WAL.

   The state is atomic, so the thread of the live push and the thread of the WAL replay can use one object. Only one of them
   makes the probe. Time is a parameter, so that tests do not have to wait. */
class PushFailover
{

public:

    /* Sends the request to one endpoint. bProbe is true for the try of the primary while the backup is in use. It can use a
       shorter timeout, because a primary which does not answer must not hold up the request for long */
    typedef std::function<PushAction (PushEndpoint Endpoint, bool bProbe)> Sender;

    PushFailover() : m_bHasBackup (false), m_FailbackSec (60), m_Active (PUSH_PRIMARY), m_tNextProbe (0), m_Failovers (0), m_Failbacks (0)
    {
        for (int e = 0; e < 2; e++)
        {
            for (int a = 0; a < 3; a++)
                m_Requests[e][a] = 0;
        }
    }

    void Configure (bool bHasBackup, time_t FailbackSec)
    {
        m_bHasBackup  = bHasBackup;
        m_FailbackSec = FailbackSec;
    }

    /* Sends the request. *pSwitchedTo is set to the endpoint which is in use from now on, if this call changed it (else -1) */
    PushAction Send (const Sender& SendTo, time_t Now, int *pSwitchedTo = NULL)
    {
        PushAction Action = PUSH_RETRY;

        if (pSwitchedTo)
            *pSwitchedTo = -1;

        if (false == m_bHasBackup)
            return Try (SendTo, PUSH_PRIMARY, false);

        if (PUSH_PRIMARY == m_Active.load())
        {
            Action = Try (SendTo, PUSH_PRIMARY, false);

            if (PUSH_RETRY != Action)
                return Action;

            Action = Try (SendTo, PUSH_BACKUP, false);

            if (PUSH_ACCEPTED == Action)
                SwitchTo (PUSH_BACKUP, Now, pSwitchedTo);

            return Action;
        }

        /* The backup is in use: try the primary again every FailbackSec seconds. compare_exchange lets only one thread do it */
        time_t tNext = m_tNextProbe.load();

        if ( (Now >= tNext) && m_tNextProbe.compare_exchange_strong (tNext, Now + m_FailbackSec) )
        {
            Action = Try (SendTo, PUSH_PRIMARY, true);

            if (PUSH_RETRY != Action)
            {
                if (PUSH_ACCEPTED == Action)
                    SwitchTo (PUSH_PRIMARY, Now, pSwitchedTo);

                return Action;
            }
        }

        Action = Try (SendTo, PUSH_BACKUP, false);

        if (PUSH_RETRY != Action)
            return Action;

        /* The backup failed too. The primary may be back: do not wait for the next probe */
        Action = Try (SendTo, PUSH_PRIMARY, false);

        if (PUSH_ACCEPTED == Action)
            SwitchTo (PUSH_PRIMARY, Now, pSwitchedTo);

        return Action;
    }

    bool         HasBackup()  const { return m_bHasBackup; }
    PushEndpoint GetActive()  const { return m_Active.load(); }
    time_t       GetFailbackSec() const { return m_FailbackSec; }

    /* Statistics: requests to an endpoint, by what came out, and the number of switches */
    std::int64_t GetRequests (PushEndpoint Endpoint, PushAction Action) const { return m_Requests[Endpoint][Action].load(); }
    std::int64_t GetFailovers() const { return m_Failovers.load(); }
    std::int64_t GetFailbacks() const { return m_Failbacks.load(); }

private:

    PushAction Try (const Sender& SendTo, PushEndpoint Endpoint, bool bProbe)
    {
        PushAction Action = SendTo (Endpoint, bProbe);

        m_Requests[Endpoint][Action]++;

        return Action;
    }

    void SwitchTo (PushEndpoint Endpoint, time_t Now, int *pSwitchedTo)
    {
        if (PUSH_BACKUP == Endpoint)
            m_tNextProbe = Now + m_FailbackSec;

        if (m_Active.exchange (Endpoint) == Endpoint)
            return;

        if (PUSH_BACKUP == Endpoint)
            m_Failovers++;
        else
            m_Failbacks++;

        if (pSwitchedTo)
            *pSwitchedTo = Endpoint;
    }

    bool                        m_bHasBackup;
    time_t                      m_FailbackSec;
    std::atomic<PushEndpoint>   m_Active;
    std::atomic<time_t>         m_tNextProbe;
    std::atomic<std::int64_t>   m_Failovers;
    std::atomic<std::int64_t>   m_Failbacks;
    std::atomic<std::int64_t>   m_Requests[2][3];
};
