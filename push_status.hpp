
#pragma once

/* What to do with a push request, decided by the HTTP status of the answer of the OTLP receiver.

   The OTLP specification retries only 429, 502, 503 and 504 and says that all other 4xx and 5xx codes must not be retried,
   and that a 400 is never retried. otelfwd is more careful with the data than that. A wrong token (401), a wrong URL (404) or
   a size limit of the receiver (413) is a problem of the configuration, which is fixed later. Dropping the logs meanwhile would
   lose them. So only a 400, the receiver telling that the data itself is bad, fails completely.

   Status                                         Action           What it means
   ---------------------------------------------  ---------------  -----------------------------------------------------------
   200 - 299                                      PUSH_ACCEPTED    delivered
   400                                            PUSH_REJECTED    the receiver refuses this data for good. It is not tried at
                                                                   the backup endpoint, not kept in the WAL, but counted and logged
   everything else, including no answer (0):      PUSH_RETRY       not delivered. Try the backup endpoint, then keep the request
   no connection, timeout, 1xx, 3xx (redirects                     in the WAL and try again later
   are not followed), 401, 403, 404, 408, 413,
   429, 500, 502, 503, 504 ... */

enum PushAction
{
    PUSH_ACCEPTED = 0,
    PUSH_RETRY,
    PUSH_REJECTED
};


/* HttpStatus is the status of the answer, or 0 if there was none (no connection, timeout, ...) */
inline PushAction GetPushAction (long HttpStatus)
{
    if ( (HttpStatus >= 200) && (HttpStatus <= 299) )
        return PUSH_ACCEPTED;

    if (400 == HttpStatus)
        return PUSH_REJECTED;

    return PUSH_RETRY;
}


/* For log messages */
inline const char *GetPushActionName (PushAction Action)
{
    switch (Action)
    {
        case PUSH_ACCEPTED: return "accepted";
        case PUSH_RETRY:    return "retry";
        case PUSH_REJECTED: return "rejected";
    }

    return "unknown";
}
