/* Compile with an older provider: D16/18 attach, D22 explicitly refuses. */
#define main retained_reliable_reset_main
#ifdef MOQ_TERMINAL_WT
#include "../../pico_wt/tests/test_pico_wt_reliable_reset.c"
#else
#include "test_pq_sim_reliable_reset.c"
#endif
#undef main
#ifdef PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION
#error legacy_control_requires_provider_without_terminal_contract
#endif
int main(void)
{
    const moq_version_t versions[] = {MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22};
    for (size_t i = 0; i < sizeof(versions) / sizeof(versions[0]); i++) {
        scenario = "legacy-attachment";
#ifdef MOQ_TERMINAL_WT
        pico_wt_harness_t p;
        bool ok = open_harness(&p, 0x73, versions[i], true, 0);
        CHECK(ok == (versions[i] != MOQ_VERSION_DRAFT_22));
        pico_wt_harness_cleanup(&p);
#else
        pq_sim_t p;
        int rc = sim_setup(&p, 0x73, versions[i], true, 0);
        if (rc == 0) rc = sim_handshake(&p);
        CHECK((rc == 0) == (versions[i] != MOQ_VERSION_DRAFT_22));
        sim_cleanup(&p);
#endif
        printf("legacy d%u attachment checked\n", (unsigned)versions[i]);
    }
    return failures != 0;
}
