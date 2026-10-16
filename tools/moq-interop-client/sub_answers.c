#include "sub_answers.h"

#include <string.h>

void interop_sub_answers_init(interop_sub_answers_t *a, moq_namespace_t want_ns,
                              moq_bytes_t want_name)
{
    memset(a, 0, sizeof(*a));
    a->want_ns = want_ns;
    a->want_name = want_name;
    a->failed_rc = MOQ_OK;
}

static bool bytes_eq(moq_bytes_t x, moq_bytes_t y)
{
    return x.len == y.len && (x.len == 0 || memcmp(x.data, y.data, x.len) == 0);
}

bool interop_full_track_name_eq(moq_namespace_t ns_a, moq_bytes_t name_a,
                                moq_namespace_t ns_b, moq_bytes_t name_b)
{
    if (ns_a.count != ns_b.count || !bytes_eq(name_a, name_b)) return false;
    for (size_t i = 0; i < ns_a.count; i++)
        if (!bytes_eq(ns_a.parts[i], ns_b.parts[i])) return false;
    return true;
}

bool interop_sub_answers_on_request(interop_sub_answers_t *a,
                                    const moq_session_t *s,
                                    const moq_subscribe_request_event_t *req)
{
    bool test = interop_full_track_name_eq(req->track_namespace, req->track_name,
                                           a->want_ns, a->want_name);
    if (test && !a->have_test) {
        a->have_test = true;
        a->test_sub = req->sub;
        return true;
    }
    if (a->n_pending == INTEROP_SUB_ANSWERS_MAX) {
        a->unanswered = true;
        return false;
    }
    a->pending[a->n_pending] = req->sub;
    a->pending_code[a->n_pending] = !test
        ? MOQ_REQUEST_ERROR_DOES_NOT_EXIST
        : moq_session_supports_concurrent_subscriptions(s)
              ? MOQ_REQUEST_ERROR_NOT_SUPPORTED
              : MOQ_REQUEST_ERROR_DUPLICATE_SUBSCRIPTION;
    a->n_pending++;
    return false;
}

void interop_sub_answers_flush(interop_sub_answers_t *a, moq_session_t *s,
                               uint64_t now_us)
{
    size_t done = 0;
    while (done < a->n_pending) {
        moq_reject_subscribe_cfg_t rcfg;
        moq_reject_subscribe_cfg_init(&rcfg);
        rcfg.error_code = a->pending_code[done];
        bool other = rcfg.error_code == MOQ_REQUEST_ERROR_DOES_NOT_EXIST;
        rcfg.reason = other
            ? (moq_bytes_t){ (const uint8_t *)"no such track", 13 }
            : (moq_bytes_t){ (const uint8_t *)"one subscription per track", 26 };
        moq_result_t rc = moq_session_reject_subscribe(s, a->pending[done], &rcfg,
                                                       now_us);
        if (rc == MOQ_ERR_WOULD_BLOCK) break;   /* the rest wait, in order */
        if (rc == MOQ_OK) {
            if (other) a->refused_other++;
            else a->refused_duplicate++;
        } else {
            a->unanswered = true;
            a->failed_rc = rc;
        }
        done++;
    }
    if (done > 0) {
        memmove(a->pending, a->pending + done,
                (a->n_pending - done) * sizeof(a->pending[0]));
        memmove(a->pending_code, a->pending_code + done,
                (a->n_pending - done) * sizeof(a->pending_code[0]));
        a->n_pending -= done;
    }
}

interop_verdict_t interop_publisher_verdict(bool step_ok,
                                            const interop_sub_answers_t *a,
                                            const char **why)
{
    if (a->unanswered) {
        if (why != NULL)
            *why = a->failed_rc != MOQ_OK
                ? "a forwarded SUBSCRIBE could not be answered (refusal failed)"
                : "a forwarded SUBSCRIBE could not be answered (refusal queue full)";
        return INTEROP_VERDICT_FAIL;
    }
    if (!step_ok) return INTEROP_VERDICT_FAIL;
    return a->n_pending == 0 ? INTEROP_VERDICT_PASS : INTEROP_VERDICT_WAIT;
}
