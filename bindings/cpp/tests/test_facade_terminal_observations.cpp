#include <moq/publisher.hpp>
#include <moq/subscriber.hpp>
#include <array>
#include <cstdio>
#include <cstring>
#include <memory>

static unsigned failures;
#define CHECK(x) do { if (!(x)) { std::fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); ++failures; } } while (0)

static void prefixes()
{
    moq_pub_namespace_terminal_info_t info{};
    info.struct_size = sizeof(info); info.kind = MOQ_PUB_NAMESPACE_TERMINATED;
    info.cause = MOQ_NAMESPACE_TERMINAL_TRUNCATED_MESSAGE;
    info.local_role = MOQ_NAMESPACE_ROLE_REQUESTER; info.stream_error_code = UINT64_MAX;
    std::array<unsigned char, sizeof(info) + 8> bytes{};
    std::memcpy(bytes.data(), &info, sizeof(info));
    for (size_t n = 0; n < sizeof(info); ++n) {
        auto physical = std::make_unique<unsigned char[]>(n);
        if (n) std::memcpy(physical.get(), bytes.data(), n);
        auto got = moq::pub_namespace_terminal_view::from_native(physical.get(), n);
        CHECK(!got && got.error().code() == moq::errc::abi_mismatch);
        auto stamped = info; stamped.struct_size = static_cast<uint32_t>(n);
        got = moq::pub_namespace_terminal_view::from_native(&stamped, sizeof(stamped));
        CHECK(!got && got.error().code() == moq::errc::abi_mismatch);
    }
    auto full = moq::pub_namespace_terminal_view::from_native(bytes.data(), bytes.size());
    CHECK(full.ok());
    CHECK(full->cause == moq::namespace_terminal_cause::truncated_message);
    CHECK(full->local_role == moq::request_role::requester && full->stream_error_code == UINT64_MAX);
    auto future = static_cast<uint32_t>(bytes.size());
    std::memcpy(bytes.data(), &future, sizeof(future));
    CHECK(moq::pub_namespace_terminal_view::from_native(bytes.data(), bytes.size()).ok());
    info.kind = MOQ_PUB_NAMESPACE_REJECTED;
    info.struct_size = MOQ_PUB_NAMESPACE_TERMINAL_INFO_V0_SIZE;
    info.error_code = MOQ_REQUEST_ERROR_UNAUTHORIZED;
    std::memcpy(bytes.data(), &info, sizeof(info));
    auto old = moq::pub_namespace_terminal_view::from_native(bytes.data(), info.struct_size);
    CHECK(old.ok() && !old->cause && !old->local_role && old->stream_error_code == 0);
    CHECK(old->error_code == moq::request_error::unauthorized);
    moq_sub_status_outcome_t status{};
    status.struct_size = sizeof(status); status.kind = MOQ_SUB_STATUS_OUTCOME_TERMINATED;
    status.cause = MOQ_TRACK_STATUS_PEER_STOP_SENDING;
    status.local_role = MOQ_TRACK_STATUS_REQUESTER; status.stream_error_code = UINT64_MAX;
    for (size_t n = 0; n < sizeof(status); ++n) {
        auto physical = std::make_unique<unsigned char[]>(n);
        if (n) std::memcpy(physical.get(), &status, n);
        auto got = moq::sub_status_outcome::from_native(physical.get(), n);
        CHECK(!got && got.error().code() == moq::errc::abi_mismatch);
        auto stamped = status; stamped.struct_size = static_cast<uint32_t>(n);
        got = moq::sub_status_outcome::from_native(&stamped, sizeof(stamped));
        CHECK(!got && got.error().code() == moq::errc::abi_mismatch);
    }
    auto got = moq::sub_status_outcome::from_native(&status, sizeof(status));
    CHECK(got.ok() && got->cause == moq::track_status_terminal_cause::peer_stop_sending);
    CHECK(got->stream_error_code == UINT64_MAX && !got->largest && !got->expires_ms);
    status.kind = MOQ_SUB_STATUS_OUTCOME_OK;
    status.flags = MOQ_SUB_STATUS_OUTCOME_HAS_LARGEST | MOQ_SUB_STATUS_OUTCOME_HAS_EXPIRES;
    got = moq::sub_status_outcome::from_native(&status, sizeof(status));
    CHECK(got.ok() && got->largest && got->expires_ms == 0 && !got->cause);
    status.kind = MOQ_SUB_STATUS_OUTCOME_ERROR;
    status.error_code = UINT64_MAX; status.flags = MOQ_SUB_STATUS_OUTCOME_CAN_RETRY;
    got = moq::sub_status_outcome::from_native(&status, sizeof(status));
    CHECK(got.ok() && got->can_retry && got->retry_after_ms == 0);
    CHECK(static_cast<uint64_t>(got->error_code) == UINT64_MAX && !got->largest && !got->expires_ms && !got->cause);
    std::array<unsigned char, sizeof(status) + 8> status_future{};
    status.struct_size = static_cast<uint32_t>(status_future.size());
    std::memcpy(status_future.data(), &status, sizeof(status));
    CHECK(moq::sub_status_outcome::from_native(status_future.data(), status_future.size()).ok());
}

static uint32_t accept_uri(void *, uint32_t, moq_bytes_t, moq_bytes_t) { return MOQ_URI_ACCEPT; }
static moq::session make_session(moq::perspective role)
{
    moq::session_config cfg{};
    cfg.version = MOQ_VERSION_DRAFT_22; cfg.perspective = role;
    cfg.send_request_capacity = true; cfg.initial_request_capacity = 128;
    cfg.max_track_statuses = 32;
    auto s = moq::session::create(cfg).value();
    moq::transport_binding_config binding{};
    binding.kind = MOQ_BINDING_RAW_QUIC;
    binding.facts_known = binding.facts_supported = MOQ_BINDING_FACTS_ALL;
    if (role == moq::perspective::client) {
        binding.target_present = MOQ_BINDING_TARGET_COMPLETE; binding.authority = "test.invalid";
    } else binding.accept_uri = accept_uri;
    CHECK(s.bind_transport(binding).ok());
    return s;
}
struct pair {
    moq::session client = make_session(moq::perspective::client);
    moq::session server = make_session(moq::perspective::server);
    moq::stream_ref request;
    static moq::stream_ref peer(moq::stream_ref r, unsigned from)
    {
        const uint64_t bits[] = {UINT64_C(1) << 40, UINT64_C(1) << 41};
        return moq::stream_ref(r.value() & bits[from ^ 1] ? r.value() & ~bits[from ^ 1] : r.value() | bits[from]);
    }
    unsigned pump(moq::session &from, moq::session &to, unsigned dir)
    {
        for (unsigned i = 0; i < 128; ++i) {
            auto a = from.poll_action();
            if (!a) return i;
            a->visit_extended(
                [&](const moq::action::open_uni_control &v) { CHECK(to.on_control_bytes(v.data, 1).ok()); },
                [&](const moq::action::send_uni_control &v) { CHECK(to.on_control_bytes(v.data, 1).ok()); },
                [&](const moq::action::send_control &v) { CHECK(to.on_control_bytes(v.data, 1).ok()); },
                [&](const moq::action::open_bidi_stream &v) {
                    if (!dir) request = v.ref;
                    CHECK(to.on_bidi_stream_bytes(peer(v.ref, dir), v.data, v.fin, 1).ok());
                },
                [&](const moq::action::send_bidi_stream &v) { CHECK(to.on_bidi_stream_bytes(peer(v.ref, dir), v.data, v.fin, 1).ok()); },
                [&](const moq::action::close_bidi_stream &v) { CHECK(to.on_bidi_stream_bytes(peer(v.ref, dir), {}, true, 1).ok()); },
                [&](const moq::action::reset_bidi_stream &v) { CHECK(to.on_bidi_stream_reset(peer(v.ref, dir), v.error_code, 1).ok()); },
                [&](const moq::action::stop_bidi_stream &v) { CHECK(to.on_bidi_stream_stop(peer(v.ref, dir), v.error_code, 1).ok()); },
                [](const auto &) { CHECK(false); });
        }
        CHECK(false); return 128;
    }
    void settle()
    {
        for (unsigned i = 0; i < 32; ++i) {
            auto n = pump(client, server, 0); n += pump(server, client, 1);
            if (!n) return;
        }
        CHECK(false);
    }
    pair()
    {
        CHECK(client.start(1).ok()); CHECK(server.start(1).ok()); settle();
        while (client.poll_event()) {}
        while (server.poll_event()) {}
        CHECK(client.state() == moq::session_state::established);
    }
};

static void subscriber_route(uint64_t code)
{
    pair p;
    auto sub = moq::subscriber::create(p.client).value();
    const moq::sub_status_config cfg{.ns = {"n24"}, .track = "v"};
    std::array<moq_sub_status_req_t *, 16> tokens{};
    for (auto &token : tokens) {
        token = sub.track_status(cfg, 1).value().raw(); p.settle();
        while (p.server.poll_event()) {}
        CHECK(p.client.on_bidi_stream_reset(p.request, code, 1).ok());
        CHECK(sub.tick(1).ok());
    }
    CHECK(!sub.track_status(cfg, 1));
    CHECK(!sub.poll_status() && !sub.poll_status());
    CHECK(!sub.track_status(cfg, 1));
    auto first = sub.poll_status_outcome().value();
    CHECK(first && first->request.raw() == tokens[0]);
    CHECK(first->kind == moq::sub_status_outcome_kind::terminated);
    CHECK(first->cause == moq::track_status_terminal_cause::peer_reset_stream);
    CHECK(first->local_role == moq::request_role::requester && first->stream_error_code == code);
    CHECK(!first->largest && !first->expires_ms && !first->can_retry && first->retry_after_ms == 0);
    auto reused = sub.track_status(cfg, 1).value(); CHECK(reused.raw() == tokens[0]); p.settle();
    moq_event_t e{};
    CHECK(moq_session_poll_events(p.server.raw(), &e, 1) == 1);
    CHECK(e.kind == MOQ_EVENT_TRACK_STATUS_REQUEST);
    moq_accept_track_status_cfg_t ac; moq_accept_track_status_cfg_init(&ac);
    ac.has_largest = true;
    CHECK(moq_session_accept_track_status(p.server.raw(), e.u.track_status_request.handle, &ac, 1) == MOQ_OK);
    moq_event_cleanup(&e); p.settle(); CHECK(sub.tick(1).ok());
    for (size_t i = 1; i < tokens.size(); ++i) {
        auto got = sub.poll_status_outcome().value();
        CHECK(got && got->request.raw() == tokens[i] && got->stream_error_code == code);
    }
    auto ok = sub.poll_status_outcome().value();
    CHECK(ok && ok->kind == moq::sub_status_outcome_kind::ok && ok->request.raw() == reused.raw());
    CHECK(ok->largest && ok->largest->group == 0 && ok->largest->object == 0 && !ok->expires_ms);
    CHECK(!sub.poll_status_outcome().value());
}

struct observed {
    unsigned count = 0;
    uint64_t code = 0;
    bool stop = false;
};
static void callback(void *ctx, moq::result<moq::pub_namespace_terminal_view> got) noexcept
{
    auto &o = *static_cast<observed *>(ctx); ++o.count;
    CHECK(got.ok()); if (!got) return;
    CHECK(got->kind == moq::pub_namespace_terminal_kind::terminated);
    CHECK(got->cause == (o.stop ? moq::namespace_terminal_cause::peer_stop_sending : moq::namespace_terminal_cause::peer_reset_stream));
    CHECK(got->local_role == moq::request_role::requester && got->stream_error_code == o.code);
    CHECK(got->namespace_parts.size() == 1 && got->namespace_parts[0].len == 3);
    CHECK(static_cast<uint64_t>(got->error_code) == 0 && !got->can_retry && got->reason.empty());
}
static void publisher_route(bool accepted, bool stop, uint64_t code)
{
    pair p; observed seen{0, code, stop}; moq::publisher_observer observer{&seen, callback};
    auto original = moq::publisher::create_observed(p.client, observer).value();
    auto pub = std::move(original);
    auto track = pub.add_track({.ns = {"n24"}, .track = "v", .advertise_namespace = true}, 1).value();
    p.settle();
    moq_event_t e{}; CHECK(moq_session_poll_events(p.server.raw(), &e, 1) == 1);
    CHECK(e.kind == MOQ_EVENT_NAMESPACE_PUBLISHED);
    if (accepted) {
        moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
        CHECK(moq_session_accept_namespace(p.server.raw(), e.u.namespace_published.ann, &ac, 1) == MOQ_OK);
    }
    moq_event_cleanup(&e); p.settle(); CHECK(pub.tick(1).ok());
    CHECK((stop ? p.client.on_bidi_stream_stop(p.request, code, 1) : p.client.on_bidi_stream_reset(p.request, code, 1)).ok());
    CHECK(pub.tick(1).ok()); CHECK(pub.tick(1).ok()); CHECK(seen.count == 1);
    CHECK(pub.remove_track(track, 1).ok());
    CHECK(p.client.state() == moq::session_state::established);
}
int main()
{
    prefixes();
    subscriber_route(0); subscriber_route(UINT64_MAX);
    for (unsigned a = 0; a < 2; ++a) for (unsigned s = 0; s < 2; ++s) for (unsigned c = 0; c < 2; ++c)
        publisher_route(a != 0, s != 0, c ? UINT64_MAX : 0);
    std::printf("facade_terminal_observations: %u failures\n", failures);
    return failures ? 1 : 0;
}
