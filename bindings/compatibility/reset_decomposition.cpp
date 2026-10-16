#include <moq/action.hpp>

uint64_t legacy_reset_decomposition(const moq::action::reset_data &action)
{
    const auto &[stream, error] = action;
    return stream.value() + error;
}

moq::action::reset_data legacy_reset_construction(moq::stream_ref stream)
{
    return {stream, 7};
}
