// Interface shapes as they were before draft-22 support was added. This
// client uses only those pre-D22 interfaces.
#include <moq/moq.hpp>
#include <tuple>
#include <type_traits>

uint64_t legacy_reset_decomposition(const moq::action::reset_data &);
moq::action::reset_data legacy_reset_construction(moq::stream_ref);
int legacy_action_visit(const moq::action_variant &);

static_assert(std::is_aggregate_v<moq::action::reset_data>);
auto baseline_members(const moq::action::reset_data &value)
{
    const auto &[f0, f1] = value;
    return std::tie(f0, f1);
}

static_assert(std::is_aggregate_v<moq::event::namespace_done>);
auto baseline_members(const moq::event::namespace_done &value)
{
    const auto &[f0] = value;
    return std::tie(f0);
}

static_assert(std::is_aggregate_v<moq::event::namespace_cancelled>);
auto baseline_members(const moq::event::namespace_cancelled &value)
{
    const auto &[f0, f1, f2] = value;
    return std::tie(f0, f1, f2);
}

static_assert(std::is_aggregate_v<moq::event::unsubscribed>);
auto baseline_members(const moq::event::unsubscribed &value)
{
    const auto &[f0] = value;
    return std::tie(f0);
}

static_assert(std::is_aggregate_v<moq::event::subscribe_updated>);
auto baseline_members(const moq::event::subscribe_updated &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14);
}

static_assert(std::is_aggregate_v<moq::event::fetch_request>);
auto baseline_members(const moq::event::fetch_request &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11);
}

static_assert(std::is_aggregate_v<moq::event::fetch_cancelled>);
auto baseline_members(const moq::event::fetch_cancelled &value)
{
    const auto &[f0] = value;
    return std::tie(f0);
}

static_assert(std::is_aggregate_v<moq::event::fetch_reset>);
auto baseline_members(const moq::event::fetch_reset &value)
{
    const auto &[f0, f1, f2, f3] = value;
    return std::tie(f0, f1, f2, f3);
}

static_assert(std::is_aggregate_v<moq::event::publish_request>);
auto baseline_members(const moq::event::publish_request &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12);
}

static_assert(std::is_aggregate_v<moq::event::publish_finished>);
auto baseline_members(const moq::event::publish_finished &value)
{
    const auto &[f0, f1, f2, f3] = value;
    return std::tie(f0, f1, f2, f3);
}

static_assert(std::is_aggregate_v<moq::event::publish_unsubscribed>);
auto baseline_members(const moq::event::publish_unsubscribed &value)
{
    const auto &[f0] = value;
    return std::tie(f0);
}

static_assert(std::is_aggregate_v<moq::event::publish_updated>);
auto baseline_members(const moq::event::publish_updated &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14);
}

static_assert(std::is_aggregate_v<moq::event::subscribe_tracks_cancelled>);
auto baseline_members(const moq::event::subscribe_tracks_cancelled &value)
{
    const auto &[f0] = value;
    return std::tie(f0);
}

static_assert(std::is_aggregate_v<moq::subscribe_config>);
auto baseline_members(const moq::subscribe_config &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10);
}

static_assert(std::is_aggregate_v<moq::subscription_update_config>);
auto baseline_members(const moq::subscription_update_config &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13);
}

static_assert(std::is_aggregate_v<moq::fetch_config>);
auto baseline_members(const moq::fetch_config &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14);
}

static_assert(std::is_aggregate_v<moq::publish_config>);
auto baseline_members(const moq::publish_config &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7);
}

static_assert(std::is_aggregate_v<moq::accept_publish_config>);
auto baseline_members(const moq::accept_publish_config &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10);
}

static_assert(std::is_aggregate_v<moq::publication_update_config>);
auto baseline_members(const moq::publication_update_config &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8);
}

static_assert(std::is_aggregate_v<moq::session_config>);
auto baseline_members(const moq::session_config &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6, f7, f8, f9, f10, f11, f12, f13, f14, f15, f16, f17, f18, f19, f20, f21, f22, f23, f24, f25, f26);
}

static_assert(std::is_aggregate_v<moq::publisher_config>);
auto baseline_members(const moq::publisher_config &value)
{
    const auto &[f0, f1, f2] = value;
    return std::tie(f0, f1, f2);
}

static_assert(std::is_aggregate_v<moq::subscriber_config>);
auto baseline_members(const moq::subscriber_config &value)
{
    const auto &[f0, f1, f2, f3, f4, f5, f6] = value;
    return std::tie(f0, f1, f2, f3, f4, f5, f6);
}

static_assert(std::is_aggregate_v<moq::sub_track_config>);
auto baseline_members(const moq::sub_track_config &value)
{
    const auto &[f0, f1, f2, f3, f4, f5] = value;
    return std::tie(f0, f1, f2, f3, f4, f5);
}

using baseline_events = std::variant<
    moq::event::setup_complete,
    moq::event::session_closed,
    moq::event::subscribe_request,
    moq::event::subscribe_ok,
    moq::event::subscribe_error,
    moq::event::request_ready,
    moq::event::object_received,
    moq::event::namespace_published,
    moq::event::namespace_done,
    moq::event::namespace_rejected,
    moq::event::namespace_accepted,
    moq::event::namespace_cancelled,
    moq::event::goaway,
    moq::event::request_redirect,
    moq::event::request_goaway,
    moq::event::unsubscribed,
    moq::event::subscribe_updated,
    moq::event::object_chunk,
    moq::event::ns_sub_request,
    moq::event::ns_sub_ok,
    moq::event::ns_sub_error,
    moq::event::namespace_found,
    moq::event::namespace_gone,
    moq::event::fetch_request,
    moq::event::fetch_error,
    moq::event::fetch_cancelled,
    moq::event::fetch_ok,
    moq::event::fetch_complete,
    moq::event::fetch_object,
    moq::event::fetch_gap,
    moq::event::publish_request,
    moq::event::publish_ok,
    moq::event::publish_error,
    moq::event::publish_finished,
    moq::event::track_status_request,
    moq::event::track_status_ok,
    moq::event::track_status_error,
    moq::event::subscribe_tracks_request,
    moq::event::subscribe_tracks_ok,
    moq::event::subscribe_tracks_error,
    moq::event::publish_blocked,
    moq::event::subscribe_tracks_cancelled,
    moq::event::subscribe_done,
    moq::event::publish_unsubscribed,
    moq::event::publish_updated,
    moq::event::subscription_update_ok,
    moq::event::publication_update_ok,
    moq::event::fetch_reset,
    moq::event::unknown>;
static_assert(std::is_same_v<moq::event_variant, baseline_events>);

int legacy_event_visit(const moq::event_variant &value)
{
    return std::visit(moq::overloaded{
        [](const moq::event::setup_complete &) { return 0; },
        [](const moq::event::session_closed &) { return 1; },
        [](const moq::event::subscribe_request &) { return 2; },
        [](const moq::event::subscribe_ok &) { return 3; },
        [](const moq::event::subscribe_error &) { return 4; },
        [](const moq::event::request_ready &) { return 5; },
        [](const moq::event::object_received &) { return 6; },
        [](const moq::event::namespace_published &) { return 7; },
        [](const moq::event::namespace_done &) { return 8; },
        [](const moq::event::namespace_rejected &) { return 9; },
        [](const moq::event::namespace_accepted &) { return 10; },
        [](const moq::event::namespace_cancelled &) { return 11; },
        [](const moq::event::goaway &) { return 12; },
        [](const moq::event::request_redirect &) { return 13; },
        [](const moq::event::request_goaway &) { return 14; },
        [](const moq::event::unsubscribed &) { return 15; },
        [](const moq::event::subscribe_updated &) { return 16; },
        [](const moq::event::object_chunk &) { return 17; },
        [](const moq::event::ns_sub_request &) { return 18; },
        [](const moq::event::ns_sub_ok &) { return 19; },
        [](const moq::event::ns_sub_error &) { return 20; },
        [](const moq::event::namespace_found &) { return 21; },
        [](const moq::event::namespace_gone &) { return 22; },
        [](const moq::event::fetch_request &) { return 23; },
        [](const moq::event::fetch_error &) { return 24; },
        [](const moq::event::fetch_cancelled &) { return 25; },
        [](const moq::event::fetch_ok &) { return 26; },
        [](const moq::event::fetch_complete &) { return 27; },
        [](const moq::event::fetch_object &) { return 28; },
        [](const moq::event::fetch_gap &) { return 29; },
        [](const moq::event::publish_request &) { return 30; },
        [](const moq::event::publish_ok &) { return 31; },
        [](const moq::event::publish_error &) { return 32; },
        [](const moq::event::publish_finished &) { return 33; },
        [](const moq::event::track_status_request &) { return 34; },
        [](const moq::event::track_status_ok &) { return 35; },
        [](const moq::event::track_status_error &) { return 36; },
        [](const moq::event::subscribe_tracks_request &) { return 37; },
        [](const moq::event::subscribe_tracks_ok &) { return 38; },
        [](const moq::event::subscribe_tracks_error &) { return 39; },
        [](const moq::event::publish_blocked &) { return 40; },
        [](const moq::event::subscribe_tracks_cancelled &) { return 41; },
        [](const moq::event::subscribe_done &) { return 42; },
        [](const moq::event::publish_unsubscribed &) { return 43; },
        [](const moq::event::publish_updated &) { return 44; },
        [](const moq::event::subscription_update_ok &) { return 45; },
        [](const moq::event::publication_update_ok &) { return 46; },
        [](const moq::event::fetch_reset &) { return 47; },
        [](const moq::event::unknown &) { return 48; }
    }, value);
}

template<class T>
void reconstruct()
{
    T value{};
    auto copy = std::apply([](const auto &...fields) { return T{fields...}; },
                           baseline_members(value));
    static_cast<void>(copy);
}

int legacy_range_switch(moq::fetch_range_kind value)
{
    switch (value) {
    case moq::fetch_range_kind::non_existent: return 1;
    case moq::fetch_range_kind::unknown: return 2;
    }
    return 0;
}

int legacy_error_switch(moq::request_error value)
{
    switch (value) {
    case moq::request_error::internal_error:
    case moq::request_error::unauthorized:
    case moq::request_error::timeout:
    case moq::request_error::not_supported:
    case moq::request_error::malformed_auth_token:
    case moq::request_error::expired_auth_token:
    case moq::request_error::excessive_load:
    case moq::request_error::malformed_track:
    case moq::request_error::uninterested:
    case moq::request_error::namespace_too_large:
    case moq::request_error::going_away:
    case moq::request_error::does_not_exist:
    case moq::request_error::invalid_range:
    case moq::request_error::duplicate_subscription:
    case moq::request_error::prefix_overlap:
    case moq::request_error::invalid_joining_request_id:
    case moq::request_error::unsupported_extension:
    case moq::request_error::redirect: return 1;
    }
    return 0;
}

int main()
{
    reconstruct<moq::action::reset_data>();
    reconstruct<moq::event::namespace_done>();
    reconstruct<moq::event::namespace_cancelled>();
    reconstruct<moq::event::unsubscribed>();
    reconstruct<moq::event::subscribe_updated>();
    reconstruct<moq::event::fetch_request>();
    reconstruct<moq::event::fetch_cancelled>();
    reconstruct<moq::event::fetch_reset>();
    reconstruct<moq::event::publish_request>();
    reconstruct<moq::event::publish_finished>();
    reconstruct<moq::event::publish_unsubscribed>();
    reconstruct<moq::event::publish_updated>();
    reconstruct<moq::event::subscribe_tracks_cancelled>();
    reconstruct<moq::subscribe_config>();
    reconstruct<moq::subscription_update_config>();
    reconstruct<moq::fetch_config>();
    reconstruct<moq::publish_config>();
    reconstruct<moq::accept_publish_config>();
    reconstruct<moq::publication_update_config>();
    reconstruct<moq::session_config>();
    reconstruct<moq::publisher_config>();
    reconstruct<moq::subscriber_config>();
    reconstruct<moq::sub_track_config>();
    const auto reset = legacy_reset_construction(moq::stream_ref(UINT64_C(5)));
    return legacy_reset_decomposition(reset) != 12 ||
           legacy_action_visit(reset) != 3 ||
           legacy_event_visit(moq::event::unknown{}) != 48;
}
