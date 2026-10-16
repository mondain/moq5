#include <moq/subscriber.h>

#ifdef __cplusplus
#define PIN static_assert
#define ALIGNOF alignof
#else
#define PIN _Static_assert
#define ALIGNOF _Alignof
#endif

#if UINTPTR_MAX == UINT64_MAX
#define FLAGS 16
#define LARGEST 24
#define FULL 80
#define OLD_FULL 80
#define ALIGNMENT 8
#elif UINTPTR_MAX == UINT32_MAX && defined(__i386__)
#define FLAGS 12
#define LARGEST 20
#define FULL 76
#define OLD_FULL 60
#define ALIGNMENT 4
#elif UINTPTR_MAX == UINT32_MAX
#define FLAGS 12
#define LARGEST 24
#define FULL 80
#define OLD_FULL 72
#define ALIGNMENT 8
#else
#error Unsupported layout target
#endif

PIN(MOQ_SUB_STATUS_OUTCOME_OK == 1 && MOQ_SUB_STATUS_OUTCOME_ERROR == 2 &&
    MOQ_SUB_STATUS_OUTCOME_TERMINATED == 3, "outcome constants");
PIN(MOQ_SUB_STATUS_OUTCOME_HAS_LARGEST == 1 && MOQ_SUB_STATUS_OUTCOME_HAS_EXPIRES == 2 &&
    MOQ_SUB_STATUS_OUTCOME_CAN_RETRY == 4, "flag constants");
PIN(offsetof(moq_sub_status_outcome_t, struct_size) == 0, "size offset");
PIN(offsetof(moq_sub_status_outcome_t, kind) == 4, "kind offset");
PIN(offsetof(moq_sub_status_outcome_t, request) == 8, "request offset");
PIN(offsetof(moq_sub_status_outcome_t, flags) == FLAGS, "flags offset");
PIN(offsetof(moq_sub_status_outcome_t, reserved) == FLAGS + 4, "reserved offset");
PIN(offsetof(moq_sub_status_outcome_t, largest_group) == LARGEST, "group offset");
PIN(offsetof(moq_sub_status_outcome_t, largest_object) == LARGEST + 8, "object offset");
PIN(offsetof(moq_sub_status_outcome_t, expires_ms) == LARGEST + 16, "expiry offset");
PIN(offsetof(moq_sub_status_outcome_t, error_code) == LARGEST + 24, "error offset");
PIN(offsetof(moq_sub_status_outcome_t, retry_after_ms) == LARGEST + 32, "retry offset");
PIN(offsetof(moq_sub_status_outcome_t, cause) == LARGEST + 40, "cause offset");
PIN(offsetof(moq_sub_status_outcome_t, local_role) == LARGEST + 44, "role offset");
PIN(offsetof(moq_sub_status_outcome_t, stream_error_code) == LARGEST + 48, "code offset");
PIN(sizeof(moq_sub_status_outcome_t) == FULL, "full outcome extent");
PIN(MOQ_SUB_STATUS_OUTCOME_V0_SIZE == FULL, "physical full v0 floor");
PIN(ALIGNOF(moq_sub_status_outcome_t) == ALIGNMENT, "record alignment");
PIN(sizeof(((moq_sub_status_outcome_t *)0)->kind) == 4, "kind width");
PIN(sizeof(((moq_sub_status_outcome_t *)0)->flags) == 4, "flags width");
PIN(sizeof(((moq_sub_status_outcome_t *)0)->cause) == 4, "cause width");
PIN(sizeof(((moq_sub_status_outcome_t *)0)->local_role) == 4, "role width");
PIN(sizeof(moq_sub_status_result_t) == OLD_FULL, "unchanged legacy layout");
PIN(MOQ_SUB_STATUS_OK == 1 && MOQ_SUB_STATUS_ERROR == 2, "unchanged legacy kinds");

static moq_result_t (*const sized_poll)(moq_subscriber_t *, moq_sub_status_outcome_t *, size_t) =
    moq_sub_poll_status_outcome;
static moq_result_t (*const legacy_poll)(moq_subscriber_t *, moq_sub_status_result_t *) =
    moq_sub_poll_status;

int main(void) { return sized_poll == NULL || legacy_poll == NULL; }
