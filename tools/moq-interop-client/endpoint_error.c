#include "endpoint_error.h"

#include <stdio.h>

const char *interop_result_name(moq_result_t rc)
{
    switch (rc) {
    case MOQ_OK:                     return "MOQ_OK";
    case MOQ_DONE:                   return "MOQ_DONE";
    case MOQ_ERR_NOMEM:              return "MOQ_ERR_NOMEM";
    case MOQ_ERR_INVAL:              return "MOQ_ERR_INVAL";
    case MOQ_ERR_PROTO:              return "MOQ_ERR_PROTO";
    case MOQ_ERR_CLOSED:             return "MOQ_ERR_CLOSED";
    case MOQ_ERR_WRONG_STATE:        return "MOQ_ERR_WRONG_STATE";
    case MOQ_ERR_STALE_HANDLE:       return "MOQ_ERR_STALE_HANDLE";
    case MOQ_ERR_WRONG_SESSION:      return "MOQ_ERR_WRONG_SESSION";
    case MOQ_ERR_WOULD_BLOCK:        return "MOQ_ERR_WOULD_BLOCK";
    case MOQ_ERR_BUFFER:             return "MOQ_ERR_BUFFER";
    case MOQ_ERR_REQUEST_BLOCKED:    return "MOQ_ERR_REQUEST_BLOCKED";
    case MOQ_ERR_ABI_MISMATCH:       return "MOQ_ERR_ABI_MISMATCH";
    case MOQ_ERR_GOAWAY:             return "MOQ_ERR_GOAWAY";
    case MOQ_ERR_INTERRUPTED:        return "MOQ_ERR_INTERRUPTED";
    case MOQ_ERR_UNSUPPORTED:        return "MOQ_ERR_UNSUPPORTED";
    case MOQ_ERR_INPUT_NOT_CONSUMED: return "MOQ_ERR_INPUT_NOT_CONSUMED";
    case MOQ_ERR_LIMIT:              return "MOQ_ERR_LIMIT";
    case MOQ_ERR_INTERNAL:           return "MOQ_ERR_INTERNAL";
    default:                         return NULL;
    }
}

char *interop_endpoint_error(char *buf, size_t cap, const char *what,
                             moq_result_t rc, uint32_t draft_pin)
{
    if (buf == NULL || cap == 0) return buf;
    const char *name = interop_result_name(rc);
    int n = snprintf(buf, cap, "failed to create %s: %s (%s %d)",
                     what != NULL ? what : "endpoint", moq_strerror(rc),
                     name != NULL ? name : "result", (int)rc);
    if (n >= 0 && (size_t)n < cap && rc == MOQ_ERR_UNSUPPORTED &&
        draft_pin != 0) {
        snprintf(buf + n, cap - (size_t)n,
                 "; requested draft-%u is not supported by this transport "
                 "backend", (unsigned)draft_pin);
    }
    return buf;
}
