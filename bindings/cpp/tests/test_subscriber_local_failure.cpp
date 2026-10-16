// Reuse the real-session pump, without changing its ACK-result controls.
#define main subscriber_local_failure_ack_controls_main
#include "test_subscriber_track_result.cpp"
#undef main

#include <array>
#include <limits>
#include <memory>
#include <type_traits>

static void local_failure_values()
{
    static_assert(std::is_same_v<decltype(moq::sub_local_failure{}.local_result), int32_t>);
    moq_sub_local_failure_t c{};
    c.struct_size = sizeof(c);
    c.track = reinterpret_cast<moq_sub_track_t *>(uintptr_t{0x1230});
    c.fetch = reinterpret_cast<moq_sub_fetch_req_t *>(uintptr_t{0x4560});
    c.fill._opaque = UINT64_MAX;
    for (uint32_t owner : {uint32_t(MOQ_SUB_LOCAL_FAILURE_TRACK), uint32_t(MOQ_SUB_LOCAL_FAILURE_FETCH),
                          uint32_t(MOQ_SUB_LOCAL_FAILURE_FILL), UINT32_MAX}) {
        for (int32_t code : {INT32_MIN, int32_t(MOQ_ERR_BUFFER), int32_t(0), INT32_MAX}) {
            for (uint64_t id : {UINT64_C(0), UINT64_MAX}) {
                c.owner_kind = owner; c.local_result = code; c.initiating_request_id = id;
                auto result = moq::sub_local_failure::from_native(&c, sizeof(c));
                MOQ_CHECK(result.ok());
                if (!result) continue;
                const auto &v = *result;
                MOQ_CHECK(static_cast<uint32_t>(v.owner_kind) == owner && v.local_result == code);
                const bool track = owner == MOQ_SUB_LOCAL_FAILURE_TRACK || owner == MOQ_SUB_LOCAL_FAILURE_FILL;
                MOQ_CHECK(v.track.has_value() == track);
                if (v.track) MOQ_CHECK(v.track->raw() == c.track);
                MOQ_CHECK(v.fetch_id.has_value() == (owner == MOQ_SUB_LOCAL_FAILURE_FETCH));
                if (v.fetch_id) MOQ_CHECK(*v.fetch_id == reinterpret_cast<uintptr_t>(c.fetch));
                MOQ_CHECK(v.fill.has_value() == (owner == MOQ_SUB_LOCAL_FAILURE_FILL));
                MOQ_CHECK(v.initiating_request_id.has_value() == (owner == MOQ_SUB_LOCAL_FAILURE_FILL));
                if (v.fill) MOQ_CHECK(v.fill->raw()._opaque == UINT64_MAX && *v.initiating_request_id == id);
            }
        }
    }
    c.owner_kind = MOQ_SUB_LOCAL_FAILURE_TRACK; c.track = nullptr;
    MOQ_CHECK(!moq::sub_local_failure::from_native(&c, sizeof(c))->track);
    c.owner_kind = MOQ_SUB_LOCAL_FAILURE_FETCH; c.fetch = nullptr;
    MOQ_CHECK(!moq::sub_local_failure::from_native(&c, sizeof(c))->fetch_id);
    c.owner_kind = MOQ_SUB_LOCAL_FAILURE_FILL; c.fill = {};
    auto absent = moq::sub_local_failure::from_native(&c, sizeof(c)).value();
    MOQ_CHECK(!absent.track && !absent.fetch_id && !absent.fill && absent.initiating_request_id == UINT64_MAX);
}

static void local_failure_prefixes()
{
    moq_sub_local_failure_t c{};
    c.struct_size = sizeof(c); c.owner_kind = MOQ_SUB_LOCAL_FAILURE_FILL;
    c.local_result = MOQ_ERR_BUFFER; c.fill._opaque = 42;
    const size_t floor = MOQ_SUB_LOCAL_FAILURE_V0_SIZE;
    for (size_t size = 0; size < floor; ++size) {
        auto physical = std::make_unique<unsigned char[]>(size);
        if (size) std::memcpy(physical.get(), &c, size);
        auto result = moq::sub_local_failure::from_native(physical.get(), size);
        MOQ_CHECK(!result && result.error().code() == moq::errc::abi_mismatch);
        auto short_stamp = c; short_stamp.struct_size = static_cast<uint32_t>(size);
        result = moq::sub_local_failure::from_native(&short_stamp, sizeof(short_stamp));
        MOQ_CHECK(!result && result.error().code() == moq::errc::abi_mismatch);
    }
    MOQ_CHECK(!moq::sub_local_failure::from_native(nullptr, sizeof(c)));
    std::array<unsigned char, sizeof(c) + 17> future{};
    c.struct_size = static_cast<uint32_t>(future.size());
    std::memcpy(future.data(), &c, sizeof(c));
    for (size_t size : {floor, sizeof(c), future.size()}) {
        auto result = moq::sub_local_failure::from_native(future.data(), size);
        MOQ_CHECK(result && result->initiating_request_id == 0);
    }
    auto invalid = moq::subscriber{}.poll_local_failure();
    MOQ_CHECK(!invalid && invalid.error().code() == moq::errc::invalid);
}

static moq::session local_failure_session(moq::perspective role)
{
    moq::session_config cfg{};
    cfg.perspective = role; cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.send_request_capacity = true; cfg.initial_request_capacity = 64;
    cfg.max_object_payload_size = 32 * 1024 * 1024;
    cfg.max_receive_buffer_bytes = 64 * 1024 * 1024;
    auto s = moq::session::create(cfg).value();
    moq::transport_binding_config binding{};
    binding.kind = MOQ_BINDING_RAW_QUIC;
    binding.facts_known = binding.facts_supported = MOQ_BINDING_FACTS_ALL;
    if (role == moq::perspective::client) {
        binding.target_present = MOQ_BINDING_TARGET_COMPLETE; binding.authority = "sim.invalid";
    } else binding.accept_uri = accept_uri;
    MOQ_CHECK(s.bind_transport(binding).ok());
    return s;
}

static void local_failure_route(uint32_t owner)
{
    session_pair pair{local_failure_session(moq::perspective::client),
                      local_failure_session(moq::perspective::server), {}};
    MOQ_CHECK(pair.client.start(0).ok() && pair.server.start(0).ok()); pair.settle();
    while (pair.client.poll_event()) {}
    while (pair.server.poll_event()) {}
    auto sub = moq::subscriber::create(pair.client).value();
    MOQ_CHECK(!sub.poll_local_failure().value());
    moq::sub_track_options options{};
    if (owner == MOQ_SUB_LOCAL_FAILURE_FILL) {
        options.location = moq::location_filter_config{moq::location_filter_kind::next_object};
        options.fill = moq::fill_config{};
        options.fill->location = moq::location_filter_config{moq::location_filter_kind::absolute_start};
    }
    auto track = sub.subscribe({.ns = {"n21"}, .track = "video"}, options, 0).value();
    pair.settle();
    moq_subscription_t peer{};
    moq_fill_t child{};
    uint64_t initiating_id = UINT64_MAX;
    moq_event_t event{};
    while (moq_session_poll_events(pair.server.raw(), &event, 1)) {
        if (event.kind == MOQ_EVENT_SUBSCRIBE_REQUEST) peer = event.u.subscribe_request.sub;
        moq_event_cleanup(&event);
    }
    moq_accept_subscribe_cfg_t accept; moq_accept_subscribe_cfg_init(&accept);
    accept.has_largest = true; accept.largest_group = 1; accept.largest_object = 1;
    auto props = properties(MOQ_VERSION_DRAFT_22);
    accept.track_properties = {props.data(), props.size()};
    MOQ_CHECK(moq_session_accept_subscribe(pair.server.raw(), peer, &accept, 0) == MOQ_OK);
    pair.settle(); MOQ_CHECK(sub.tick(0).ok());
    while (moq_session_poll_events(pair.server.raw(), &event, 1)) {
        if (event.kind == MOQ_EVENT_FILL_REQUEST) {
            child = event.u.fill_request.fill; initiating_id = event.u.fill_request.initiating_request_id;
        }
        moq_event_cleanup(&event);
    }
    auto token = sub.track_fill(track);
    if (owner == MOQ_SUB_LOCAL_FAILURE_FILL) {
        auto created = sub.poll_fill();
        MOQ_CHECK(created && created->kind() == moq::sub_fill_item_kind::created);
        MOQ_CHECK(initiating_id == 0);
    }
    moq::sub_fetch_request fetch;
    moq_fetch_t peer_fetch{};
    const moq::sub_fetch_config fetch_cfg{.ns = {"n21"}, .track = "video", .start_group = 1, .end_group = 1, .end_object = 2};
    if (owner == MOQ_SUB_LOCAL_FAILURE_FETCH) {
        fetch = sub.fetch(fetch_cfg, 0).value(); pair.settle();
        while (moq_session_poll_events(pair.server.raw(), &event, 1)) {
            if (event.kind == MOQ_EVENT_FETCH_REQUEST) peer_fetch = event.u.fetch_request.fetch;
            moq_event_cleanup(&event);
        }
        moq_accept_fetch_cfg_t cfg; moq_accept_fetch_cfg_init(&cfg);
        cfg.end_group = 1; cfg.end_object = 2;
        MOQ_CHECK(moq_session_accept_fetch(pair.server.raw(), peer_fetch, &cfg, 0) == MOQ_OK);
    }
    // The unchanged wrapper uses the native 16 MiB limit. ACK bytes make this
    // otherwise admissible session object permanently too large for the facade.
    std::vector<uint8_t> bytes(16 * 1024 * 1024 - 1, 0x61);
    auto payload = moq::buffer::create(bytes.data(), bytes.size()).value();
    if (owner == MOQ_SUB_LOCAL_FAILURE_TRACK) {
        moq_subgroup_cfg_t cfg; moq_subgroup_cfg_init(&cfg);
        cfg.group_id = 2;
        moq_subgroup_handle_t group{};
        MOQ_CHECK(moq_session_open_subgroup(pair.server.raw(), peer, &cfg, 0, &group) == MOQ_OK);
        moq_object_cfg_t object; moq_object_cfg_init(&object); object.payload = payload.raw();
        MOQ_CHECK(moq_session_write_object_ex(pair.server.raw(), group, &object, 0) == MOQ_OK);
        MOQ_CHECK(moq_session_close_subgroup(pair.server.raw(), group, 0) == MOQ_OK);
    } else {
        moq_fetch_object_cfg_t object; moq_fetch_object_cfg_init(&object);
        object.group_id = 1; object.payload = payload.raw();
        if (owner == MOQ_SUB_LOCAL_FAILURE_FILL) {
            MOQ_CHECK(moq_session_open_fill(pair.server.raw(), child, 0) == MOQ_OK);
            MOQ_CHECK(moq_session_write_fill_object(pair.server.raw(), child, &object, 0) == MOQ_OK);
            MOQ_CHECK(moq_session_end_fill(pair.server.raw(), child, 0) == MOQ_OK);
        } else {
            MOQ_CHECK(moq_session_write_fetch_object(pair.server.raw(), peer_fetch, &object, 0) == MOQ_OK);
            MOQ_CHECK(moq_session_end_fetch(pair.server.raw(), peer_fetch, 0) == MOQ_OK);
        }
    }
    pair.settle();
    auto tick = sub.tick(0);
    MOQ_CHECK(!tick && tick.error().code() == moq::errc::buffer);
    MOQ_CHECK(sub.tick(0).ok());
    if (owner == MOQ_SUB_LOCAL_FAILURE_TRACK) {
        MOQ_CHECK(moq_sub_release_track(sub.raw(), track.raw()) == MOQ_ERR_WRONG_STATE);
    } else if (owner == MOQ_SUB_LOCAL_FAILURE_FILL) {
        auto terminal = sub.poll_fill();
        MOQ_CHECK(terminal && terminal->kind() == moq::sub_fill_item_kind::terminal);
        MOQ_CHECK(sub.track_fill(track) == token);
    } else {
        while (sub.poll_fetch()) {}
        auto pinned = sub.fetch(fetch_cfg, 0).value();
        MOQ_CHECK(pinned.raw() != fetch.raw());
    }
    auto notice = sub.poll_local_failure().value();
    MOQ_CHECK(notice && static_cast<uint32_t>(notice->owner_kind) == owner);
    if (!notice) return;
    MOQ_CHECK(notice->local_result == MOQ_ERR_BUFFER);
    MOQ_CHECK(!sub.poll_local_failure().value());
    if (owner == MOQ_SUB_LOCAL_FAILURE_TRACK) {
        MOQ_CHECK(notice->track && notice->track->raw() == track.raw());
        MOQ_CHECK(!notice->fill && !notice->fetch_id && !notice->initiating_request_id);
        MOQ_CHECK(moq_sub_release_track(sub.raw(), track.raw()) == MOQ_OK);
    } else if (owner == MOQ_SUB_LOCAL_FAILURE_FILL) {
        MOQ_CHECK(notice->track && notice->track->raw() == track.raw());
        MOQ_CHECK(notice->fill == token && notice->initiating_request_id == 0 && !notice->fetch_id);
        MOQ_CHECK(!sub.track_fill(track).valid() && track.is_active());
    } else {
        MOQ_CHECK(notice->fetch_id == reinterpret_cast<uintptr_t>(fetch.raw()));
        MOQ_CHECK(!notice->track && !notice->fill && !notice->initiating_request_id);
        MOQ_CHECK(sub.fetch(fetch_cfg, 0)->raw() == fetch.raw());
    }
    MOQ_CHECK(pair.client.state() == moq::session_state::established);
}

int main()
{
    local_failure_values(); local_failure_prefixes();
    for (uint32_t owner : {uint32_t(MOQ_SUB_LOCAL_FAILURE_TRACK), uint32_t(MOQ_SUB_LOCAL_FAILURE_FETCH),
                          uint32_t(MOQ_SUB_LOCAL_FAILURE_FILL)}) local_failure_route(owner);
    std::printf("subscriber_local_failure: %d failures\n", failures);
    return failures != 0;
}
