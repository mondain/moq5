/* Public initializer ABI: independent old extent and every caller-size boundary. */
#include <moq/media_receiver.h>
#include "test_support.h"
#include <stdlib.h>
#include <string.h>

typedef struct receiver_cfg_v0 {
    uint32_t struct_size;
    const moq_endpoint_cfg_t *endpoint;
    moq_namespace_t namespace_;
    moq_bytes_t catalog_track;
    bool auto_subscribe;
    moq_media_time_mode_t time_mode;
    moq_media_overflow_cfg_t overflow;
    uint32_t max_track_events;
} receiver_cfg_v0_t;

typedef struct track_cfg_v0 {
    uint32_t struct_size;
    moq_media_start_mode_t start;
    bool has_priority;
    uint8_t priority;
} track_cfg_v0_t;

#define CHECK_TRACK_OFFSET(field) \
    _Static_assert(offsetof(moq_media_receiver_track_subscribe_cfg_t, field) == \
                   offsetof(track_cfg_v0_t, field), "old track " #field " offset")
CHECK_TRACK_OFFSET(struct_size);
CHECK_TRACK_OFFSET(start);
CHECK_TRACK_OFFSET(has_priority);
CHECK_TRACK_OFFSET(priority);
#undef CHECK_TRACK_OFFSET
_Static_assert(offsetof(moq_media_receiver_track_subscribe_cfg_t, location) >= sizeof(track_cfg_v0_t),
               "track options start beyond complete old extent");

_Static_assert(offsetof(moq_media_receiver_cfg_t, companion_profile) >=
               sizeof(receiver_cfg_v0_t), "companion does not reuse old padding");

static int failures;

static void test_track_initializers(void)
{
    void (*old_init)(moq_media_receiver_track_subscribe_cfg_t *) =
        moq_media_receiver_track_subscribe_cfg_init;
    void (*sized_init)(moq_media_receiver_track_subscribe_cfg_t *, size_t) =
        moq_media_receiver_track_subscribe_cfg_init_sized;
    old_init(NULL);
    sized_init(NULL, SIZE_MAX);
    const size_t guard = sizeof(max_align_t);
    for (size_t size = 0; size <= sizeof(moq_media_receiver_track_subscribe_cfg_t) + 16; size++) {
        size_t total = guard + size + guard;
        unsigned char *storage = malloc(total);
        unsigned char *expected = malloc(total);
        MOQ_TEST_CHECK(storage != NULL && expected != NULL);
        if (!storage || !expected) { free(storage); free(expected); return; }
        memset(storage, 0xa5, total);
        memset(expected, 0xa5, total);
        size_t n = size < sizeof(moq_media_receiver_track_subscribe_cfg_t)
                     ? size : sizeof(moq_media_receiver_track_subscribe_cfg_t);
        if (n >= sizeof(uint32_t)) {
            memset(expected + guard, 0, n);
            uint32_t stamp = (uint32_t)n;
            memcpy(expected + guard, &stamp, sizeof(stamp));
        }
        sized_init((moq_media_receiver_track_subscribe_cfg_t *)(storage + guard), size);
        MOQ_TEST_CHECK(memcmp(storage, expected, total) == 0);
        free(storage); free(expected);
    }
    track_cfg_v0_t *old = malloc(sizeof(*old));
    MOQ_TEST_CHECK(old != NULL);
    if (!old) return;
    memset(old, 0xa5, sizeof(*old));
    old_init((moq_media_receiver_track_subscribe_cfg_t *)old);
    MOQ_TEST_CHECK(old->struct_size == sizeof(*old));
    MOQ_TEST_CHECK(old->start == MOQ_MEDIA_START_CURRENT && !old->has_priority);
    free(old);
    /* An old config can be embedded at its original, weaker alignment. */
    if (_Alignof(track_cfg_v0_t) < _Alignof(moq_media_receiver_track_subscribe_cfg_t)) {
        const size_t offset = _Alignof(track_cfg_v0_t);
        unsigned char *storage = malloc(offset + sizeof(track_cfg_v0_t) + 8);
        MOQ_TEST_CHECK(storage != NULL);
        if (!storage) return;
        memset(storage, 0xa5, offset + sizeof(track_cfg_v0_t) + 8);
        old_init((moq_media_receiver_track_subscribe_cfg_t *)(storage + offset));
        track_cfg_v0_t copied;
        memcpy(&copied, storage + offset, sizeof(copied));
        MOQ_TEST_CHECK(copied.struct_size == sizeof(copied));
        MOQ_TEST_CHECK(copied.start == MOQ_MEDIA_START_CURRENT);
        for (size_t i = 0; i < offset; i++) MOQ_TEST_CHECK(storage[i] == 0xa5);
        for (size_t i = offset + sizeof(copied); i < offset + sizeof(copied) + 8; i++)
            MOQ_TEST_CHECK(storage[i] == 0xa5);
        free(storage);
    }
    moq_media_receiver_track_subscribe_cfg_t full;
    memset(&full, 0xa5, sizeof(full));
    old_init(&full);
    MOQ_TEST_CHECK(full.struct_size == sizeof(track_cfg_v0_t));
    for (size_t n = sizeof(track_cfg_v0_t); n < sizeof(full); n++)
        MOQ_TEST_CHECK(((const unsigned char *)&full)[n] == 0xa5);
    sized_init(&full, SIZE_MAX);
    MOQ_TEST_CHECK(full.struct_size == sizeof(full));
    MOQ_TEST_CHECK(full.start == MOQ_MEDIA_START_CURRENT && !full.has_priority);
    MOQ_TEST_CHECK(full.location == NULL && full.fill == NULL);
    MOQ_TEST_PASS("receiver_track_initializers_all_sizes");
}

static void test_initializers(void)
{
    void (*const old_init[])(moq_media_receiver_cfg_t *) = {
        moq_media_receiver_cfg_init,
        moq_media_receiver_cfg_init_live,
        moq_media_receiver_cfg_init_flow_control
    };
    void (*const sized_init[])(moq_media_receiver_cfg_t *, size_t) = {
        moq_media_receiver_cfg_init_sized,
        moq_media_receiver_cfg_init_live_sized,
        moq_media_receiver_cfg_init_flow_control_sized
    };
    const moq_media_overflow_policy_t policies[] = {
        MOQ_MEDIA_OVERFLOW_UNSET,
        MOQ_MEDIA_OVERFLOW_DROP_TO_KEYFRAME,
        MOQ_MEDIA_OVERFLOW_FLOW_CONTROL
    };
    const size_t policy_offset = offsetof(moq_media_receiver_cfg_t, overflow) +
                                 offsetof(moq_media_overflow_cfg_t, policy);
    const size_t companion_offset = offsetof(moq_media_receiver_cfg_t, companion_profile);
    const moq_media_companion_profile_t auto_companion = MOQ_MEDIA_COMPANION_AUTO;
    const size_t guard = sizeof(max_align_t);

    for (size_t variant = 0; variant < 3; ++variant) {
        old_init[variant](NULL);
        sized_init[variant](NULL, SIZE_MAX);
        for (size_t size = 0; size <= sizeof(moq_media_receiver_cfg_t) + 16; ++size) {
            size_t total = guard + size + guard;
            unsigned char *storage = malloc(total);
            unsigned char *expected = malloc(total);
            MOQ_TEST_CHECK(storage != NULL && expected != NULL);
            if (!storage || !expected) {
                free(expected);
                free(storage);
                return;
            }
            memset(storage, 0xa5, total);
            memset(expected, 0xa5, total);
            size_t n = size < sizeof(moq_media_receiver_cfg_t)
                         ? size : sizeof(moq_media_receiver_cfg_t);
            if (n >= sizeof(uint32_t)) {
                memset(expected + guard, 0, n);
                uint32_t stamp = (uint32_t)n;
                memcpy(expected + guard, &stamp, sizeof(stamp));
                if (n >= policy_offset + sizeof(policies[variant]))
                    memcpy(expected + guard + policy_offset, &policies[variant],
                           sizeof(policies[variant]));
                /* The companion default is stamped only when wholly covered. */
                if (n >= companion_offset + sizeof(auto_companion))
                    memcpy(expected + guard + companion_offset, &auto_companion,
                           sizeof(auto_companion));
            }
            sized_init[variant]((moq_media_receiver_cfg_t *)(storage + guard), size);
            MOQ_TEST_CHECK(memcmp(storage, expected, total) == 0);
            free(expected);
            free(storage);
        }

        receiver_cfg_v0_t *old = malloc(sizeof(*old));
        MOQ_TEST_CHECK(old != NULL);
        if (!old) return;
        memset(old, 0xa5, sizeof(*old));
        old_init[variant]((moq_media_receiver_cfg_t *)old);
        MOQ_TEST_CHECK(old->struct_size == sizeof(*old));
        MOQ_TEST_CHECK(old->overflow.policy == policies[variant]);
        MOQ_TEST_CHECK(old->endpoint == NULL && old->namespace_.parts == NULL);
        free(old);

        moq_media_receiver_cfg_t full;
        memset(&full, 0xa5, sizeof(full));
        sized_init[variant](&full, SIZE_MAX);
        MOQ_TEST_CHECK(full.struct_size == sizeof(full));
        MOQ_TEST_CHECK(full.overflow.policy == policies[variant]);
        MOQ_TEST_CHECK(full.companion_profile == MOQ_MEDIA_COMPANION_AUTO);
        MOQ_TEST_CHECK(full.time_mode == MOQ_MEDIA_TIME_RAW);
        MOQ_TEST_CHECK(full.overflow.max_objects == 0 && full.overflow.max_bytes == 0);
        MOQ_TEST_CHECK(full.max_track_events == 0 && !full.auto_subscribe);
    }
    MOQ_TEST_PASS("receiver_initializers_all_sizes");
}

int main(void)
{
    test_initializers();
    test_track_initializers();
    return failures ? 1 : 0;
}
