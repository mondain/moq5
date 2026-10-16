/* Pure private-observer invariants, not a protocol or provider simulation. */
#include "../../core/src/session/session_outbound_internal.h"

#include <stdio.h>
#include <stdlib.h>

#ifdef MOQ_TEST_RELIABLE_OBSERVER_BASELINE
#define reliable_output session_has_outbound_obligation
#else
#define reliable_output session_has_reliable_outbound_obligation
#endif

static unsigned failures;
static unsigned cases;

static void check(moq_session_t *s, bool any, bool reliable, const char *name)
{
    moq_session_t *before = malloc(sizeof(*before));
    if (before == NULL) abort();
    memcpy(before, s, sizeof(*before));
    moq_action_t actions[4];
    memcpy(actions, s->actions, sizeof(actions));
    bool observed_any = session_has_outbound_obligation(s);
    bool observed_reliable = reliable_output(s);
    ++cases;
    if (observed_any != any || observed_reliable != reliable ||
        memcmp(before, s, sizeof(*before)) != 0 ||
        memcmp(actions, s->actions, sizeof(actions)) != 0) {
        fprintf(stderr, "FAIL %s any=%d/%d reliable=%d/%d\n", name,
                observed_any, any, observed_reliable, reliable);
        ++failures;
    }
    free(before);
}

int main(void)
{
    moq_session_t *s = calloc(1, sizeof(*s));
    if (s == NULL) return 2;
    moq_action_t actions[4] = {0};
    s->actions = actions;
    s->action_cap = 4;
    s->sg_occ_head = s->rx_occ_head = s->sub_occ_head = s->pub_occ_head = -1;
    check(s, false, false, "empty");

    for (size_t i = 0; i < 4; ++i) actions[i].kind = MOQ_ACTION_SEND_DATAGRAM;
    s->action_head = 3;
    for (size_t count = 1; count <= 4; ++count) {
        s->action_tail = s->action_head + count;
        check(s, true, false, "datagrams-only-wrapped");
    }
    /* Every reliable/control kind at every position of a physically wrapped
     * full ring must remain visible behind datagrams. Unknown kinds fail safe. */
    const moq_action_kind_t kinds[] = {
        MOQ_ACTION_SEND_CONTROL, MOQ_ACTION_CLOSE_SESSION, MOQ_ACTION_SEND_DATA,
        MOQ_ACTION_RESET_DATA, MOQ_ACTION_STOP_DATA, MOQ_ACTION_OPEN_BIDI_STREAM,
        MOQ_ACTION_SEND_BIDI_STREAM, MOQ_ACTION_CLOSE_BIDI_STREAM,
        MOQ_ACTION_OPEN_UNI_CONTROL, MOQ_ACTION_SEND_UNI_CONTROL,
        MOQ_ACTION_RESET_BIDI_STREAM, MOQ_ACTION_STOP_BIDI_STREAM,
        MOQ_ACTION_ABORT_BIDI_STREAM, UINT32_MAX
    };
    for (size_t k = 0; k < sizeof(kinds) / sizeof(kinds[0]); ++k) {
        for (size_t i = 0; i < 4; ++i) {
            actions[i].kind = kinds[k];
            check(s, true, true, "mixed-wrapped");
            actions[i].kind = MOQ_ACTION_SEND_DATAGRAM;
        }
    }

    moq_ns_sub_entry_t ns = {0};
    s->ns_subs = &ns;
    s->ns_sub_cap = 1;
    ns.state = MOQ_NS_SUB_ESTABLISHED;
    ns.local_subscriber = true;
    ns.pending_fin = true;
    check(s, true, true, "datagrams-and-retained-fin");
    s->action_head = s->action_tail;
    check(s, true, true, "retained-fin-only");
    ns.pending_fin = false;
    check(s, false, false, "no-retained-output");
    /* Stale storage outside the live ring is not queued work. */
    actions[0].kind = MOQ_ACTION_SEND_DATA;
    check(s, false, false, "stale-ring-storage");

    free(s);
    printf("outbound observation: %u cases, %u failures\n", cases, failures);
    return failures != 0;
}
