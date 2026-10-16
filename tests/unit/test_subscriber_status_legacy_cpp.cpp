#include <moq/moq.hpp>

#include <array>
#include <cstdio>
#include <cstring>

static unsigned failures;
#define CHECK(x) do { if (!(x)) { \
    std::fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); ++failures; \
} } while (0)

static moq::stream_ref peer_ref(moq::stream_ref ref, unsigned from)
{
    constexpr uint64_t bits[] = {UINT64_C(1) << 40, UINT64_C(1) << 41};
    const auto back = bits[from ^ 1];
    return moq::stream_ref((ref.value() & back) ? ref.value() & ~back
                                               : ref.value() | bits[from]);
}

struct pair {
    moq::session client, server;
    moq::stream_ref request_stream;

    size_t pump(moq::session &from, moq::session &to, unsigned dir)
    {
        for (size_t i = 0; i < 128; ++i) {
            auto a = from.poll_action();
            if (!a) return i;
            a->visit_extended(
                [&](const moq::action::send_control &v) { CHECK(to.on_control_bytes(v.data, 1).ok()); },
                [&](const moq::action::open_uni_control &v) { CHECK(to.on_control_bytes(v.data, 1).ok()); },
                [&](const moq::action::send_uni_control &v) { CHECK(to.on_control_bytes(v.data, 1).ok()); },
                [&](const moq::action::open_bidi_stream &v) {
                    if (dir == 0) request_stream = v.ref;
                    CHECK(to.on_bidi_stream_bytes(peer_ref(v.ref, dir), v.data, v.fin, 1).ok());
                },
                [&](const moq::action::send_bidi_stream &v) {
                    CHECK(to.on_bidi_stream_bytes(peer_ref(v.ref, dir), v.data, v.fin, 1).ok());
                },
                [&](const moq::action::close_bidi_stream &v) {
                    CHECK(to.on_bidi_stream_bytes(peer_ref(v.ref, dir), {}, true, 1).ok());
                },
                [&](const moq::action::reset_bidi_stream &v) {
                    CHECK(to.on_bidi_stream_reset(peer_ref(v.ref, dir), v.error_code, 1).ok());
                },
                [&](const moq::action::stop_bidi_stream &v) {
                    CHECK(to.on_bidi_stream_stop(peer_ref(v.ref, dir), v.error_code, 1).ok());
                },
                [](const auto &) { CHECK(false); });
        }
        CHECK(false);
        return 128;
    }

    void settle()
    {
        for (unsigned i = 0; i < 32; ++i) {
            auto count = pump(client, server, 0);
            count += pump(server, client, 1);
            if (!count) return;
        }
        CHECK(false);
    }
};

static uint32_t accept_uri(void *, uint32_t, moq_bytes_t, moq_bytes_t)
{
    return MOQ_URI_ACCEPT;
}

static moq::session make_session(moq::perspective role)
{
    moq::session_config cfg{};
    cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.perspective = role;
    cfg.send_request_capacity = true;
    cfg.initial_request_capacity = 128;
    cfg.max_track_statuses = 32;
    auto session = moq::session::create(cfg).value();
    moq::transport_binding_config binding{};
    binding.kind = MOQ_BINDING_RAW_QUIC;
    binding.facts_known = MOQ_BINDING_FACTS_ALL;
    binding.facts_supported = MOQ_BINDING_FACTS_ALL;
    if (role == moq::perspective::client) {
        binding.target_present = MOQ_BINDING_TARGET_COMPLETE;
        binding.authority = "sim.invalid";
    } else binding.accept_uri = accept_uri;
    CHECK(session.bind_transport(binding).ok());
    return session;
}

static void drain(moq::session &session)
{
    for (unsigned i = 0; i < 128; ++i) if (!session.poll_event()) return;
    CHECK(false);
}

static void run(uint64_t code)
{
    pair p{make_session(moq::perspective::client), make_session(moq::perspective::server), {}};
    CHECK(p.client.start(1).ok());
    CHECK(p.server.start(1).ok());
    p.settle();
    CHECK(p.client.state() == moq::session_state::established);
    drain(p.client);
    drain(p.server);
    auto sub = moq::subscriber::create(p.client).value();
    const moq::sub_status_config cfg{.ns = {"status"}, .track = "video"};
    std::array<moq_sub_status_req_t *, 16> tokens{};
    for (auto &token : tokens) {
        token = sub.track_status(cfg, 1).value().raw();
        p.settle();
        drain(p.server);
        CHECK(p.request_stream.value() != 0);
        CHECK(p.client.on_bidi_stream_reset(p.request_stream, code, 1).ok());
        CHECK(sub.tick(1).ok());
    }
    auto full = sub.track_status(cfg, 1);
    CHECK(!full && full.error().code() == moq::errc::would_block);
    CHECK(!sub.poll_status().has_value());
    CHECK(!sub.poll_status().has_value());
    full = sub.track_status(cfg, 1);
    CHECK(!full && full.error().code() == moq::errc::would_block);

    moq_sub_status_outcome_t out;
    std::memset(&out, 0xa5, sizeof(out));
    CHECK(moq_sub_poll_status_outcome(sub.raw(), &out, sizeof(out)) == MOQ_OK);
    CHECK(out.kind == MOQ_SUB_STATUS_OUTCOME_TERMINATED && out.request == tokens[0]);
    CHECK(out.cause == MOQ_TRACK_STATUS_PEER_RESET_STREAM);
    CHECK(out.local_role == MOQ_TRACK_STATUS_REQUESTER && out.stream_error_code == code);
    CHECK(out.flags == 0 && out.error_code == 0 && out.retry_after_ms == 0);
    auto reused = sub.track_status(cfg, 1).value();
    CHECK(reused.raw() == tokens[0]);
    p.settle();
    moq_track_status_handle_t peer = MOQ_TRACK_STATUS_HANDLE_INVALID;
    unsigned requests = 0;
    for (unsigned i = 0; i < 128; ++i) {
        moq_event_t e;
        size_t count = 0;
        CHECK(moq_session_poll_events_ex(p.server.raw(), &e, 1, sizeof(e), &count) == MOQ_OK);
        if (!count) break;
        if (e.kind == MOQ_EVENT_TRACK_STATUS_REQUEST) { ++requests; peer = e.u.track_status_request.handle; }
        moq_event_cleanup(&e);
    }
    CHECK(requests == 1);
    moq_accept_track_status_cfg_t response;
    moq_accept_track_status_cfg_init(&response);
    CHECK(moq_session_accept_track_status(p.server.raw(), peer, &response, 1) == MOQ_OK);
    p.settle();
    CHECK(sub.tick(1).ok());
    for (size_t i = 1; i < tokens.size(); ++i) {
        CHECK(moq_sub_poll_status_outcome(sub.raw(), &out, sizeof(out)) == MOQ_OK);
        CHECK(out.kind == MOQ_SUB_STATUS_OUTCOME_TERMINATED && out.request == tokens[i]);
        CHECK(out.stream_error_code == code);
    }
    auto old_ok = sub.poll_status();
    CHECK(old_ok.has_value());
    if (old_ok) CHECK(old_ok->request.raw() == reused.raw());
    CHECK(!sub.poll_status().has_value());
    CHECK(moq_sub_poll_status_outcome(sub.raw(), &out, sizeof(out)) == MOQ_DONE);
    std::printf("legacy C++ reset=%llu: refusal, same-head consume, reuse\n",
                static_cast<unsigned long long>(code));
}

int main()
{
    run(0);
    run(UINT64_MAX);
    std::printf("subscriber_status_legacy_cpp: 2 cases, %u failures\n", failures);
    return failures ? 1 : 0;
}
