#include <moq/session.h>
#include <stddef.h>

enum { legacy_action_size = 96, legacy_reset_detail_size = 16 };
_Static_assert(sizeof(moq_action_t) == legacy_action_size, "old action envelope");
_Static_assert(offsetof(moq_action_t, u) == 16, "old action detail offset");
_Static_assert(offsetof(moq_reset_data_action_t, error_code) == 8, "old reset prefix");

moq_result_t legacy_poll(moq_session_t *session, moq_action_t *actions,
                         size_t capacity, size_t *count)
{
    return moq_session_poll_actions_ex(session, actions, capacity,
                                       legacy_action_size, count);
}

uint64_t legacy_reset_code(const moq_action_t *action)
{
    if (action->kind != MOQ_ACTION_RESET_DATA ||
        action->detail_size < legacy_reset_detail_size) return 0;
    return action->u.reset_data.error_code;
}

uint64_t legacy_reset_roundtrip(void)
{
    moq_action_t action = {
        .kind = MOQ_ACTION_RESET_DATA,
        .detail_size = legacy_reset_detail_size,
        .u.reset_data = {.stream_ref = {._v = 11}, .error_code = 7}
    };
    return legacy_reset_code(&action);
}
