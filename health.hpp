/* health.hpp - one health state for alerting, shared by otelfwd and domfwd

   The state is a number, so an alert can use a simple rule:

     0 = OK      (green)
     1 = WARNING (yellow)   alert: health > 0
     2 = ERROR   (red)      alert: health == 2

   The worst of these rules wins. Otherwise the state is OK, also in the first minutes of an outage: a restart of the receiver
   must not raise an alert.

     * WAL fill:       WARNING at 25% of the size limit, ERROR at 50%. (Without a limit there is no fill to measure.)
     * Duration:       the receiver is not reachable (domfwd: not connected, otelfwd: no push accepted) for more than 15 minutes
                       is WARNING, for more than 30 minutes is ERROR.
     * Lost data:      data was dropped in the last 10 minutes (the WAL was full or refused it) is ERROR. Data which was rejected
                       for good (invalid line, refused by the receiver as bad data) in the last 10 minutes is WARNING.
     * WAL not usable: a WAL was wanted and could not be opened is ERROR: nothing is kept while the receiver is not there.

   The program calls Update() now and then (the metrics interval is fine: the limits are in minutes) with what it knows, and
   writes GetState() as its health metric. A change of the state is logged once with the reason. Not thread safe: one thread
   calls Update() and GetState(). Only the standard library is used, it also builds on Windows. */

#ifndef HEALTH_HPP
#define HEALTH_HPP

#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <string>

#define HEALTH_OK        0
#define HEALTH_WARNING   1
#define HEALTH_ERROR     2

#ifndef HEALTH_WARN_PERCENT
    #define HEALTH_WARN_PERCENT       25
#endif

#ifndef HEALTH_ERROR_PERCENT
    #define HEALTH_ERROR_PERCENT      50
#endif

#ifndef HEALTH_WARN_DOWN_SEC
    #define HEALTH_WARN_DOWN_SEC      (15 * 60)
#endif

#ifndef HEALTH_ERROR_DOWN_SEC
    #define HEALTH_ERROR_DOWN_SEC     (30 * 60)
#endif

#ifndef HEALTH_LOST_WINDOW_SEC
    #define HEALTH_LOST_WINDOW_SEC    (10 * 60)
#endif


/* What the program knows at the moment. The counters count from the start of the program */
struct HealthInput
{
    bool     bUnreachable = false;  // the receiver is not reachable now: not connected, or the last pushes failed
    bool     bWalFailed   = false;  // a WAL was wanted and could not be opened
    uint64_t WalBytes     = 0;      // the size of the WAL now
    uint64_t WalMaxBytes  = 0;      // the size limit of the WAL, 0 for no limit
    uint64_t Dropped      = 0;      // counter: data which was lost (the WAL was full or refused it)
    uint64_t Rejected     = 0;      // counter: data which was rejected for good (invalid line, refused as bad data)
};


class HealthMonitor
{

public:

    typedef void (*LogFunc) (const char *pszMessage);

    /* Takes the input of now, sets the state, and logs a change of the state. pLog can be NULL. Returns the state */
    int Update (time_t tNow, const HealthInput& Input, LogFunc pLog)
    {
        int         State   = HEALTH_OK;
        std::string Reason;

        /* How long the receiver was not reachable: the time of the first Update() which saw it */
        if (Input.bUnreachable)
        {
            if (0 == m_tUnreachableSince)
                m_tUnreachableSince = tNow;
        }
        else
        {
            m_tUnreachableSince = 0;
        }

        /* Data which was lost or rejected since the last Update(): the time is kept for the window */
        if (Input.Dropped > m_LastDropped)
            m_tLastDropped = tNow;

        if (Input.Rejected > m_LastRejected)
            m_tLastRejected = tNow;

        m_LastDropped  = Input.Dropped;
        m_LastRejected = Input.Rejected;

        if (Input.bWalFailed)
            Raise (State, Reason, HEALTH_ERROR, "the WAL could not be opened, nothing is kept while the receiver is not reachable");

        if ( (0 != m_tLastDropped) && ((tNow - m_tLastDropped) <= HEALTH_LOST_WINDOW_SEC) )
            Raise (State, Reason, HEALTH_ERROR, "data was dropped in the last 10 minutes");

        if ( (0 != m_tLastRejected) && ((tNow - m_tLastRejected) <= HEALTH_LOST_WINDOW_SEC) )
            Raise (State, Reason, HEALTH_WARNING, "data was rejected in the last 10 minutes");

        if (0 != m_tUnreachableSince)
        {
            time_t Down = tNow - m_tUnreachableSince;

            if (Down > HEALTH_ERROR_DOWN_SEC)
                Raise (State, Reason, HEALTH_ERROR, "the receiver is not reachable for more than 30 minutes");
            else if (Down > HEALTH_WARN_DOWN_SEC)
                Raise (State, Reason, HEALTH_WARNING, "the receiver is not reachable for more than 15 minutes");
        }

        /* Without a limit there is no fill to measure */
        if (Input.WalMaxBytes > 0)
        {
            double Percent = 100.0 * static_cast<double> (Input.WalBytes) / static_cast<double> (Input.WalMaxBytes);

            if (Percent >= HEALTH_ERROR_PERCENT)
                Raise (State, Reason, HEALTH_ERROR, "the WAL is more than half full");
            else if (Percent >= HEALTH_WARN_PERCENT)
                Raise (State, Reason, HEALTH_WARNING, "the WAL is more than a quarter full");
        }

        if ( (State != m_State) && pLog )
        {
            std::string Message = "Health: ";

            Message += GetName (State);

            if (false == Reason.empty())
            {
                Message += " (";
                Message += Reason;
                Message += ")";
            }

            pLog (Message.c_str());
        }

        m_State = State;
        return m_State;
    }

    int GetState() const
    {
        return m_State;
    }

    static const char *GetName (int State)
    {
        if (HEALTH_ERROR == State)
            return "ERROR";

        if (HEALTH_WARNING == State)
            return "WARNING";

        return "OK";
    }

private:

    int      m_State             = HEALTH_OK;
    time_t   m_tUnreachableSince = 0;
    time_t   m_tLastDropped      = 0;
    time_t   m_tLastRejected     = 0;
    uint64_t m_LastDropped       = 0;
    uint64_t m_LastRejected      = 0;

    /* The worst state wins, and its reason is the one which is logged */
    static void Raise (int& State, std::string& Reason, int NewState, const char *pszReason)
    {
        if (NewState > State)
        {
            State  = NewState;
            Reason = pszReason;
        }
    }
};

#endif
