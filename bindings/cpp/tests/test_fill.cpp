// Fills (draft-22 section 3.4), publisher state notifications and the
// transport binding through the C++ binding: the subscribe config's location
// and fill are copied by value and reach the wire; the publisher sees
// fill_request, answers with the six fill operations, and the subscriber's
// fill_object keeps its buffers alive past the polled_event; fill_terminal
// carries reason, domain and code; a draft without fills refuses before
// anything is sent; publish_state round-trips; a transport binding is a
// plain value the session copies.
#include <moq/moq.hpp>
#include "test_support.hpp"

#include <cstring>
#include <optional>
#include <utility>
#include <vector>

// Streams are delivered to the peer under a ref of the peer's own: the
// opener's ref plus a per-direction offset, and a reply on that ref comes
// back without it. Uni-control bytes are control bytes; a request-stream
// abort reaches the peer as both a reset and a stop.
static constexpr uint64_t k_peer_bit[2] = {UINT64_C(1) << 40, UINT64_C(1) << 41};

static moq::stream_ref to_peer(moq::stream_ref ref, int from)
{
    const uint64_t v = ref.value();
    const uint64_t back = k_peer_bit[from ^ 1];
    if (v & back) return moq::stream_ref(v & ~back);   // reply on a peer-opened stream
    return moq::stream_ref(v | k_peer_bit[from]);
}

static void pump_all(moq::session &from, moq::session &to, uint64_t now, int dir)
{
    while (auto a = from.poll_action()) {
        a->visit_extended(
            [&](const moq::action::send_control &sc) {
                to.on_control_bytes(sc.data, now);
            },
            [&](const moq::action::open_uni_control &uc) {
                to.on_control_bytes(uc.data, now);
            },
            [&](const moq::action::send_uni_control &uc) {
                to.on_control_bytes(uc.data, now);
            },
            [&](const moq::action::send_data &sd) {
                std::vector<uint8_t> buf;
                buf.insert(buf.end(), sd.header.begin(), sd.header.end());
                buf.insert(buf.end(), sd.payload.begin(), sd.payload.end());
                to.on_data_bytes(to_peer(sd.ref, dir), buf, sd.fin, now);
            },
            [&](const moq::extended_action::reset_data &rd) {
                to.on_data_reset(to_peer(rd.ref, dir), rd.error_code, now);
            },
            [&](const moq::action::stop_data &sd) {
                to.on_data_stop(to_peer(sd.ref, dir), sd.error_code, now);
            },
            [&](const moq::action::open_bidi_stream &bs) {
                to.on_bidi_stream_bytes(to_peer(bs.ref, dir), bs.data, bs.fin, now);
            },
            [&](const moq::action::send_bidi_stream &bs) {
                to.on_bidi_stream_bytes(to_peer(bs.ref, dir), bs.data, bs.fin, now);
            },
            [&](const moq::action::reset_bidi_stream &rb) {
                to.on_bidi_stream_reset(to_peer(rb.ref, dir), rb.error_code, now);
            },
            [&](const moq::action::stop_bidi_stream &sb) {
                to.on_bidi_stream_stop(to_peer(sb.ref, dir), sb.error_code, now);
            },
            [&](const moq::action::abort_bidi_stream &ab) {
                to.on_bidi_stream_reset(to_peer(ab.ref, dir), ab.error_code, now);
                to.on_bidi_stream_stop(to_peer(ab.ref, dir), ab.error_code, now);
            },
            [&](const moq::action::send_datagram &dg) {
                to.on_datagram(dg.data, now);
            },
            [](const auto &) {});
    }
}

static void settle(moq::session &c, moq::session &s, uint64_t now)
{
    for (int i = 0; i < 6; i++) {
        pump_all(c, s, now, 0);
        pump_all(s, c, now, 1);
    }
}

struct session_pair {
    moq::session client;
    moq::session server;
};

static uint32_t accept_any_uri(void *, uint32_t, moq_bytes_t, moq_bytes_t)
{
    return MOQ_URI_ACCEPT;
}

// A profile that needs transport facts before its first message (draft 22
// over raw QUIC) gets them here; the client names a target, the server a
// URI policy. A profile that needs none starts unbound, so none is sent.
static void bind_if_required(int &failures, moq::session &s, bool client)
{
    if (s.binding_requirements() == 0) return;
    moq::transport_binding_config b{};
    b.kind            = MOQ_BINDING_RAW_QUIC;
    b.facts_known     = MOQ_BINDING_FACTS_ALL;
    b.facts_supported = MOQ_BINDING_FACTS_ALL;
    if (client) {
        b.target_present = MOQ_BINDING_TARGET_COMPLETE;
        b.authority      = "sim.invalid";
    } else {
        b.accept_uri = accept_any_uri;
    }
    MOQ_CHECK(s.bind_transport(b).ok());
}

static session_pair establish_pair(int &failures, moq_version_t ver)
{
    moq::session_config ccfg{};
    ccfg.perspective              = moq::perspective::client;
    ccfg.send_request_capacity    = true;
    ccfg.initial_request_capacity = 10;
    ccfg.version                  = ver;

    moq::session_config scfg{};
    scfg.perspective              = moq::perspective::server;
    scfg.send_request_capacity    = true;
    scfg.initial_request_capacity = 10;
    scfg.version                  = ver;

    auto cr = moq::session::create(ccfg);
    auto sr = moq::session::create(scfg);
    MOQ_CHECK(cr.ok());
    MOQ_CHECK(sr.ok());
    auto c = std::move(*cr);
    auto s = std::move(*sr);
    bind_if_required(failures, c, true);
    bind_if_required(failures, s, false);
    MOQ_CHECK(c.start(0).ok());
    // Drafts 18 and 22 open a control stream from each side, so the server
    // starts too (moq-sim does the same); draft 16's server only answers.
    if (ver != MOQ_VERSION_DRAFT_16) MOQ_CHECK(s.start(0).ok());
    settle(c, s, 0);
    MOQ_CHECK(c.state() == moq::session_state::established);
    MOQ_CHECK(s.state() == moq::session_state::established);
    while (c.poll_event()) {}
    while (s.poll_event()) {}
    return {std::move(c), std::move(s)};
}

// Subscribe with a Next Object location and a fill of the current group;
// the server accepts with Largest (3,2). Returns the server's subscription.
static moq::subscription subscribe_with_fill(int &failures, session_pair &p,
                                             moq::subscription &csub)
{
    moq::subscribe_config sc{};
    moq::subscribe_options options{};
    sc.ns    = {"fill"};
    sc.track = "v";
    options.location = moq::location_filter_config{moq::location_filter_kind::next_object};
    moq::fill_config fill{};
    fill.has_subscriber_priority = true;
    fill.subscriber_priority     = 9;
    fill.location = moq::location_filter_config{};
    fill.location->kind                  = moq::location_filter_kind::relative_group;
    fill.location->relative_group_offset = 1;
    options.fill = fill;
    auto r = p.client.subscribe(sc, options, 1);
    MOQ_CHECK(r.ok());
    if (!r.ok()) return {};
    csub = *r;
    settle(p.client, p.server, 1);

    moq::subscription ssub{};
    while (auto ev = p.server.poll_event()) {
        ev->visit_extended(
            [&](const moq::extended_event::subscribe_request &sr) { ssub = sr.sub; },
            [](const auto &) {});
    }
    MOQ_CHECK(ssub.valid());
    moq::accept_subscribe_config ac{};
    ac.has_largest    = true;
    ac.largest_group  = 3;
    ac.largest_object = 2;
    MOQ_CHECK(p.server.accept_subscribe(ssub, ac, 2).ok());
    settle(p.client, p.server, 2);
    return ssub;
}

static void test_fill_round_trip(int &failures)
{
    auto p = establish_pair(failures, MOQ_VERSION_DRAFT_22);
    moq::subscription csub{};
    (void)subscribe_with_fill(failures, p, csub);

    // The subscriber's child exists; the publisher owes the fill for the
    // current group, resolved to [3,0]..[3,2] against the Largest.
    std::optional<moq::fill_handle> cfill, sfill;
    while (auto ev = p.client.poll_event()) {
        ev->visit_extended(
            [&](const moq::event::fill_created &fc) {
                cfill = fc.fill;
                MOQ_CHECK(fc.sub == csub);
                MOQ_CHECK(!fc.pub.valid());
            },
            [](const auto &) {});
    }
    while (auto ev = p.server.poll_event()) {
        ev->visit_extended(
            [&](const moq::event::fill_request &fr) {
                sfill = fr.fill;
                MOQ_CHECK(fr.start_group == 3 && fr.start_object == 0);
                MOQ_CHECK(fr.end_group == 3 && fr.end_object == 2);
                MOQ_CHECK(fr.subscriber_priority == 9);
                MOQ_CHECK(fr.group_order == moq::group_order::ascending);
            },
            [](const auto &) {});
    }
    MOQ_CHECK(cfill.has_value());
    MOQ_CHECK(sfill.has_value());
    if (!cfill || !sfill) return;

    // The six operations: open, one object, one gap, FIN. abort/cancel are
    // exercised below on their own children.
    MOQ_CHECK(p.server.open_fill(*sfill, 3).ok());
    bool freed = false;
    static const uint8_t bytes[3] = {'o', 'n', 'e'};
    auto wrapped = moq::buffer::wrap(bytes, sizeof(bytes),
        [](void *ctx, const uint8_t *, size_t) { *static_cast<bool *>(ctx) = true; },
        &freed);
    MOQ_CHECK(wrapped.ok());
    if (!wrapped.ok()) return;
    moq::fetch_object_config oc{};
    oc.group_id = 3; oc.object_id = 0; oc.publisher_priority = 5;
    MOQ_CHECK(p.server.write_fill_object(*sfill, oc, *wrapped, 3).ok());
    MOQ_CHECK(p.server.write_fill_range(*sfill, moq::fetch_range_kind::non_existent,
                                        3, 1, 3).ok());
    MOQ_CHECK(p.server.end_fill(*sfill, 4).ok());
    settle(p.client, p.server, 4);

    // The subscriber's view: object, gap, terminal COMPLETE. The object's
    // buffer outlives its polled_event.
    std::optional<moq::event::fill_object> kept;
    int gaps = 0, terminals = 0;
    while (auto ev = p.client.poll_event()) {
        ev->visit_extended(
            [&](const moq::event::fill_object &fo) {
                MOQ_CHECK(fo.fill == *cfill);
                MOQ_CHECK(fo.group_id == 3 && fo.object_id == 0);
                MOQ_CHECK(fo.publisher_priority == 5);
                kept = fo;   // copies: increfs the retained owner
            },
            [&](const moq::event::fill_gap &fg) {
                gaps++;
                MOQ_CHECK(fg.fill == *cfill);
                MOQ_CHECK(fg.range_kind == moq::fetch_range_kind::non_existent);
                MOQ_CHECK(fg.group_id == 3 && fg.object_id == 1);
            },
            [&](const moq::event::fill_terminal &ft) {
                terminals++;
                MOQ_CHECK(ft.fill == *cfill);
                MOQ_CHECK(ft.reason == moq::fill_reason::complete);
                MOQ_CHECK(ft.error_domain == moq::fill_error_domain::none);
            },
            [](const auto &) {});
    }
    MOQ_CHECK(kept.has_value());
    MOQ_CHECK(gaps == 1);
    MOQ_CHECK(terminals == 1);
    if (kept) {
        MOQ_CHECK(kept->payload_data.size() == 3);
        MOQ_CHECK(std::memcmp(kept->payload_data.data(), "one", 3) == 0);
        MOQ_CHECK(kept->payload_owned().raw() == kept->payload_rcbuf);
    }
    // The publisher's own terminal.
    int sterm = 0;
    while (auto ev = p.server.poll_event()) {
        ev->visit_extended(
            [&](const moq::event::fill_terminal &ft) {
                sterm++;
                MOQ_CHECK(ft.fill == *sfill);
                MOQ_CHECK(ft.reason == moq::fill_reason::complete);
            },
            [](const auto &) {});
    }
    MOQ_CHECK(sterm == 1);
    // Stale after the terminal.
    MOQ_CHECK(p.client.cancel_fill(*cfill, 5).error().code() == moq::errc::stale_handle);
}

static void test_fill_abort_and_cancel(int &failures)
{
    auto p = establish_pair(failures, MOQ_VERSION_DRAFT_22);
    moq::subscription csub{};
    auto ssub = subscribe_with_fill(failures, p, csub);

    std::optional<moq::fill_handle> cfill, sfill;
    while (auto ev = p.client.poll_event()) {
        ev->visit_extended([&](const moq::event::fill_created &fc) { cfill = fc.fill; },
                  [](const auto &) {});
    }
    while (auto ev = p.server.poll_event()) {
        ev->visit_extended([&](const moq::event::fill_request &fr) { sfill = fr.fill; },
                  [](const auto &) {});
    }
    MOQ_CHECK(cfill && sfill);
    if (!cfill || !sfill) return;

    // Failure on the wire: an unopened child is opened with its header and
    // reset (section 3.4.1); the subscriber reads the stream code.
    MOQ_CHECK(p.server.abort_fill(*sfill, MOQ_STREAM_ERROR_UNKNOWN_OBJECT_STATUS, 3).ok());
    settle(p.client, p.server, 3);
    int terminals = 0;
    while (auto ev = p.client.poll_event()) {
        ev->visit_extended(
            [&](const moq::event::fill_terminal &ft) {
                terminals++;
                MOQ_CHECK(ft.reason == moq::fill_reason::peer_cancelled_or_reset);
                MOQ_CHECK(ft.error_domain == moq::fill_error_domain::stream);
                MOQ_CHECK(ft.error_code == MOQ_STREAM_ERROR_UNKNOWN_OBJECT_STATUS);
            },
            [](const auto &) {});
    }
    MOQ_CHECK(terminals == 1);
    while (p.server.poll_event()) {}

    // A second fill on an update, cancelled by the subscriber while open.
    moq::subscription_update_config uc{};
    uc.has_subscriber_priority = true;
    uc.subscriber_priority     = 3;
    moq::update_options uoptions{};
    uoptions.fill = moq::fill_config{};
    uoptions.fill->location = moq::location_filter_config{};
    uoptions.fill->location->kind        = moq::location_filter_kind::absolute_start;
    uoptions.fill->location->start_group = 3;
    MOQ_CHECK(p.client.update_subscription(csub, uc, uoptions, 4).ok());
    settle(p.client, p.server, 4);
    cfill.reset(); sfill.reset();
    while (auto ev = p.client.poll_event()) {
        ev->visit_extended([&](const moq::event::fill_created &fc) { cfill = fc.fill; },
                  [](const auto &) {});
    }
    while (auto ev = p.server.poll_event()) {
        ev->visit_extended([&](const moq::event::fill_request &fr) { sfill = fr.fill; },
                  [](const auto &) {});
    }
    MOQ_CHECK(cfill && sfill);
    if (!cfill || !sfill) return;
    MOQ_CHECK(p.server.open_fill(*sfill, 5).ok());
    settle(p.client, p.server, 5);
    MOQ_CHECK(p.client.cancel_fill(*cfill, 6).ok());
    settle(p.client, p.server, 6);
    terminals = 0;
    while (auto ev = p.client.poll_event()) {
        ev->visit_extended(
            [&](const moq::event::fill_terminal &ft) {
                terminals++;
                MOQ_CHECK(ft.reason == moq::fill_reason::local_cancelled);
            },
            [](const auto &) {});
    }
    MOQ_CHECK(terminals == 1);
    int sterm = 0;
    while (auto ev = p.server.poll_event()) {
        ev->visit_extended(
            [&](const moq::event::fill_terminal &ft) {
                sterm++;
                MOQ_CHECK(ft.reason == moq::fill_reason::peer_cancelled_or_reset);
            },
            [](const auto &) {});
    }
    MOQ_CHECK(sterm == 1);

    // The subscription itself is untouched by either end.
    // Authorize the subscriber-controlled value before the publisher reports it.
    moq::subscription_update_config state_update{};
    state_update.has_forward = true;
    state_update.forward = false;
    const auto updated = p.client.update_subscription(csub, state_update, 7);
    MOQ_CHECK(updated.ok());
    if (!updated.ok()) return;
    settle(p.client, p.server, 7);
    int update_acks = 0;
    while (auto ev = p.client.poll_event()) {
        ev->visit_extended(
            [&](const moq::event::subscription_update_ok &ack) {
                ++update_acks;
                MOQ_CHECK(ack.sub == csub);
            },
            [](const auto &) {});
    }
    MOQ_CHECK(update_acks == 1);
    if (update_acks != 1) return;
    moq::publish_state_config ps{};
    ps.has_forward = true;
    ps.forward     = false;
    MOQ_CHECK(p.server.notify_subscription_state(ssub, ps, 7).ok());
    settle(p.client, p.server, 7);
    int states = 0;
    while (auto ev = p.client.poll_event()) {
        ev->visit_extended(
            [&](const moq::extended_event::publish_state &st) {
                states++;
                MOQ_CHECK(st.family == moq::request_family::subscribe);
                MOQ_CHECK(st.sub == csub);
                MOQ_CHECK(st.has_forward && !st.forward);
            },
            [](const auto &) {});
    }
    MOQ_CHECK(states == 1);
}

static void test_fill_refused_on_draft_18(int &failures)
{
    auto p = establish_pair(failures, MOQ_VERSION_DRAFT_18);
    moq::subscribe_config sc{};
    moq::subscribe_options options{};
    sc.ns    = {"fill"};
    sc.track = "v";
    options.fill  = moq::fill_config{};
    auto r = p.client.subscribe(sc, options, 1);
    MOQ_CHECK(!r.ok());
    MOQ_CHECK(r.error().code() == moq::errc::unsupported);
    MOQ_CHECK(!p.client.poll_action());   // nothing was sent

    // A relative start more than one group back has no wire form on
    // draft 18 either (its filters reach back one group at most).
    options.fill.reset();
    options.location = moq::location_filter_config{};
    options.location->kind                  = moq::location_filter_kind::relative_group;
    options.location->relative_group_offset = 3;
    auto r2 = p.client.subscribe(sc, options, 1);
    MOQ_CHECK(!r2.ok());
    MOQ_CHECK(r2.error().code() == moq::errc::unsupported);

    // State notifications are a draft-22 message.
    options.location.reset();
    auto r3 = p.client.subscribe(sc, options, 1);
    MOQ_CHECK(r3.ok());
    settle(p.client, p.server, 1);
    moq::subscription ssub{};
    while (auto ev = p.server.poll_event()) {
        ev->visit_extended([&](const moq::extended_event::subscribe_request &sr) { ssub = sr.sub; },
                  [](const auto &) {});
    }
    MOQ_CHECK(p.server.accept_subscribe(ssub, {}, 2).ok());
    settle(p.client, p.server, 2);
    moq::publish_state_config ps{};
    ps.has_forward = true;
    MOQ_CHECK(p.server.notify_subscription_state(ssub, ps, 3).error().code() ==
              moq::errc::unsupported);
}

static void test_transport_binding(int &failures)
{
    // The query says whether the profile needs facts before it starts; the
    // config is a plain value the session copies on the single bind call.
    moq::session_config cfg{};
    cfg.perspective = moq::perspective::client;
    cfg.version     = MOQ_VERSION_DRAFT_22;
    auto r = moq::session::create(cfg);
    MOQ_CHECK(r.ok());
    if (!r.ok()) return;
    auto s = std::move(*r);
    MOQ_CHECK(s.binding_requirements() ==
              moq::session::version_binding_requirements(MOQ_VERSION_DRAFT_22));
    moq::transport_binding_config b{};
    b.kind            = MOQ_BINDING_RAW_QUIC;
    b.facts_known     = MOQ_BINDING_FACTS_ALL;
    b.facts_supported = MOQ_BINDING_FACTS_ALL;
    b.target_present  = MOQ_BINDING_TARGET_COMPLETE;
    b.authority       = "relay.example";
    b.path_query      = "/moq";
    auto br = s.bind_transport(b);
    if (s.binding_requirements() == 0) {
        MOQ_CHECK(!br.ok());
        MOQ_CHECK(br.error().code() == moq::errc::unsupported);
    } else {
        MOQ_CHECK(br.ok());
        // Single-use: a second binding is refused with the session unchanged.
        MOQ_CHECK(s.bind_transport(b).error().code() == moq::errc::wrong_state);
    }
}

int main()
{
    int failures = 0;
    test_fill_round_trip(failures);
    test_fill_abort_and_cancel(failures);
    test_fill_refused_on_draft_18(failures);
    test_transport_binding(failures);
    MOQ_PASS("test_cpp_fill");
    return failures;
}
