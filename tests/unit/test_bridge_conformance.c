#include "../support/bridge_conformance_pair.h"
#include "../conformance/conformance_scenarios.h"
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int run_count = 0;
static int skip_count = 0;
static int fail_count = 0;

/* MOQ_CONFORMANCE_ONLY and MOQ_CONFORMANCE_SKIP hold comma-separated scenario
 * names. ONLY restricts the run to the named scenarios; SKIP removes them.
 * Both unset runs everything. Lets one binary serve several ctest entries,
 * including an expected-failure entry for a known divergence. */
static bool name_listed(const char *list, const char *name)
{
    if (!list || !*list) return false;
    size_t n = strlen(name);
    for (const char *p = list; *p;) {
        const char *q = strchr(p, ',');
        size_t len = q ? (size_t)(q - p) : strlen(p);
        if (len == n && memcmp(p, name, n) == 0) return true;
        if (!q) break;
        p = q + 1;
    }
    return false;
}

static bool scenario_selected(const char *name)
{
    const char *only = getenv("MOQ_CONFORMANCE_ONLY");
    const char *skip = getenv("MOQ_CONFORMANCE_SKIP");
    if (only && *only && !name_listed(only, name)) return false;
    return !name_listed(skip, name);
}

#define RUN_SCENARIO(name, required_caps) do { \
    if (!scenario_selected(#name)) break; \
    moq_adapter_pair_t p = bridge_conformance_create(); \
    if (!p.ops) { \
        fprintf(stderr, "FAIL: create for " #name "\n"); \
        fail_count++; \
    } else if ((required_caps) && \
               (p.capabilities & (required_caps)) != (uint32_t)(required_caps)) { \
        skip_count++; \
        moq_pair_destroy(&p); \
    } else { \
        int f = name(&p); \
        if (f > 0) fail_count += f; \
        run_count++; \
        moq_pair_destroy(&p); \
    } \
} while (0)

#define CAP_BIDI (MOQ_ADAPTER_PAIR_CAP_BIDI_STREAMS | \
                  MOQ_ADAPTER_PAIR_CAP_INJECT_BIDI_FIN)

int main(void)
{
    RUN_SCENARIO(conformance_setup_handshake, 0);
    RUN_SCENARIO(conformance_subscribe_and_object, 0);
    RUN_SCENARIO(conformance_datagram_object, 0);
    RUN_SCENARIO(conformance_single_control_stream,
                 MOQ_ADAPTER_PAIR_CAP_OPENED_BIDI_COUNT);
    RUN_SCENARIO(conformance_close_not_fatal, 0);
    RUN_SCENARIO(conformance_session_timer_goaway, 0);
    RUN_SCENARIO(conformance_bidi_halfclose_accept, CAP_BIDI);
    RUN_SCENARIO(conformance_bidi_halfclose_reject_late_fin, CAP_BIDI);
    RUN_SCENARIO(conformance_crossed_cancel,
                 MOQ_ADAPTER_PAIR_CAP_BIDI_STREAMS);
    RUN_SCENARIO(conformance_reset_propagation, 0);
    RUN_SCENARIO(conformance_would_block_ordering, 0);
    RUN_SCENARIO(conformance_dropped_datagram_unblocks, 0);
    RUN_SCENARIO(conformance_blocked_ns_reject_tombstone,
                 CAP_BIDI | MOQ_ADAPTER_PAIR_CAP_TOMBSTONES |
                 MOQ_ADAPTER_PAIR_CAP_BLOCK_WRITES);

    printf("test_bridge_conformance: %d run, %d skipped, %d failed\n",
           run_count, skip_count, fail_count);

    return fail_count ? 1 : 0;
}
