/*
 * Data-stream resets over real mvfst on localhost (attach adapter, one
 * QuicServer per process), observed from the receiving application.
 *
 * Each round the publisher opens a subgroup, writes an object, resets the
 * subgroup and ends the subscription with PUBLISH_DONE counting that one
 * stream -- all in one pass, so the stream header is still unsent when the
 * reset is asked for. The subscriber surfaces the done promptly only when it
 * received the header (it can then account for the reset stream).
 *
 * The mvfst revision this adapter builds against closes the connection when
 * it receives RESET_STREAM_AT, so neither end advertises reliable stream
 * reset and the bridge falls back to a plain reset (the protocol's fallback).
 * Pinned: every round's subscription works, the connection stays healthy
 * across eight such resets, and the observed header delivery rate is printed.
 * argv[1] selects the draft (16 or 22).
 */

#include "support/mvfst_loopback_pair.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>

static int failures = 0;

#define MVFST_CHECK(expr) do { \
    if (!(expr)) { \
        std::fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

using namespace moq::mvfst::test;

/* Each round is a new request; the pair's server grants 16 request ids, so
 * eight rounds of client requests fit. */
static const int ROUNDS = 8;

static bool client_saw(loopback_pair &lp, moq_event_kind_t kind)
{
    bool seen = false;
    moq_event_t ev[16]; size_t ne;
    moq_session_poll_events_ex(lp.client_session, ev, 16, sizeof(moq_event_t), &ne);
    for (size_t i = 0; i < ne; i++) {
        if (ev[i].kind == kind) seen = true;
        moq_event_cleanup(&ev[i]);
    }
    return seen;
}

/* One round on its own track: 1 when the done surfaced, 0 when it did not,
 * -1 when the subscription or the publisher's calls failed. */
static int run_round(loopback_pair &lp, int round)
{
    std::string track = "t" + std::to_string(round);
    lp.ss.expect_sub_ns1 = "mvfst";
    lp.ss.expect_sub_ns2 = "reset";
    lp.ss.expect_sub_track = track;
    lp.ss.sub_accepted.store(false);

    moq_subscribe_cfg_t sc;
    moq_subscribe_cfg_init(&sc);
    moq_bytes_t ns[] = {{(const uint8_t *)"mvfst", 5}, {(const uint8_t *)"reset", 5}};
    sc.track_namespace.parts = ns;
    sc.track_namespace.count = 2;
    sc.track_name = {(const uint8_t *)track.data(), track.size()};
    sc.filter = MOQ_SUBSCRIBE_FILTER_NEXT_GROUP;
    moq_subscription_t csub;
    if (moq_session_subscribe(lp.client_session, &sc, 0, &csub) < 0) return -1;
    lp.pump_client();
    bool sub_ok = false;
    if (!lp.wait_for([&]() { return (sub_ok |= client_saw(lp, MOQ_EVENT_SUBSCRIBE_OK)); }))
        return -1;

    std::atomic<bool> ok{true};
    lp.run_on_server([&]() {
        moq_session_t *s = lp.ss.session;
        moq_subgroup_cfg_t sg; moq_subgroup_cfg_init(&sg);
        sg.group_id = 6; sg.publisher_priority = 128;
        moq_subgroup_handle_t h;
        moq_rcbuf_t *b = nullptr;
        moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"object", 6, &b);
        moq_done_subscribe_cfg_t dc; moq_done_subscribe_cfg_init(&dc);
        dc.status_code = 0x2;
        dc.stream_count = 1;
        if (moq_session_open_subgroup(s, lp.ss.sub_handle, &sg, 0, &h) < 0 ||
            moq_session_write_object(s, h, 0, b, 0) < 0 ||
            moq_session_reset_subgroup(s, h, 0x1, 0) < 0 ||
            moq_session_done_subscribe(s, lp.ss.sub_handle, &dc, 0) < 0)
            ok.store(false);
        moq_rcbuf_decref(b);
        lp.service_server();
    });
    if (!ok.load()) return -1;
    bool done = false;
    lp.wait_for([&]() { return (done |= client_saw(lp, MOQ_EVENT_SUBSCRIBE_DONE)); }, 500);
    return done ? 1 : 0;
}

int main(int argc, char **argv)
{
    moq_version_t v = (argc > 1 && std::atoi(argv[1]) == 22) ? MOQ_VERSION_DRAFT_22
                                                              : MOQ_VERSION_DRAFT_16;
    {
        loopback_pair lp(v);
        MVFST_CHECK(lp.init_ok);
        if (lp.init_ok && lp.wait_setup()) {
            int got = 0, broken = 0;
            for (int r = 0; r < ROUNDS; r++) {
                int rr = run_round(lp, r);
                if (rr < 0) broken++;
                else got += rr;
            }
            MVFST_CHECK(broken == 0);
            std::printf("mvfst d%d plain reset: header delivered in %d/%d rounds\n",
                        v == MOQ_VERSION_DRAFT_22 ? 22 : 16, got, ROUNDS);
            MVFST_CHECK(!lp.has_error());
        } else {
            MVFST_CHECK(false && "setup failed");
        }
    }
    if (failures == 0) std::printf("PASS: mvfst_reset_header\n");
    return failures != 0;
}
