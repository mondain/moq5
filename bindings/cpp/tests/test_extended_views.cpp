#include <moq/moq.hpp>
#include "test_support.hpp"

#include <array>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <optional>
#include <tuple>
#include <utility>

static void released(void *ctx, const uint8_t *, size_t)
{
    ++*static_cast<unsigned *>(ctx);
}

static moq_rcbuf_t *owned_byte(uint8_t &byte, unsigned &releases)
{
    return moq::buffer::wrap(&byte, 1, released, &releases).value().release();
}

static void test_reset(int &failures)
{
    moq_action_t raw{};
    raw.kind = MOQ_ACTION_RESET_DATA;
    raw.detail_size = offsetof(moq_reset_data_action_t, reliable_size);
    raw.u.reset_data.stream_ref = moq_stream_ref_t{13};
    raw.u.reset_data.error_code = UINT64_MAX;
    raw.u.reset_data.reliable_size = UINT64_MAX;
    moq::polled_action old(raw);
    MOQ_CHECK(old.reliable_size() == 0);
    const auto legacy = std::get<moq::action::reset_data>(old.variant());
    const auto &[ref, code] = legacy;
    MOQ_CHECK(ref.value() == 13 && code == UINT64_MAX);
    MOQ_CHECK(std::get<moq::extended_action::reset_data>(old.variant_extended()).reliable_size == 0);

    raw.detail_size = sizeof(moq_reset_data_action_t) - 1;
    MOQ_CHECK(moq::polled_action(raw).reliable_size() == 0);
    raw.detail_size = sizeof(moq_reset_data_action_t);
    moq::polled_action full(raw);
    MOQ_CHECK(full.reliable_size() == UINT64_MAX);
    auto view = full.variant_extended();
    MOQ_CHECK(std::get<moq::extended_action::reset_data>(view).reliable_size == UINT64_MAX);
    raw.u.reset_data.reliable_size = 0;
    MOQ_CHECK(moq::polled_action(raw).reliable_size() == 0);
    raw.kind = MOQ_ACTION_STOP_DATA;
    raw.u.reset_data.reliable_size = 9;
    MOQ_CHECK(moq::polled_action(raw).reliable_size() == 0);
}

static void test_action_alternatives(int &failures)
{
    const std::array<moq_action_kind_t, 5> kinds{
        MOQ_ACTION_OPEN_UNI_CONTROL, MOQ_ACTION_SEND_UNI_CONTROL,
        MOQ_ACTION_RESET_BIDI_STREAM, MOQ_ACTION_STOP_BIDI_STREAM,
        MOQ_ACTION_ABORT_BIDI_STREAM};
    for (size_t i = 0; i < kinds.size(); ++i) {
        moq_action_t raw{};
        raw.kind = kinds[i];
        moq::polled_action owner(raw);
        MOQ_CHECK(std::get<moq::action::unknown>(owner.variant()).kind == kinds[i]);
        MOQ_CHECK(owner.variant_extended().index() == i + 9);
    }
    moq_action_t unknown{};
    unknown.kind = UINT32_MAX;
    MOQ_CHECK(std::get<moq::action::unknown>(moq::polled_action(unknown).variant()).kind == UINT32_MAX);
    MOQ_CHECK(std::get<moq::action::unknown>(moq::polled_action(unknown).variant_extended()).kind == UINT32_MAX);
}

static void test_action_lifetime(int &failures)
{
    uint8_t byte = 91;
    unsigned releases = 0;
    std::optional<moq::action_variant> legacy;
    std::optional<moq::extended_action_variant> extended;
    {
        moq_action_t raw{};
        raw.kind = MOQ_ACTION_SEND_DATA;
        raw.detail_size = sizeof(raw.u.send_data);
        raw.u.send_data.payload = owned_byte(byte, releases);
        moq::polled_action owner(raw);
        legacy = owner.variant();
        extended = owner.variant_extended();
        moq::polled_action moved(std::move(owner));
        MOQ_CHECK(moq_rcbuf_refcount(moved.raw().u.send_data.payload) == 3);
    }
    MOQ_CHECK(releases == 0);
    auto copy = *extended;
    legacy.reset();
    extended.reset();
    MOQ_CHECK(std::get<moq::action::send_data>(copy).payload[0] == 91);
    MOQ_CHECK(releases == 0);
    copy = moq::action::unknown{};
    MOQ_CHECK(releases == 1);
}

static void test_fill_lifetime(int &failures)
{
    uint8_t payload = 37, properties = 53;
    unsigned payload_releases = 0, properties_releases = 0;
    std::optional<moq::extended_event_variant> retained;
    {
        moq_event_t raw{};
        raw.kind = MOQ_EVENT_FILL_OBJECT;
        raw.detail_size = MOQ_FILL_OBJECT_EVENT_V0_SIZE;
        raw.u.fill_object.initiating_request_id = UINT64_MAX;
        raw.u.fill_object.payload = owned_byte(payload, payload_releases);
        raw.u.fill_object.properties = owned_byte(properties, properties_releases);
        moq::polled_event owner(raw);
        MOQ_CHECK(std::get<moq::event::unknown>(owner.variant()).kind == MOQ_EVENT_FILL_OBJECT);
        MOQ_CHECK(moq_rcbuf_refcount(raw.u.fill_object.payload) == 1);
        retained = owner.variant_extended();
        moq::polled_event moved(std::move(owner));
        MOQ_CHECK(moq_rcbuf_refcount(moved.raw().u.fill_object.properties) == 2);
    }
    MOQ_CHECK(payload_releases == 0 && properties_releases == 0);
    auto copy = *retained;
    retained.reset();
    const auto &object = std::get<moq::event::fill_object>(copy);
    MOQ_CHECK(object.initiating_request_id == UINT64_MAX);
    MOQ_CHECK(object.payload_data[0] == 37 && object.properties_data[0] == 53);
    copy = moq::event::unknown{};
    MOQ_CHECK(payload_releases == 1 && properties_releases == 1);

    // Projecting only the legacy unknown must still clean the owned native refs.
    {
        moq_event_t raw{};
        raw.kind = MOQ_EVENT_FILL_OBJECT;
        raw.detail_size = MOQ_FILL_OBJECT_EVENT_V0_SIZE;
        raw.u.fill_object.payload = owned_byte(payload, payload_releases);
        raw.u.fill_object.properties = owned_byte(properties, properties_releases);
        moq::polled_event owner(raw);
        MOQ_CHECK(std::holds_alternative<moq::event::unknown>(owner.variant()));
    }
    MOQ_CHECK(payload_releases == 2 && properties_releases == 2);
}

static void test_update_snapshot(int &failures)
{
    moq_subscription_update_options_t options{};
    options.struct_size = sizeof(options);
    options.flags = MOQ_SUBSCRIPTION_UPDATE_OPTIONS_LOCATION |
                    MOQ_SUBSCRIPTION_UPDATE_OPTIONS_OBJECT_DELIVERY_TIMEOUT |
                    MOQ_SUBSCRIPTION_UPDATE_OPTIONS_SUBGROUP_DELIVERY_TIMEOUT;
    options.initiating_request_id = 42;
    moq_location_filter_cfg_init(&options.location);
    options.location.kind = MOQ_LOCATION_FILTER_NONE;
    options.object_delivery_timeout_ms = 0;
    options.subgroup_delivery_timeout_ms = UINT64_MAX;
    moq_event_t raw{};
    raw.kind = MOQ_EVENT_SUBSCRIBE_UPDATED;
    raw.detail_size = sizeof(raw.u.subscribe_updated);
    raw.u.subscribe_updated.options = &options;
    moq::polled_event owner(raw);
    auto view = std::get<moq::extended_event::subscribe_updated>(owner.variant_extended());
    options = {};
    MOQ_CHECK(view.options.has_value());
    if (view.options) {
        MOQ_CHECK(view.options->initiating_request_id == 42);
        MOQ_CHECK(view.options->location && view.options->location->kind == moq::location_filter_kind::none);
        MOQ_CHECK(view.options->object_delivery_timeout_ms == 0);
        MOQ_CHECK(view.options->subgroup_delivery_timeout_ms == UINT64_MAX);
    }
    const auto legacy = std::get<moq::event::subscribe_updated>(owner.variant());
    MOQ_CHECK(!legacy.has_filter && !legacy.has_delivery_timeout);

    options.struct_size = sizeof(options);
    auto empty = std::get<moq::extended_event::subscribe_updated>(owner.variant_extended());
    MOQ_CHECK(empty.options && !empty.options->location &&
              !empty.options->object_delivery_timeout_ms && !empty.options->subgroup_delivery_timeout_ms);
    raw.detail_size = offsetof(moq_subscribe_updated_event_t, options);
    auto old = std::get<moq::extended_event::subscribe_updated>(moq::polled_event(raw).variant_extended());
    MOQ_CHECK(!old.options);
}

static void test_terminal_projection(int &failures)
{
    moq_event_t raw{};
    raw.kind = MOQ_EVENT_PUBLISH_FINISHED;
    raw.detail_size = sizeof(raw.u.publish_finished);
    raw.u.publish_finished.cause = MOQ_REQUEST_TERMINAL_MISSING_RESPONSE;
    moq::polled_event owner(raw);
    MOQ_CHECK(std::holds_alternative<moq::event::unknown>(owner.variant()));
    MOQ_CHECK(std::get<moq::extended_event::publish_finished>(owner.variant_extended()).cause ==
              moq::request_terminal_cause::missing_response);
    raw.u.publish_finished.cause = MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM;
    raw.u.publish_finished.stream_error_code = UINT64_MAX;
    auto reset = std::get<moq::extended_event::publish_finished>(moq::polled_event(raw).variant_extended());
    MOQ_CHECK(reset.stream_error_code == UINT64_MAX);
    raw.u.publish_finished.cause = 0;
    raw.u.publish_finished.status_code = 4;
    MOQ_CHECK(std::get<moq::event::publish_finished>(moq::polled_event(raw).variant()).status_code == 4);
    raw.kind = MOQ_EVENT_FETCH_RESET;
    raw.detail_size = sizeof(raw.u.fetch_reset);
    raw.u.fetch_reset = {};
    raw.u.fetch_reset.cause = MOQ_REQUEST_TERMINAL_TRUNCATED_RESPONSE;
    MOQ_CHECK(std::holds_alternative<moq::event::unknown>(moq::polled_event(raw).variant()));
    raw.u.fetch_reset.cause = MOQ_REQUEST_TERMINAL_PEER_RESET_STREAM;
    MOQ_CHECK(std::get<moq::event::fetch_reset>(moq::polled_event(raw).variant()).error_code == 0);
}

static void test_legacy_update_projection(int &failures)
{
    moq_event_t raw{};
    raw.kind = MOQ_EVENT_PUBLISH_UPDATED;
    raw.detail_size = sizeof(raw.u.publish_updated);
    raw.u.publish_updated.has_filter = true;
    raw.u.publish_updated.filter = MOQ_SUBSCRIBE_FILTER_ABSOLUTE_RANGE;
    raw.u.publish_updated.start_group = 2;
    raw.u.publish_updated.end_group = 4;
    auto legacy = std::get<moq::event::publish_updated>(moq::polled_event(raw).variant());
    MOQ_CHECK(legacy.has_filter && legacy.start_group == 2 && legacy.end_group == 4);
    auto extended = std::get<moq::extended_event::publish_updated>(moq::polled_event(raw).variant_extended());
    MOQ_CHECK(extended.has_filter && extended.end_group == 4);
    MOQ_CHECK(!extended.options);

    raw = {};
    raw.kind = MOQ_EVENT_PUBLISH_STATE;
    raw.detail_size = sizeof(raw.u.publish_state);
    raw.u.publish_state.has_filter = true;
    MOQ_CHECK(std::holds_alternative<moq::event::unknown>(moq::polled_event(raw).variant_extended()));
}

static void test_publication_options(int &failures)
{
    constexpr size_t prefix = offsetof(moq_subscription_update_options_t,
                                      subgroup_delivery_timeout_ms) + sizeof(uint64_t);
    static_assert(prefix == 80);
    for (unsigned row = 0; row < 6; ++row) {
        moq_subscription_update_options_t input{};
        input.struct_size = prefix;
        input.initiating_request_id = 40 + row;
        input.object_delivery_timeout_ms = UINT64_MAX;
        input.subgroup_delivery_timeout_ms = UINT64_MAX;
        if (row) {
            input.flags = MOQ_SUBSCRIPTION_UPDATE_OPTIONS_LOCATION;
            moq_location_filter_cfg_init(&input.location);
            input.location.kind = row == 1 ? MOQ_LOCATION_FILTER_NONE :
                row == 2 ? MOQ_LOCATION_FILTER_RELATIVE_GROUP : MOQ_LOCATION_FILTER_OBJECT_END;
            if (row == 2) input.location.relative_group_offset = UINT64_MAX;
            if (row >= 3) {
                input.location.start_group = 1;
                input.location.start_object = 2;
                input.location.end_group = 3;
                input.location.end_object = UINT64_MAX;
            }
        }
        if (row >= 3) {
            input.flags |= MOQ_SUBSCRIPTION_UPDATE_OPTIONS_OBJECT_DELIVERY_TIMEOUT;
            input.object_delivery_timeout_ms = 0;
        }
        if (row == 4 || row == 5) {
            if (row == 4) input.flags &= ~MOQ_SUBSCRIPTION_UPDATE_OPTIONS_OBJECT_DELIVERY_TIMEOUT;
            input.flags |= MOQ_SUBSCRIPTION_UPDATE_OPTIONS_SUBGROUP_DELIVERY_TIMEOUT;
        }
        void *storage = std::malloc(prefix);
        MOQ_CHECK(storage != nullptr);
        if (!storage) return;
        std::memcpy(storage, &input, prefix);
        moq_event_t raw{};
        raw.kind = MOQ_EVENT_PUBLISH_UPDATED;
        raw.detail_size = sizeof(raw.u.publish_updated);
        raw.u.publish_updated.options = static_cast<moq_subscription_update_options_t *>(storage);
        raw.u.publish_updated.answer_owed = true;
        raw.u.publish_updated.fill_requested = true;
        raw.u.publish_updated.update_count = 1;
        raw.u.publish_updated.update_request_id = input.initiating_request_id;
        auto copy = std::get<moq::extended_event::publish_updated>(
            moq::polled_event(raw).variant_extended());
        std::memset(storage, 0, prefix);
        std::free(storage);
        MOQ_CHECK(copy.options.has_value());
        if (!copy.options) continue;
        MOQ_CHECK(copy.answer_owed && copy.fill_requested && copy.update_count == 1);
        MOQ_CHECK(copy.update_request_id == input.initiating_request_id);
        MOQ_CHECK(copy.options->initiating_request_id == input.initiating_request_id);
        MOQ_CHECK(copy.options->flags == input.flags);
        MOQ_CHECK(copy.options->location.has_value() == (row != 0));
        if (copy.options->location) {
            const auto &location = *copy.options->location;
            MOQ_CHECK(static_cast<uint32_t>(location.kind) == input.location.kind);
            MOQ_CHECK(location.relative_group_offset == input.location.relative_group_offset);
            MOQ_CHECK(location.end_object == input.location.end_object);
        }
        MOQ_CHECK(copy.options->object_delivery_timeout_ms ==
            ((row == 3 || row == 5) ? std::optional<uint64_t>(0) : std::nullopt));
        MOQ_CHECK(copy.options->subgroup_delivery_timeout_ms ==
            (row >= 4 ? std::optional<uint64_t>(UINT64_MAX) : std::nullopt));
        MOQ_CHECK(!copy.has_filter && !copy.has_delivery_timeout && copy.delivery_timeout_us == 0);
    }

    moq_event_t raw{};
    raw.kind = MOQ_EVENT_PUBLISH_UPDATED;
    const auto end = offsetof(moq_publish_updated_event_t, options) + sizeof(raw.u.publish_updated.options);
    for (size_t n = offsetof(moq_publish_updated_event_t, options); n < end; ++n) {
        raw.detail_size = static_cast<uint32_t>(n);
        raw.u.publish_updated.options = reinterpret_cast<const moq_subscription_update_options_t *>(1);
        raw.u.publish_updated.answer_owed = true;
        raw.u.publish_updated.update_request_id = UINT64_MAX;
        const auto view = std::get<moq::extended_event::publish_updated>(
            moq::polled_event(raw).variant_extended());
        MOQ_CHECK(!view.options && !view.answer_owed && view.update_request_id == 0);
    }
    moq_subscription_update_options_t options{};
    raw.detail_size = sizeof(raw.u.publish_updated);
    raw.u.publish_updated.options = &options;
    for (uint32_t size : {0u, 4u, 79u, 80u, static_cast<uint32_t>(sizeof(options))}) {
        options.struct_size = size;
        const auto view = std::get<moq::extended_event::publish_updated>(
            moq::polled_event(raw).variant_extended());
        MOQ_CHECK(view.options.has_value() == (size >= prefix));
    }
    options.flags = MOQ_SUBSCRIPTION_UPDATE_OPTIONS_LOCATION;
    options.location.struct_size = MOQ_LOCATION_FILTER_CFG_V0_SIZE - 1;
    options.location.kind = MOQ_LOCATION_FILTER_OBJECT_END;
    options.location.end_object = UINT64_MAX;
    const auto short_location = std::get<moq::extended_event::publish_updated>(
        moq::polled_event(raw).variant_extended());
    MOQ_CHECK(short_location.options && !short_location.options->location);
    MOQ_PASS("publication options copied and prefix-gated");
}

static void test_admission_prefixes(int &failures)
{
    static_assert(MOQ_SUBSCRIPTION_ADMISSION_OPTIONS_V0_SIZE == 72);
    moq_event_t raw{};
    raw.kind = MOQ_EVENT_SUBSCRIBE_REQUEST;
    const size_t offset = offsetof(moq_subscribe_request_event_t, options);
    const auto read = [&] {
        return std::get<moq::extended_event::subscribe_request>(moq::polled_event(raw).variant_extended());
    };
    for (size_t size = offset; size < offset + sizeof(void *); ++size) {
        raw.detail_size = static_cast<uint32_t>(size);
        raw.u.subscribe_request.options = reinterpret_cast<const moq_subscription_admission_options_t *>(1);
        MOQ_CHECK(!read().options);
        MOQ_CHECK(std::holds_alternative<moq::event::subscribe_request>(moq::polled_event(raw).variant()));
    }
    raw.detail_size = static_cast<uint32_t>(offset - 1);
    MOQ_CHECK(std::holds_alternative<moq::event::unknown>(moq::polled_event(raw).variant_extended()));
    raw.detail_size = sizeof(raw.u.subscribe_request);
    raw.u.subscribe_request.options = nullptr;
    MOQ_CHECK(!read().options);
    for (size_t size : {4u, 71u, 72u, 88u}) {
        moq_subscription_admission_options_t input{};
        input.struct_size = static_cast<uint32_t>(size);
        input.flags = UINT32_MAX;
        moq_location_filter_cfg_init(&input.location);
        input.location.kind = UINT32_MAX;
        input.location.end_object = UINT64_MAX;
        input.subgroup_delivery_timeout_ms = UINT64_MAX;
        auto *storage = static_cast<unsigned char *>(std::malloc(size));
        MOQ_CHECK(storage != nullptr);
        if (!storage) return;
        std::memset(storage, 0xa5, size);
        std::memcpy(storage, &input, std::min(size, sizeof(input)));
        raw.u.subscribe_request.options = reinterpret_cast<const moq_subscription_admission_options_t *>(storage);
        auto copy = read();
        std::memset(storage, 0, size);
        std::free(storage);
        MOQ_CHECK(copy.options.has_value() == (size >= 72));
        if (copy.options) {
            MOQ_CHECK(copy.options->flags == UINT32_MAX && copy.options->fill_requested);
            MOQ_CHECK(copy.options->location && static_cast<uint32_t>(copy.options->location->kind) == UINT32_MAX);
            MOQ_CHECK(copy.options->location->end_object == UINT64_MAX);
            MOQ_CHECK(copy.options->object_delivery_timeout_ms == 0);
            MOQ_CHECK(copy.options->subgroup_delivery_timeout_ms == UINT64_MAX);
        }
    }
    moq_subscription_admission_options_t input{};
    input.struct_size = sizeof(input);
    input.object_delivery_timeout_ms = input.subgroup_delivery_timeout_ms = UINT64_MAX;
    raw.u.subscribe_request.options = &input;
    auto absent = read();
    MOQ_CHECK(absent.options && absent.options->flags == 0 && !absent.options->fill_requested);
    MOQ_CHECK(!absent.options->location && !absent.options->object_delivery_timeout_ms &&
              !absent.options->subgroup_delivery_timeout_ms);
    input.flags = MOQ_SUBSCRIPTION_ADMISSION_LOCATION;
    input.location.struct_size = MOQ_LOCATION_FILTER_CFG_V0_SIZE - 1;
    MOQ_CHECK(read().options && !read().options->location);
    input.location.struct_size++;
    auto none = read();
    MOQ_CHECK(none.options->location && none.options->location->kind == moq::location_filter_kind::none);
    // Keep the legacy 14-member aggregate decomposition, not a new options tail.
    const auto legacy = std::get<moq::event::subscribe_request>(moq::polled_event(raw).variant());
    const auto &[sub, ns, name, filter, priority, order, forward, sg, so, eg, timeout, tokens, has_new, group] = legacy;
    (void)std::tie(sub, ns, name, filter, priority, order, forward, sg, so, eg, timeout, tokens, has_new, group);
    MOQ_PASS("admission complete-prefix copies and unchanged legacy aggregate");
}

static void test_state_prefixes(int &failures)
{
    moq_event_t raw{};
    raw.kind = MOQ_EVENT_PUBLISH_STATE;
    raw.u.publish_state.family = MOQ_REQUEST_FAMILY_SUBSCRIBE;
    raw.u.publish_state.handle.subscription = moq_subscription_t{17};
    raw.u.publish_state.has_forward = true;
    raw.u.publish_state.has_largest = true;
    raw.u.publish_state.largest_object = UINT64_MAX;
    const size_t offset = offsetof(moq_publish_state_event_t, location);
    const auto read = [&] { return moq::polled_event(raw).variant_extended(); };
    for (size_t size = 0; size < offset; ++size) {
        raw.detail_size = static_cast<uint32_t>(size);
        MOQ_CHECK(std::holds_alternative<moq::event::unknown>(read()));
    }
    for (size_t size = offset; size < offset + sizeof(void *); ++size) {
        raw.detail_size = static_cast<uint32_t>(size);
        raw.u.publish_state.location = reinterpret_cast<const moq_location_filter_cfg_t *>(1);
        raw.u.publish_state.has_filter = false;
        auto absent = std::get<moq::extended_event::publish_state>(read());
        MOQ_CHECK(!absent.location && absent.has_forward && !absent.forward);
        MOQ_CHECK(absent.has_largest && absent.largest_group == 0 && absent.largest_object == UINT64_MAX);
        raw.u.publish_state.has_filter = true;
        MOQ_CHECK(std::holds_alternative<moq::event::unknown>(read()));
    }
    raw.detail_size = sizeof(raw.u.publish_state);
    raw.u.publish_state.location = nullptr;
    MOQ_CHECK(std::holds_alternative<moq::event::unknown>(read()));
    for (size_t size : {4u, 47u, 48u, 64u}) {
        moq_location_filter_cfg_t input{};
        input.struct_size = static_cast<uint32_t>(size);
        input.kind = UINT32_MAX;
        input.end_object = UINT64_MAX;
        auto *storage = static_cast<unsigned char *>(std::malloc(size));
        MOQ_CHECK(storage != nullptr);
        if (!storage) return;
        std::memset(storage, 0xa5, size);
        std::memcpy(storage, &input, std::min(size, sizeof(input)));
        raw.u.publish_state.location = reinterpret_cast<const moq_location_filter_cfg_t *>(storage);
        auto copy = read();
        std::memset(storage, 0, size);
        std::free(storage);
        if (size < 48) { MOQ_CHECK(std::holds_alternative<moq::event::unknown>(copy)); continue; }
        const auto &state = std::get<moq::extended_event::publish_state>(copy);
        MOQ_CHECK(state.location && static_cast<uint32_t>(state.location->kind) == UINT32_MAX);
        MOQ_CHECK(state.location->end_object == UINT64_MAX && state.sub.raw()._opaque == 17);
        const moq::event::publish_state &legacy = state;
        const auto &[family, sub, pub, hf, forward, hl, filter, group, object] = legacy;
        (void)std::tie(family, sub, pub, hf, forward, hl, filter, group, object);
    }
    raw.u.publish_state.has_filter = false;
    raw.u.publish_state.family = MOQ_REQUEST_FAMILY_FETCH;
    MOQ_CHECK(std::holds_alternative<moq::event::unknown>(read()));
    raw.u.publish_state.family = MOQ_REQUEST_FAMILY_PUBLISH;
    raw.u.publish_state.location = reinterpret_cast<const moq_location_filter_cfg_t *>(1);
    MOQ_CHECK(!std::get<moq::extended_event::publish_state>(read()).location);
    MOQ_CHECK(std::holds_alternative<moq::event::unknown>(moq::polled_event(raw).variant()));
    MOQ_PASS("state snapshot size gates, future values and fixed legacy shape");
}

int main()
{
    int failures = 0;
    test_reset(failures);
    test_action_alternatives(failures);
    test_action_lifetime(failures);
    test_fill_lifetime(failures);
    test_update_snapshot(failures);
    test_terminal_projection(failures);
    test_legacy_update_projection(failures);
    test_publication_options(failures);
    test_admission_prefixes(failures);
    test_state_prefixes(failures);
    return failures != 0;
}
