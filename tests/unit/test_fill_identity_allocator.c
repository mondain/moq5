#include "../../core/src/facade/subscriber.c"
#include "test_support.h"

static int failures;

static void test_reservation(void)
{
    sub_fill_identity_allocator_t ids = {
        .lock = ATOMIC_FLAG_INIT, .last_issued = UINT64_MAX - 1
    };
    moq_sub_fill_req_t slot = {0};
    moq_subscriber_t owner = {0};
    moq_sub_track_t track = {0};
    owner.fill_reqs = &slot;
    owner.fill_cap = 1;
    slot.sub = &owner;
    track.sub = &owner;
    moq_sub_fill_req_t *out = NULL;
    MOQ_TEST_CHECK(alloc_fill_req(&owner, &track, &ids, &out) == MOQ_OK);
    MOQ_TEST_CHECK(out == &slot);
    MOQ_TEST_CHECK_EQ_U64(slot.seq, UINT64_MAX);
    MOQ_TEST_CHECK(slot.state == SUB_FILL_REQUESTED && slot.track == &track);
    MOQ_TEST_CHECK(fill_resolve(&owner, fill_handle_of(&slot)) == &slot);
    MOQ_TEST_PASS("last_identity_reserves_one_slot");

    free_fill_req(&slot);
    moq_sub_fill_req_t slot_before;
    moq_subscriber_t owner_before;
    moq_sub_track_t track_before;
    memcpy(&slot_before, &slot, sizeof(slot));
    memcpy(&owner_before, &owner, sizeof(owner));
    memcpy(&track_before, &track, sizeof(track));
    for (unsigned i = 0; i < 3; ++i) {
        out = NULL;
        MOQ_TEST_CHECK(alloc_fill_req(&owner, &track, &ids, &out) == MOQ_ERR_LIMIT);
        MOQ_TEST_CHECK(out == NULL);
        MOQ_TEST_CHECK(memcmp(&slot, &slot_before, sizeof(slot)) == 0);
        MOQ_TEST_CHECK(memcmp(&owner, &owner_before, sizeof(owner)) == 0);
        MOQ_TEST_CHECK(memcmp(&track, &track_before, sizeof(track)) == 0);
        MOQ_TEST_CHECK_EQ_U64(ids.last_issued, UINT64_MAX);
    }
    MOQ_TEST_PASS("exhausted_reservation_limit_without_owner_effects");

    sub_fill_identity_allocator_t available = SUB_FILL_IDENTITY_ALLOCATOR_INIT;
    slot.state = SUB_FILL_ACTIVE;
    memcpy(&slot_before, &slot, sizeof(slot));
    MOQ_TEST_CHECK(alloc_fill_req(&owner, &track, &available, &out) == MOQ_ERR_WOULD_BLOCK);
    MOQ_TEST_CHECK(out == NULL);
    MOQ_TEST_CHECK_EQ_U64(available.last_issued, 0);
    MOQ_TEST_CHECK(memcmp(&slot, &slot_before, sizeof(slot)) == 0);
    MOQ_TEST_PASS("occupied_slot_is_temporary_and_does_not_issue_identity");
}

int main(void)
{
    sub_fill_identity_allocator_t fresh = SUB_FILL_IDENTITY_ALLOCATOR_INIT;
    uint64_t value = UINT64_MAX;
    MOQ_TEST_CHECK(sub_fill_identity_next(&fresh, &value));
    MOQ_TEST_CHECK_EQ_U64(value, 1);
    MOQ_TEST_CHECK(sub_fill_identity_next(&fresh, &value));
    MOQ_TEST_CHECK_EQ_U64(value, 2);
    MOQ_TEST_PASS("nonzero_monotonic_identity");

    sub_fill_identity_allocator_t last = {
        .lock = ATOMIC_FLAG_INIT, .last_issued = UINT64_MAX - 1
    };
    MOQ_TEST_CHECK(sub_fill_identity_next(&last, &value));
    MOQ_TEST_CHECK_EQ_U64(value, UINT64_MAX);
    for (unsigned i = 0; i < 4; ++i) {
        value = 37;
        MOQ_TEST_CHECK(!sub_fill_identity_next(&last, &value));
        MOQ_TEST_CHECK_EQ_U64(value, 37);
        MOQ_TEST_CHECK_EQ_U64(last.last_issued, UINT64_MAX);
    }
    MOQ_TEST_CHECK(!atomic_flag_test_and_set_explicit(&last.lock,
                                                     memory_order_acquire));
    atomic_flag_clear_explicit(&last.lock, memory_order_release);
    MOQ_TEST_PASS("exhaustion_is_sticky_and_unlocks");

    MOQ_TEST_CHECK(sub_fill_identity_next(&fresh, &value));
    MOQ_TEST_CHECK_EQ_U64(value, 3);
    MOQ_TEST_PASS("private_instance_exhaustion_is_isolated");
    test_reservation();
    printf("%s: %d failures\n", failures ? "FAIL" : "PASS", failures);
    return failures ? 1 : 0;
}
