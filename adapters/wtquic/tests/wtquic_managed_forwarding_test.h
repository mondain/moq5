#ifndef WTQUIC_MANAGED_FORWARDING_TEST_H
#define WTQUIC_MANAGED_FORWARDING_TEST_H

#include <moq/wtquic.h>

/* Defined only by the test wrapper TUs, never by a shipped library. */
typedef struct {
    uint32_t table_size;
    bool installed;
} wtq_test_forward_table_t;

wtq_test_forward_table_t wtq_test_msquic_commit_dispatch(
    const wtq_session_events_t *forward, wtq_session_t *session,
    wtq_stream_t *stream, moq_wtquic_conn_t *adapter);
wtq_test_forward_table_t wtq_test_network_commit_dispatch(
    const wtq_session_events_t *forward, wtq_session_t *session,
    wtq_stream_t *stream, moq_wtquic_conn_t *adapter);

#endif
