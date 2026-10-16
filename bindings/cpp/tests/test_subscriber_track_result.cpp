#include <moq/moq.hpp>
#include "test_support.hpp"

#include <algorithm>
#include <cstdlib>
#include <utility>
#include <vector>

static int failures;

struct allocation_ledger {
    size_t calls = 0;
    size_t blocks = 0;
    size_t bytes = 0;
    bool fail = false;
};

static void *allocate(size_t size, void *ctx)
{
    auto &ledger = *static_cast<allocation_ledger *>(ctx);
    ++ledger.calls;
    if (ledger.fail) return nullptr;
    void *p = std::malloc(size);
    if (p) {
        ++ledger.blocks;
        ledger.bytes += size;
    }
    return p;
}

static void deallocate(void *p, size_t size, void *ctx)
{
    auto &ledger = *static_cast<allocation_ledger *>(ctx);
    if (p) {
        MOQ_CHECK(ledger.blocks > 0 && ledger.bytes >= size);
        --ledger.blocks;
        ledger.bytes -= size;
    }
    std::free(p);
}

static moq::stream_ref peer_ref(moq::stream_ref ref, unsigned from)
{
    constexpr uint64_t bits[] = {UINT64_C(1) << 40, UINT64_C(1) << 41};
    const auto back = bits[from ^ 1];
    return moq::stream_ref((ref.value() & back) ? ref.value() & ~back
                                               : ref.value() | bits[from]);
}

struct session_pair {
    moq::session client;
    moq::session server;
    moq::stream_ref request_stream;

    size_t pump(moq::session &from, moq::session &to, unsigned dir)
    {
        size_t count = 0;
        while (auto action = from.poll_action()) {
            ++count;
            action->visit_extended(
                [&](const moq::action::send_control &a) {
                    MOQ_CHECK(to.on_control_bytes(a.data, 0).ok());
                },
                [&](const moq::action::open_uni_control &a) {
                    MOQ_CHECK(to.on_control_bytes(a.data, 0).ok());
                },
                [&](const moq::action::send_uni_control &a) {
                    MOQ_CHECK(to.on_control_bytes(a.data, 0).ok());
                },
                [&](const moq::action::open_bidi_stream &a) {
                    if (dir == 0) request_stream = a.ref;
                    MOQ_CHECK(to.on_bidi_stream_bytes(peer_ref(a.ref, dir), a.data, a.fin, 0).ok());
                },
                [&](const moq::action::send_bidi_stream &a) {
                    MOQ_CHECK(to.on_bidi_stream_bytes(peer_ref(a.ref, dir), a.data, a.fin, 0).ok());
                },
                [&](const moq::action::close_bidi_stream &a) {
                    MOQ_CHECK(to.on_bidi_stream_bytes(peer_ref(a.ref, dir), {}, true, 0).ok());
                },
                [&](const moq::action::send_data &a) {
                    std::vector<uint8_t> data(a.header.begin(), a.header.end());
                    data.insert(data.end(), a.payload.begin(), a.payload.end());
                    MOQ_CHECK(to.on_data_bytes(peer_ref(a.ref, dir), data, a.fin, 0).ok());
                },
                [&](const moq::extended_action::reset_data &a) {
                    MOQ_CHECK(to.on_data_reset(peer_ref(a.ref, dir), a.error_code, 0).ok());
                },
                [&](const moq::action::stop_data &a) {
                    MOQ_CHECK(to.on_data_stop(peer_ref(a.ref, dir), a.error_code, 0).ok());
                },
                [&](const moq::action::reset_bidi_stream &a) {
                    MOQ_CHECK(to.on_bidi_stream_reset(peer_ref(a.ref, dir), a.error_code, 0).ok());
                },
                [&](const moq::action::stop_bidi_stream &a) {
                    MOQ_CHECK(to.on_bidi_stream_stop(peer_ref(a.ref, dir), a.error_code, 0).ok());
                },
                [&](const moq::action::abort_bidi_stream &a) {
                    MOQ_CHECK(to.on_bidi_stream_reset(peer_ref(a.ref, dir), a.error_code, 0).ok());
                    MOQ_CHECK(to.on_bidi_stream_stop(peer_ref(a.ref, dir), a.error_code, 0).ok());
                },
                [&](const moq::action::send_datagram &a) {
                    MOQ_CHECK(to.on_datagram(a.data, 0).ok());
                },
                [](const auto &) { MOQ_CHECK(false); });
        }
        return count;
    }

    void settle()
    {
        for (unsigned i = 0; i < 32; ++i) {
            const auto sent = pump(client, server, 0);
            if (pump(server, client, 1) + sent == 0) return;
        }
        MOQ_CHECK(false);
    }
};

static uint32_t accept_uri(void *, uint32_t, moq_bytes_t, moq_bytes_t)
{
    return MOQ_URI_ACCEPT;
}

static moq::session make_session(moq_version_t version, moq::perspective role)
{
    moq::session_config cfg{};
    cfg.version = version;
    cfg.perspective = role;
    cfg.send_request_capacity = true;
    cfg.initial_request_capacity = 16;
    auto session = moq::session::create(cfg).value();
    if (session.binding_requirements()) {
        moq::transport_binding_config binding{};
        binding.kind = MOQ_BINDING_RAW_QUIC;
        binding.facts_known = MOQ_BINDING_FACTS_ALL;
        binding.facts_supported = MOQ_BINDING_FACTS_ALL;
        if (role == moq::perspective::client) {
            binding.target_present = MOQ_BINDING_TARGET_COMPLETE;
            binding.authority = "sim.invalid";
        } else {
            binding.accept_uri = accept_uri;
        }
        MOQ_CHECK(session.bind_transport(binding).ok());
    }
    return session;
}

static session_pair establish(moq_version_t version)
{
    session_pair pair{make_session(version, moq::perspective::client),
                      make_session(version, moq::perspective::server), {}};
    MOQ_CHECK(pair.client.start(0).ok());
    if (version != MOQ_VERSION_DRAFT_16) MOQ_CHECK(pair.server.start(0).ok());
    pair.settle();
    MOQ_CHECK(pair.client.state() == moq::session_state::established);
    MOQ_CHECK(pair.server.state() == moq::session_state::established);
    while (pair.client.poll_event()) {}
    while (pair.server.poll_event()) {}
    return pair;
}

static moq::subscriber make_subscriber(moq::session &session, allocation_ledger &ledger)
{
    moq_alloc_t alloc{};
    alloc.ctx = &ledger;
    alloc.alloc = allocate;
    alloc.free = deallocate;
    moq::subscriber_config cfg{};
    cfg.alloc = &alloc;
    cfg.max_tracks = 2;
    auto sub = moq::subscriber::create(session, cfg).value();
    // Neither native ownership nor the C++ copy allocator may borrow this vtable.
    alloc = {};
    return sub;
}

struct request {
    moq::subscriber_track track;
    moq::subscription peer;
};

struct fixture {
    session_pair pair;
    moq::subscriber sub;

    fixture(moq_version_t version, allocation_ledger &ledger)
        : pair(establish(version)), sub(make_subscriber(pair.client, ledger)) {}

    request subscribe()
    {
        auto track = sub.subscribe({.ns = {"ack"}, .track = "video"}, 0).value();
        pair.settle();
        moq::subscription peer;
        while (auto event = pair.server.poll_event()) {
            event->visit([&](const moq::event::subscribe_request &e) { peer = e.sub; },
                         [](const auto &) {});
        }
        MOQ_CHECK(peer.valid());
        return {track, peer};
    }

    void accept(const request &r, const moq::accept_subscribe_config &cfg = {})
    {
        MOQ_CHECK(pair.server.accept_subscribe(r.peer, cfg, 0).ok());
        pair.settle();
    }

    void release(moq::subscriber_track track)
    {
        MOQ_CHECK(sub.unsubscribe(track, 0).ok());
        MOQ_CHECK(moq_sub_release_track(sub.raw(), track.raw()) == MOQ_OK);
    }
};

static std::span<const uint8_t> properties(moq_version_t version)
{
    static constexpr uint8_t d16[] = {0x40, 0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb};
    static constexpr uint8_t modern[] = {0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb};
    return version == MOQ_VERSION_DRAFT_16 ? std::span<const uint8_t>(d16) : modern;
}

static void check_bytes(const moq::buffer &buffer, std::span<const uint8_t> expected)
{
    MOQ_CHECK(std::equal(buffer.span().begin(), buffer.span().end(),
                         expected.begin(), expected.end()));
}

static void test_presence_and_refusals(moq_version_t version)
{
    allocation_ledger ledger, other_ledger;
    {
        fixture f(version, ledger), other(version, other_ledger);
        const auto r = f.subscribe();
        const auto calls = ledger.calls;
        auto pending = std::as_const(f.sub).get_track_result(r.track).value();
        MOQ_CHECK(pending.state == moq::sub_track_state::pending);
        MOQ_CHECK(!pending.response_seen && !pending.largest && !pending.expires_ms);
        MOQ_CHECK(pending.track_properties.empty());
        MOQ_CHECK(ledger.calls == calls);

        auto wrong_owner = other.sub.get_track_result(r.track);
        MOQ_CHECK(!wrong_owner && wrong_owner.error().code() == moq::errc::invalid);
        auto invalid = f.sub.get_track_result({});
        MOQ_CHECK(!invalid && invalid.error().code() == moq::errc::invalid);
        auto empty_owner = moq::subscriber{}.get_track_result(r.track);
        MOQ_CHECK(!empty_owner && empty_owner.error().code() == moq::errc::invalid);

        f.accept(r);
        auto queued = f.sub.get_track_result(r.track).value();
        MOQ_CHECK(!queued.response_seen && queued.state == moq::sub_track_state::pending);
        MOQ_CHECK(f.sub.tick(0).ok());
        auto empty = f.sub.get_track_result(r.track).value();
        MOQ_CHECK(empty.response_seen && empty.state == moq::sub_track_state::active);
        MOQ_CHECK(!empty.largest && !empty.expires_ms && empty.track_properties.empty());
        MOQ_CHECK(ledger.calls == calls);
        MOQ_CHECK(f.sub.unsubscribe(r.track, 0).ok());
        auto done = f.sub.get_track_result(r.track).value();
        MOQ_CHECK(done.state == moq::sub_track_state::done && done.response_seen);
        MOQ_CHECK(moq_sub_release_track(f.sub.raw(), r.track.raw()) == MOQ_OK);
    }
    MOQ_CHECK(ledger.blocks == 0 && ledger.bytes == 0);
    MOQ_CHECK(other_ledger.blocks == 0 && other_ledger.bytes == 0);
}

static void test_owned_snapshot(moq_version_t version, bool release_track)
{
    allocation_ledger ledger;
    moq::sub_track_result held;
    const auto expected = properties(version);
    {
        fixture f(version, ledger);
        const auto r = f.subscribe();
        std::vector<uint8_t> input(expected.begin(), expected.end());
        moq::accept_subscribe_config cfg{};
        cfg.has_largest = true;
        cfg.has_expires = true;
        cfg.track_properties = moq::bytes_view(input.data(), input.size());
        f.accept(r, cfg);
        std::fill(input.begin(), input.end(), 0);
        MOQ_CHECK(f.sub.tick(0).ok());

        const auto calls = ledger.calls;
        auto moved = std::move(f.sub);
        held = std::as_const(moved).get_track_result(r.track).value();
        MOQ_CHECK(ledger.calls == calls + 1);
        MOQ_CHECK(held.response_seen && held.state == moq::sub_track_state::active);
        MOQ_CHECK(held.largest && held.largest->group == 0 && held.largest->object == 0);
        MOQ_CHECK(held.expires_ms.has_value() == (version != MOQ_VERSION_DRAFT_16));
        if (held.expires_ms) MOQ_CHECK(*held.expires_ms == 0);
        check_bytes(held.track_properties, expected);
        auto invalid = f.sub.get_track_result(r.track);
        MOQ_CHECK(!invalid && invalid.error().code() == moq::errc::invalid);
        f.sub = std::move(moved);

        moq_sub_track_result_t native{};
        MOQ_CHECK(moq_sub_get_track_result(f.sub.raw(), r.track.raw(), &native, sizeof(native)) == MOQ_OK);
        MOQ_CHECK(held.track_properties.data() != native.track_properties.data);
        auto copy = held;
        auto moved_copy = std::move(copy);
        MOQ_CHECK(moq_rcbuf_refcount(held.track_properties.raw()) == 2);
        check_bytes(moved_copy.track_properties, expected);

        auto payload = moq::buffer::create(reinterpret_cast<const uint8_t *>("body"), 4).value();
        auto stream = f.pair.server.open_subgroup(r.peer, {.group_id = 1}, 0).value();
        MOQ_CHECK(f.pair.server.write_object(stream, 2, payload, 0).ok());
        MOQ_CHECK(f.pair.server.close_subgroup(stream, 0).ok());
        f.pair.settle();
        MOQ_CHECK(f.sub.tick(0).ok());
        {
            auto object = f.sub.poll_object();
            MOQ_CHECK(object && object->group_id() == 1 && object->object_id() == 2);
            auto after = f.sub.get_track_result(r.track).value();
            MOQ_CHECK(after.largest && after.largest->group == 0 && after.largest->object == 0);
            check_bytes(after.track_properties, expected);
        }
        if (version != MOQ_VERSION_DRAFT_16) {
            MOQ_CHECK(f.sub.update_subscription(r.track,
                {.has_forward = true, .forward = false}, 0).ok());
            f.pair.settle();
            MOQ_CHECK(f.sub.tick(0).ok());
            auto after_update = f.sub.get_track_result(r.track).value();
            MOQ_CHECK(after_update.largest && after_update.largest->group == 0 &&
                      after_update.largest->object == 0);
            MOQ_CHECK(after_update.expires_ms == held.expires_ms);
            check_bytes(after_update.track_properties, expected);
        }
        if (release_track) f.release(r.track);
        check_bytes(held.track_properties, expected);
    }
    MOQ_CHECK(ledger.blocks == 1);
    check_bytes(held.track_properties, expected);
    held = {};
    MOQ_CHECK(ledger.blocks == 0 && ledger.bytes == 0);
}

static void test_local_copy_failure(moq_version_t version)
{
    allocation_ledger ledger;
    {
        fixture f(version, ledger);
        const auto r = f.subscribe();
        const auto bytes = properties(version);
        moq::accept_subscribe_config cfg{};
        cfg.track_properties = moq::bytes_view(bytes.data(), bytes.size());
        f.accept(r, cfg);
        MOQ_CHECK(f.sub.tick(0).ok());
        const auto calls = ledger.calls, blocks = ledger.blocks;
        ledger.fail = true;
        auto failed = f.sub.get_track_result(r.track);
        MOQ_CHECK(!failed && failed.error().code() == moq::errc::nomem);
        MOQ_CHECK(ledger.calls == calls + 1 && ledger.blocks == blocks);
        ledger.fail = false;
        auto retry = f.sub.get_track_result(r.track).value();
        MOQ_CHECK(retry.response_seen && retry.state == moq::sub_track_state::active);
        check_bytes(retry.track_properties, bytes);
        MOQ_CHECK(ledger.calls == calls + 2 && ledger.blocks == blocks + 1);
    }
    MOQ_CHECK(ledger.blocks == 0 && ledger.bytes == 0);
}

static void test_native_failure(moq_version_t version)
{
    allocation_ledger ledger;
    {
        fixture f(version, ledger);
        const auto r = f.subscribe();
        const auto bytes = properties(version);
        moq::accept_subscribe_config cfg{};
        cfg.track_properties = moq::bytes_view(bytes.data(), bytes.size());
        f.accept(r, cfg);
        ledger.fail = true;
        auto tick = f.sub.tick(0);
        MOQ_CHECK(!tick && tick.error().code() == moq::errc::nomem);
        ledger.fail = false;
        const auto calls = ledger.calls;
        auto failed = f.sub.get_track_result(r.track);
        MOQ_CHECK(!failed && failed.error().code() == moq::errc::nomem);
        MOQ_CHECK(ledger.calls == calls);

        // Terminal progress does not turn the latched query error into success.
        MOQ_CHECK(moq_session_on_transport_close(f.pair.client.raw(), 0, 0) == MOQ_OK);
        MOQ_CHECK(f.sub.tick(0).ok());
        auto terminal = f.sub.get_track_result(r.track);
        MOQ_CHECK(!terminal && terminal.error().code() == moq::errc::nomem);
        MOQ_CHECK(ledger.calls == calls);
        MOQ_CHECK(moq_sub_release_track(f.sub.raw(), r.track.raw()) == MOQ_OK);
    }
    MOQ_CHECK(ledger.blocks == 0 && ledger.bytes == 0);
}

static void test_rejected(moq_version_t version)
{
    allocation_ledger ledger;
    {
        fixture f(version, ledger);
        const auto r = f.subscribe();
        moq::reject_subscribe_config cfg{};
        cfg.error_code = moq::request_error::does_not_exist;
        MOQ_CHECK(f.pair.server.reject_subscribe(r.peer, cfg, 0).ok());
        f.pair.settle();
        MOQ_CHECK(f.sub.tick(0).ok());
        auto rejected = f.sub.get_track_result(r.track).value();
        MOQ_CHECK(rejected.state == moq::sub_track_state::error);
        MOQ_CHECK(!rejected.response_seen && !rejected.largest && !rejected.expires_ms);
        MOQ_CHECK(rejected.track_properties.empty());
        MOQ_CHECK(moq_sub_release_track(f.sub.raw(), r.track.raw()) == MOQ_OK);
    }
    MOQ_CHECK(ledger.blocks == 0 && ledger.bytes == 0);
}

static void test_full_width_wire_ack()
{
    allocation_ledger ledger;
    {
        fixture f(MOQ_VERSION_DRAFT_22, ledger);
        const auto r = f.subscribe();
        // Independent REQUEST_OK wire input, as in the native initial-result test.
        // Receive-side EXPIRES exceeds the public sender's separate expiry ceiling.
        constexpr uint8_t bytes[] = {
            0x04, 0x00, 0x26, 0x01, 0x02, 0x08,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0x01,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff,
            0x78, 0x01, 0x01, 0x03, 0xaa, 0x00, 0xbb
        };
        MOQ_CHECK(f.pair.client.on_bidi_stream_bytes(f.pair.request_stream, bytes, false, 0).ok());
        MOQ_CHECK(f.sub.tick(0).ok());
        auto result = f.sub.get_track_result(r.track).value();
        MOQ_CHECK(result.response_seen && result.state == moq::sub_track_state::active);
        MOQ_CHECK(result.largest && result.largest->group == UINT64_MAX &&
                  result.largest->object == UINT64_MAX);
        MOQ_CHECK(result.expires_ms == UINT64_MAX);
        check_bytes(result.track_properties, properties(MOQ_VERSION_DRAFT_22));
    }
    MOQ_CHECK(ledger.blocks == 0 && ledger.bytes == 0);
}

static void test_default_allocator()
{
    allocation_ledger old_ledger;
    moq::sub_track_result held;
    const auto bytes = properties(MOQ_VERSION_DRAFT_22);
    {
        fixture f(MOQ_VERSION_DRAFT_22, old_ledger);
        f.sub = moq::subscriber::create(f.pair.client).value();
        MOQ_CHECK(old_ledger.blocks == 0 && old_ledger.bytes == 0);
        const auto r = f.subscribe();
        moq::accept_subscribe_config cfg{};
        cfg.track_properties = moq::bytes_view(bytes.data(), bytes.size());
        f.accept(r, cfg);
        MOQ_CHECK(f.sub.tick(0).ok());
        old_ledger.fail = true;
        const auto calls = old_ledger.calls;
        held = f.sub.get_track_result(r.track).value();
        MOQ_CHECK(held.response_seen && old_ledger.calls == calls);
    }
    check_bytes(held.track_properties, bytes);
    MOQ_CHECK(old_ledger.blocks == 0 && old_ledger.bytes == 0);
}

int main()
{
    for (auto version : {MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22}) {
        test_presence_and_refusals(version);
        test_owned_snapshot(version, true);
        test_owned_snapshot(version, false);
        test_local_copy_failure(version);
        test_native_failure(version);
        test_rejected(version);
    }
    test_full_width_wire_ack();
    test_default_allocator();
    MOQ_PASS("subscriber_track_result (20 cases)");
    return failures != 0;
}
