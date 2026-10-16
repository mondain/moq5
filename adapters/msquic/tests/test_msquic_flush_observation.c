/* Supplemental private-state checks. The managed integration test covers the
 * causal transport behaviour; these controls discriminate every deferred output
 * class from receive-only/application-owned work without driving the session. */
#include "../../../core/src/session/session_outbound_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static unsigned checks, failures;
struct fixture {
    moq_session_t s;
    moq_profile_ops_t profile;
    moq_sg_entry_t sg;
    moq_rx_stream_t rx;
    moq_sub_entry_t sub;
    moq_pub_entry_t pub;
    moq_fill_entry_t fill;
    moq_ann_entry_t ann;
    moq_ts_entry_t ts;
    moq_ns_sub_entry_t ns;
    moq_track_sub_entry_t track;
    uint8_t drain_reason;
};

static void reset(struct fixture *f)
{
    memset(f, 0, sizeof(*f));
    f->s.profile = &f->profile;
    f->s.sg_occ_head = f->s.rx_occ_head = -1;
    f->s.sub_occ_head = f->s.pub_occ_head = -1;
    f->s.subgroups = &f->sg;
    f->s.rx_streams = &f->rx;
    f->s.subs = &f->sub;
    f->s.publishes = &f->pub;
    f->sg.occ_next = f->rx.occ_next = -1;
    f->sub.occ_next = f->pub.occ_next = -1;
    f->s.fills.entries = &f->fill;
    f->s.fills.capacity = 1;
    f->s.announcements = &f->ann;
    f->s.ann_cap = 1;
    f->s.track_statuses = &f->ts;
    f->s.ts_cap = 1;
    f->s.ns_subs = &f->ns;
    f->s.ns_sub_cap = 1;
    f->s.track_subs = &f->track;
    f->s.track_sub_cap = 1;
    f->s.drain_ref_reasons = &f->drain_reason;
}

static void observe(struct fixture *f, bool owed, const char *label)
{
    struct fixture *before = malloc(sizeof(*before));
    if (!before) exit(2);
    memcpy(before, f, sizeof(*before));
    ++checks;
    if (session_has_outbound_obligation(&f->s) != owed ||
        memcmp(before, f, sizeof(*f)) != 0) {
        fprintf(stderr, "FAIL private observation: %s\n", label);
        ++failures;
    }
    free(before);
}

static void t_queues(struct fixture *f)
{
    reset(f);
    f->s.send_len = 19;
    f->s.event_tail = 1;
    f->s.request_refeed_pending = true;
    f->s.subgroup_retry_deadline_us = 500;
    f->s.expiry_retry_deadline_us = 900;
    observe(f, false, "borrow arena, receive backlog and future timer");
    f->s.action_tail = 1;
    observe(f, true, "action ring");
    f->s.action_head = 1;
    f->s.noslot_carrier_count = 1;
    observe(f, true, "no-slot refusal output");
    f->s.noslot_carrier_count = 0;
    f->s.drain_ref_count = 1;
    observe(f, false, "ordinary incoming drain");
    f->drain_reason = MOQ_DRAIN_REJECT_STOP_OWED;
    observe(f, true, "rejection STOP owed");
    reset(f);
    f->s.sg_occ_head = 0;
    f->sg.state = MOQ_SG_RESET_OWED;
    observe(f, true, "subgroup RESET owed");
    f->s.sg_occ_head = -1;
    f->s.rx_occ_head = 0;
    f->rx.parse_state = MOQ_RX_NEED_STOP;
    observe(f, true, "receive STOP owed");
    f->rx.parse_state = MOQ_RX_PENDING_CHUNK;
    f->rx.pending_fin = true;
    f->rx.reset_owed = true;
    observe(f, false, "receive chunk/FIN/terminal delivery alone");
    f->s.sub_occ_head = 0;
    f->sub.state = MOQ_SUB_DONE_HELD;
    observe(f, true, "subscription PUBLISH_DONE held");
    f->s.sub_occ_head = -1;
    f->s.pub_occ_head = 0;
    f->pub.state = MOQ_PUB_DONE_HELD;
    observe(f, true, "publication PUBLISH_DONE held");
    f->s.pub_occ_head = -1;
    f->sub.owed_update.owed = f->pub.owed_update.owed = true;
    observe(f, false, "future application update answers");
}

static void t_fill(struct fixture *f)
{
    reset(f);
    moq_fill_entry_t *e = &f->fill;
    e->active = true;
    e->role = MOQ_FILL_PUBLISHER;
    observe(f, false, "fill awaiting application answer");
    e->control = MOQ_FILL_CONTROL_ACCEPTED;
    e->delivery_known = true;
    e->delivery.outcome = MOQ_FILL_OUTCOME_STREAM;
    observe(f, true, "fill HEADER owed");
    e->stream_bound = true;
    observe(f, false, "open idle fill");
    e->finish_requested = true;
    observe(f, true, "fill FIN owed");
    e->finish_requested = false;
    e->abort_requested = true;
    observe(f, true, "fill RESET owed");
    e->data_reset = true;
    observe(f, false, "fill RESET committed");
    e->data_reset = false;
    e->role = MOQ_FILL_SUBSCRIBER;
    observe(f, true, "fill STOP owed");
    e->stop_committed = true;
    observe(f, false, "fill STOP committed awaiting peer");
    e->active = false;
    observe(f, false, "retired fill");
}

static void t_request_families(struct fixture *f)
{
    reset(f);
    f->ann.state = MOQ_ANN_DRAINING;
    f->ann.finish_fin = true;
    observe(f, true, "namespace FIN owed");
    f->ann.rs.lifetime.local_fin = true;
    observe(f, false, "namespace FIN committed");
    f->ann.cancel_pending = true;
    observe(f, true, "namespace STOP still owed");
    f->ann.rs.lifetime.local_stop = true;
    observe(f, false, "namespace cancellation committed");
    f->ann.rs.lifetime.local_stop = false;
    f->ann.deferred_reset = true;
    observe(f, false, "crossed RESET removes namespace STOP obligation");
    f->ann.rs.lifetime.local_fin = false;
    observe(f, true, "deferred namespace RESET still owes local reset");
    f->ann.cancel_pending = f->ann.finish_fin = f->ann.deferred_reset = false;
    f->ann.deferred_fin = true;
    f->ann.deferred_cause = MOQ_NAMESPACE_TERMINAL_PEER_FIN_COMPLETE;
    observe(f, true, "deferred namespace completion owes reciprocal FIN");
    f->ann.rs.lifetime.local_fin = true;
    observe(f, false, "crossed FIN leaves no namespace output");
    f->ann.deferred_cause = 0;
    f->ann.rs.lifetime.local_fin = false;
    observe(f, false, "legal requester FIN alone is receive-only");
    reset(f);
    f->ts.state = MOQ_TS_DRAINING_RESPONSE;
    f->ts.goaway_sent = true;
    observe(f, true, "track-status sent migration cancellation");
    f->ts.rs.lifetime.local_reset = true;
    f->ts.rs.lifetime.local_stop = true;
    observe(f, false, "track-status cancellation committed");
    f->ts.rs.lifetime.local_stop = false;
    f->ts.deferred_reset = true;
    observe(f, false, "crossed RESET removes track-status STOP obligation");
    f->ts.rs.lifetime.local_reset = false;
    observe(f, true, "deferred track-status reset owes local reset");
    reset(f);
    f->ns.state = MOQ_NS_SUB_ESTABLISHED;
    f->ns.local_teardown_pending = true;
    observe(f, true, "namespace-sub local teardown");
    f->ns.local_teardown_pending = false;
    f->ns.suffix_cap_terminating = true;
    observe(f, true, "namespace-sub suffix cap close");
    f->ns.suffix_cap_terminating = false;
    f->ns.terminal.cause = MOQ_REQUEST_TERMINAL_PEER_FIN_COMPLETE;
    observe(f, true, "namespace-sub reciprocal FIN");
    f->ns.cancel_lifetime.local_fin = true;
    observe(f, false, "namespace-sub FIN committed, event retained");
    f->ns.terminal.cause = MOQ_REQUEST_TERMINAL_PEER_STOP_SENDING;
    observe(f, true, "namespace-sub cancellation STOP");
    f->ns.cancel_lifetime.peer_reset = true;
    observe(f, false, "namespace-sub cancellation complete");
    f->ns.terminal.cause = 0;
    f->ns.handoff_fin_pending = true;
    observe(f, true, "legacy namespace-sub handoff reciprocal FIN");
    f->profile.request_fin_is_not_cancel = true;
    observe(f, false, "D22 namespace requester FIN alone");
    f->ns.local_subscriber = true;
    observe(f, true, "namespace responder FIN owes reciprocal close");
    reset(f);
    f->track.state = MOQ_TRACK_SUB_ESTABLISHED;
    f->track.handoff_fin_pending = true;
    observe(f, true, "legacy track-sub reciprocal FIN");
    f->track.state = MOQ_TRACK_SUB_DRAINING_RESPONSE;
    observe(f, false, "track-sub rejected response drain");
    f->track.state = MOQ_TRACK_SUB_ESTABLISHED;
    f->profile.request_fin_is_not_cancel = true;
    observe(f, false, "D22 track-sub requester FIN alone");
}

int main(void)
{
    struct fixture *f = malloc(sizeof(*f));
    if (!f) return 2;
    t_queues(f);
    t_fill(f);
    t_request_families(f);
    free(f);
    printf("msquic private observation: %u checks, %u failures\n", checks, failures);
    return failures ? 1 : 0;
}
