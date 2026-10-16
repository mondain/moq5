#include <moq/action.hpp>
#include <type_traits>

static_assert(std::variant_size_v<moq::action_variant> == 10,
              "unchanged action_variant has ten alternatives");
static_assert(std::is_same_v<std::variant_alternative_t<9, moq::action_variant>,
                             moq::action::unknown>, "unchanged unknown action index");

int legacy_action_visit(const moq::action_variant &action)
{
    return std::visit(moq::overloaded{
        [](const moq::action::send_control &) { return 0; },
        [](const moq::action::close_session &) { return 1; },
        [](const moq::action::send_data &) { return 2; },
        [](const moq::action::reset_data &) { return 3; },
        [](const moq::action::stop_data &) { return 4; },
        [](const moq::action::open_bidi_stream &) { return 5; },
        [](const moq::action::send_bidi_stream &) { return 6; },
        [](const moq::action::close_bidi_stream &) { return 7; },
        [](const moq::action::send_datagram &) { return 8; },
        [](const moq::action::unknown &) { return 9; }
    }, action);
}
