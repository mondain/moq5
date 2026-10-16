#include "request_lifetime_internal.h"
#include <string.h>

static bool family_valid(moq_request_family_t family)
{
    return family >= MOQ_REQUEST_FAMILY_SUBSCRIBE &&
           family <= MOQ_REQUEST_FAMILY_SUBSCRIBE_TRACKS;
}

static bool valid(const moq_request_lifetime_t *s)
{
    return s && family_valid(s->family);
}

static bool subscription(const moq_request_lifetime_t *s)
{
    return s->family == MOQ_REQUEST_FAMILY_SUBSCRIBE ||
           s->family == MOQ_REQUEST_FAMILY_PUBLISH;
}

static bool local_publisher(const moq_request_lifetime_t *s)
{
    return (s->family == MOQ_REQUEST_FAMILY_SUBSCRIBE && !s->local_requester) ||
           (s->family == MOQ_REQUEST_FAMILY_PUBLISH && s->local_requester);
}

static moq_result_t unexpected(bool outgoing)
{
    return outgoing ? MOQ_ERR_WRONG_STATE : MOQ_ERR_PROTO;
}

static bool direction_closed(const moq_request_lifetime_t *s, bool outgoing)
{
    return outgoing ? s->local_fin || s->local_reset : s->peer_fin || s->peer_reset;
}

static bool failed(const moq_request_lifetime_t *s)
{
    return s->cancelled || s->peer_fin_failed;
}

moq_result_t moq_request_lifetime_init(moq_request_lifetime_t *s,
    moq_request_family_t family, bool local_requester)
{
    if (!s || !family_valid(family)) return MOQ_ERR_INVAL;
    memset(s, 0, sizeof(*s));
    s->family = family;
    s->local_requester = local_requester;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_check_initial_response(
    const moq_request_lifetime_t *s, bool outgoing)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    if (outgoing == s->local_requester || failed(s) ||
        s->response != MOQ_REQUEST_RESPONSE_PENDING || direction_closed(s, outgoing))
        return unexpected(outgoing);
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_initial_response_committed(
    moq_request_lifetime_t *s, bool outgoing, bool accepted)
{
    moq_result_t rc = moq_request_lifetime_check_initial_response(s, outgoing);
    if (rc != MOQ_OK) return rc;
    s->response = accepted ? MOQ_REQUEST_RESPONSE_ACCEPTED : MOQ_REQUEST_RESPONSE_REJECTED;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_check_fin(const moq_request_lifetime_t *s)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    if (s->local_fin) return MOQ_OK;
    if (s->local_reset || failed(s) || s->incoming_update)
        return MOQ_ERR_WRONG_STATE;
    if (s->response == MOQ_REQUEST_RESPONSE_PENDING &&
        (!s->local_requester ||
         (s->family == MOQ_REQUEST_FAMILY_PUBLISH && !s->publisher_done)))
        return MOQ_ERR_WRONG_STATE;
    if (s->response == MOQ_REQUEST_RESPONSE_ACCEPTED &&
        local_publisher(s) && !s->publisher_done)
        return MOQ_ERR_WRONG_STATE;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_fin_committed(moq_request_lifetime_t *s)
{
    moq_result_t rc = moq_request_lifetime_check_fin(s);
    if (rc != MOQ_OK) return rc;
    s->local_fin = true;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_peer_fin(moq_request_lifetime_t *s)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    if (s->peer_fin)
        return s->peer_fin_failed ? MOQ_REQUEST_LIFETIME_FAILED : MOQ_OK;
    bool missing = false;
    if (!s->cancelled) {
        missing = (s->local_requester && s->response == MOQ_REQUEST_RESPONSE_PENDING) ||
                  s->outgoing_update;
        if (subscription(s) && !local_publisher(s) && !s->publisher_done &&
            (s->response == MOQ_REQUEST_RESPONSE_ACCEPTED ||
             (s->family == MOQ_REQUEST_FAMILY_PUBLISH &&
              s->response == MOQ_REQUEST_RESPONSE_PENDING)))
            missing = true;
    }
    s->peer_fin = true;
    s->peer_fin_failed = missing;
    return missing ? MOQ_REQUEST_LIFETIME_FAILED : MOQ_OK;
}

unsigned moq_request_lifetime_cancel_actions(const moq_request_lifetime_t *s)
{
    if (!valid(s)) return 0;
    unsigned actions = 0;
    if (!s->local_fin && !s->local_reset) actions |= MOQ_REQUEST_CANCEL_RESET_LOCAL;
    if (!s->peer_fin && !s->peer_reset && !s->local_stop)
        actions |= MOQ_REQUEST_CANCEL_STOP_PEER;
    return actions;
}

moq_result_t moq_request_lifetime_reset_committed(moq_request_lifetime_t *s)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    if (s->local_reset) return MOQ_OK;
    if (s->local_fin) return MOQ_ERR_WRONG_STATE;
    s->local_reset = true;
    s->cancelled = true;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_stop_committed(moq_request_lifetime_t *s)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    if (s->local_stop) return MOQ_OK;
    if (s->peer_fin || s->peer_reset) return MOQ_ERR_WRONG_STATE;
    s->local_stop = true;
    s->cancelled = true;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_peer_reset(moq_request_lifetime_t *s)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    s->peer_reset = true;
    s->cancelled = true;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_peer_stop(moq_request_lifetime_t *s)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    s->peer_stop = true;
    s->cancelled = true;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_check_update(const moq_request_lifetime_t *s,
    bool outgoing)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    bool sender_requester = outgoing ? s->local_requester : !s->local_requester;
    if (s->family == MOQ_REQUEST_FAMILY_TRACK_STATUS ||
        (!sender_requester && s->family != MOQ_REQUEST_FAMILY_PUBLISH) ||
        s->response == MOQ_REQUEST_RESPONSE_REJECTED || failed(s) || s->update_failed ||
        s->local_reset || direction_closed(s, false) ||
        (s->publisher_done && !local_publisher(s)))
        return unexpected(outgoing);
    if (!outgoing && s->incoming_update) return MOQ_REQUEST_LIFETIME_UPDATE_LIMIT;
    if (s->local_fin || (s->publisher_done && local_publisher(s)))
        return outgoing ? MOQ_ERR_WRONG_STATE : MOQ_REQUEST_LIFETIME_TERMINAL_CROSSING;
    if (s->response == MOQ_REQUEST_RESPONSE_PENDING)
        return outgoing ? MOQ_ERR_WRONG_STATE : MOQ_ERR_WOULD_BLOCK;
    if (outgoing && s->outgoing_update) return MOQ_ERR_WOULD_BLOCK;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_update_committed(moq_request_lifetime_t *s,
    bool outgoing)
{
    moq_result_t rc = moq_request_lifetime_check_update(s, outgoing);
    if (rc != MOQ_OK) return rc;
    if (outgoing) s->outgoing_update = true;
    else s->incoming_update = true;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_check_update_response(
    const moq_request_lifetime_t *s, bool outgoing)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    bool pending = outgoing ? s->incoming_update : s->outgoing_update;
    if (!pending || failed(s) || s->response != MOQ_REQUEST_RESPONSE_ACCEPTED ||
        direction_closed(s, outgoing) ||
        (s->publisher_done && local_publisher(s) == outgoing))
        return unexpected(outgoing);
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_update_response_committed(
    moq_request_lifetime_t *s, bool outgoing, bool accepted)
{
    moq_result_t rc = moq_request_lifetime_check_update_response(s, outgoing);
    if (rc != MOQ_OK) return rc;
    if (outgoing) s->incoming_update = false;
    else s->outgoing_update = false;
    if (!accepted) s->update_failed = true;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_check_done(const moq_request_lifetime_t *s,
    bool outgoing, bool data_obligations_clear)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    bool pending_publish = s->family == MOQ_REQUEST_FAMILY_PUBLISH &&
                           s->response == MOQ_REQUEST_RESPONSE_PENDING;
    bool crossed_rejection = !outgoing && s->family == MOQ_REQUEST_FAMILY_PUBLISH &&
                             s->response == MOQ_REQUEST_RESPONSE_REJECTED;
    if (!subscription(s) || local_publisher(s) != outgoing || failed(s) ||
        (s->response != MOQ_REQUEST_RESPONSE_ACCEPTED && !pending_publish && !crossed_rejection) ||
        s->publisher_done ||
        direction_closed(s, outgoing) || (outgoing && s->incoming_update))
        return unexpected(outgoing);
    if (outgoing && !data_obligations_clear) return MOQ_ERR_WOULD_BLOCK;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_done_committed(moq_request_lifetime_t *s,
    bool outgoing, bool data_obligations_clear)
{
    moq_result_t rc = moq_request_lifetime_check_done(s, outgoing, data_obligations_clear);
    if (rc != MOQ_OK) return rc;
    s->publisher_done = true;
    return MOQ_OK;
}

moq_result_t moq_request_lifetime_check_notify(const moq_request_lifetime_t *s,
    bool outgoing)
{
    if (!valid(s)) return MOQ_ERR_INVAL;
    if (!subscription(s) || local_publisher(s) != outgoing || failed(s) ||
        s->response == MOQ_REQUEST_RESPONSE_REJECTED || s->publisher_done ||
        direction_closed(s, outgoing)) return unexpected(outgoing);
    if (s->response == MOQ_REQUEST_RESPONSE_PENDING)
        return outgoing ? MOQ_ERR_WRONG_STATE : MOQ_ERR_WOULD_BLOCK;
    return MOQ_OK;
}
