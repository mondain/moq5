/* Uses the same private table initializer as the unsupported constructor's
 * retained implementation. This does not bypass backend admission policy. */
#ifndef MOQ_WTQ_MM_TESTING
#error "internal fixture requires MOQ_WTQ_MM_TESTING"
#endif
#include "../wtquic_network_managed.c"
#include "wtquic_managed_forwarding_test.h"

wtq_test_forward_table_t wtq_test_network_commit_dispatch(
    const wtq_session_events_t *forward, wtq_session_t *session,
    wtq_stream_t *stream, moq_wtquic_conn_t *adapter)
{
    moq_wtquic_network_managed_t m = {.fwd = forward, .mc = adapter};
    wtq_session_events_t events;
    nwm_events_init(&events);
    wtq_test_forward_table_t result = {
        .table_size = events.struct_size,
        .installed = events.on_stream_send_committed != NULL,
    };
    if (result.installed) {
        events.on_stream_send_committed(session, stream, &m);
        events.on_stream_send_committed(session, stream, &m);
        m.mc = NULL;
        events.on_stream_send_committed(session, stream, &m);
    }
    return result;
}
