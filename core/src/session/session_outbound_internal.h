#ifndef MOQ_SESSION_OUTBOUND_INTERNAL_H
#define MOQ_SESSION_OUTBOUND_INTERNAL_H

#include "session_internal.h"

/* Private, same-build observation under the session's executor lock. No poll,
 * clock, allocation or progress: future application answers/timers and receive
 * backpressure alone are not accepted output. send_len is a borrow arena, not
 * a backlog; its bytes belong to actions and can survive their last poll. */
static inline bool session_cancel_output_owed(const moq_request_lifetime_t *l)
{
    return (!l->local_fin && !l->local_reset) ||
           (!l->peer_fin && !l->peer_reset && !l->local_stop);
}

static inline bool session_fill_output_owed(const moq_fill_entry_t *e)
{
    if (!e->active || (e->stream_bound && (e->data_fin || e->data_reset)))
        return false;
    bool stopping = e->cancel_requested || e->abort_requested ||
        e->parent_cancelled || e->peer_stop ||
        e->control == MOQ_FILL_CONTROL_REJECTED ||
        e->control == MOQ_FILL_CONTROL_RESET;
    if (e->role == MOQ_FILL_SUBSCRIBER)
        return e->stream_bound && stopping && !e->stop_committed;
    if (e->stream_bound)
        return stopping || e->finish_requested;
    return !e->parent_cancelled && e->control == MOQ_FILL_CONTROL_ACCEPTED &&
        e->delivery_known && e->delivery.outcome == MOQ_FILL_OUTCOME_STREAM;
}

static inline bool session_has_retained_outbound_obligation(const moq_session_t *s)
{
    if (s->noslot_carrier_count != 0) return true;
    for (size_t i = 0; i < s->drain_ref_count; ++i)
        if (s->drain_ref_reasons[i] == MOQ_DRAIN_REJECT_STOP_OWED)
            return true;
    for (int32_t i = s->sg_occ_head; i >= 0; i = s->subgroups[i].occ_next)
        if (s->subgroups[i].state == MOQ_SG_RESET_OWED)
            return true;
    for (int32_t i = s->rx_occ_head; i >= 0; i = s->rx_streams[i].occ_next)
        if (s->rx_streams[i].parse_state == MOQ_RX_NEED_STOP)
            return true;
    for (int32_t i = s->sub_occ_head; i >= 0; i = s->subs[i].occ_next)
        if (s->subs[i].state == MOQ_SUB_DONE_HELD ||
            s->subs[i].rs.retained_output != MOQ_RETAINED_OUTPUT_NONE)
            return true;
    for (int32_t i = s->pub_occ_head; i >= 0; i = s->publishes[i].occ_next)
        if (s->publishes[i].state == MOQ_PUB_DONE_HELD ||
            s->publishes[i].rs.retained_output != MOQ_RETAINED_OUTPUT_NONE)
            return true;
    for (size_t i = 0; i < s->fetch_cap; ++i)
        if (s->fetches[i].state != MOQ_FETCH_FREE &&
            s->fetches[i].rs.retained_output != MOQ_RETAINED_OUTPUT_NONE)
            return true;
    for (size_t i = 0; i < s->fills.capacity; ++i)
        if (session_fill_output_owed(&s->fills.entries[i]))
            return true;

    /* These families retain directional output independently of event delivery.
     * Deferred peer signals can still owe cancellation after a validated event
     * is delivered; ordinary retained responses alone do not owe output. */
    for (size_t i = 0; i < s->ann_cap; ++i) {
        const moq_ann_entry_t *e = &s->announcements[i];
        if (e->state == MOQ_ANN_FREE) continue;
        if (e->rs.retained_output == MOQ_RETAINED_OUTPUT_RESPONSE) return true;
        moq_request_lifetime_t life = e->rs.lifetime;
        life.peer_reset |= e->deferred_reset;
        life.peer_fin |= e->deferred_fin || e->response_fin;
        bool finish = e->finish_fin ||
            e->rs.retained_output == MOQ_RETAINED_OUTPUT_FIN ||
            (e->response_validated && e->response_fin &&
             e->role == MOQ_ANN_ROLE_ANNOUNCER) ||
            e->deferred_cause == MOQ_NAMESPACE_TERMINAL_PEER_FIN_COMPLETE;
        bool cancel = e->cancel_pending || e->deferred_reset || e->deferred_stop ||
            e->rs.retained_output == MOQ_RETAINED_OUTPUT_CANCEL ||
            (e->deferred_cause &&
             e->deferred_cause != MOQ_NAMESPACE_TERMINAL_PEER_FIN_COMPLETE);
        if (finish && !life.local_fin && !life.local_reset) return true;
        if (cancel && session_cancel_output_owed(&life))
            return true;
    }
    for (size_t i = 0; i < s->ts_cap; ++i) {
        const moq_ts_entry_t *e = &s->track_statuses[i];
        if (e->state == MOQ_TS_FREE) continue;
        moq_request_lifetime_t life = e->rs.lifetime;
        life.peer_reset |= e->deferred_reset;
        life.peer_fin |= e->rs.recv_fin;
        bool sent_migration = e->goaway_sent &&
            e->state == MOQ_TS_DRAINING_RESPONSE && !e->terminal_cause &&
            !e->migration_committed;
        if ((e->terminal_cause || e->migration_committed ||
             e->rs.retained_output == MOQ_RETAINED_OUTPUT_CANCEL ||
             e->rs.lifetime.cancelled || sent_migration ||
             e->deferred_reset || e->deferred_stop) &&
            session_cancel_output_owed(&life))
            return true;
    }
    for (size_t i = 0; i < s->ns_sub_cap; ++i) {
        const moq_ns_sub_entry_t *e = &s->ns_subs[i];
        if (e->state == MOQ_NS_SUB_FREE) continue;
        if (e->retained_output != MOQ_RETAINED_OUTPUT_NONE) return true;
        if (e->local_teardown_pending || e->suffix_cap_terminating) return true;
        if (e->terminal.cause) {
            const moq_request_lifetime_t *l = &e->cancel_lifetime;
            if (e->terminal.cause == MOQ_REQUEST_TERMINAL_PEER_FIN_COMPLETE &&
                !l->cancelled) {
                if (!l->local_fin) return true;
            } else if (session_cancel_output_owed(l)) {
                return true;
            }
        } else if (e->state != MOQ_NS_SUB_CLOSING &&
                   (e->pending_fin || e->handoff_fin_pending) &&
                   (e->local_subscriber || !s->profile->request_fin_is_not_cancel)) {
            return true;
        }
    }
    for (size_t i = 0; i < s->track_sub_cap; ++i) {
        const moq_track_sub_entry_t *e = &s->track_subs[i];
        if (e->state != MOQ_TRACK_SUB_FREE &&
            e->rs.retained_output != MOQ_RETAINED_OUTPUT_NONE) return true;
        if (e->state != MOQ_TRACK_SUB_FREE &&
            e->state != MOQ_TRACK_SUB_DRAINING_RESPONSE &&
            !s->profile->request_fin_is_not_cancel &&
            (e->rs.recv_fin || e->handoff_fin_pending))
            return true;
    }
    return false;
}

static inline bool session_has_outbound_obligation(const moq_session_t *s)
{
    return s->action_head != s->action_tail ||
           session_has_retained_outbound_obligation(s);
}

static inline bool session_has_reliable_outbound_obligation(const moq_session_t *s)
{
    /* Exclude only explicit datagrams. Retained lifecycle work and unknown
     * action kinds stay conservative; observation never drains the ring. */
    size_t count = s->action_tail - s->action_head;
    for (size_t i = 0; i < count; ++i)
        if (s->actions[(s->action_head + i) % s->action_cap].kind !=
            MOQ_ACTION_SEND_DATAGRAM)
            return true;
    return session_has_retained_outbound_obligation(s);
}

#endif
