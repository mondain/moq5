#ifndef TEST_PUBLISHER_OLD_LAYOUT_H
#define TEST_PUBLISHER_OLD_LAYOUT_H

#include <moq/session.h>

/* Historical records, deliberately independent of moq/publisher.h. */
struct moq_publisher;
struct moq_pub_track;
struct moq_pub_subscribe_info;
struct moq_pub_subscribe_info_v1;
struct moq_pub_subscribe_update_info;
struct moq_pub_namespace_terminal_info;

typedef enum test_pub_old_mode {
    TEST_PUB_OLD_REJECT = 0, TEST_PUB_OLD_ACCEPT = 1,
    TEST_PUB_OLD_CALLBACK = 2, TEST_PUB_OLD_CALLBACK_V1 = 3
} test_pub_old_mode_t;
typedef enum test_pub_old_decision {
    TEST_PUB_OLD_ALLOW = 0, TEST_PUB_OLD_DENY = 1, TEST_PUB_OLD_DEFER = 2
} test_pub_old_decision_t;
typedef test_pub_old_decision_t (*test_pub_old_subscribe_cb)(
    void *, const struct moq_pub_subscribe_info *, moq_request_error_t *);
typedef test_pub_old_decision_t (*test_pub_old_subscribe_v1_cb)(
    void *, const struct moq_pub_subscribe_info_v1 *, moq_request_error_t *);

typedef struct test_pub_old_callbacks {
    uint32_t struct_size;
    void *ctx;
    void (*on_subscriber_joined)(void *, struct moq_pub_track *);
    void (*on_subscriber_left)(void *, struct moq_pub_track *);
    void (*on_draining)(void *);
    void (*on_closed)(void *, uint64_t);
    void (*on_subscriber_updated)(void *, struct moq_pub_track *,
                                  const struct moq_pub_subscribe_update_info *);
    void (*on_publish_ok)(void *, struct moq_pub_track *, bool);
    void (*on_publish_error)(void *, struct moq_pub_track *, moq_request_error_t);
    void (*on_publish_forward_changed)(void *, struct moq_pub_track *, bool);
    void (*on_publish_finished)(void *, struct moq_pub_track *);
    void (*on_namespace_terminal)(void *, const struct moq_pub_namespace_terminal_info *);
} test_pub_old_callbacks_t;

typedef struct test_pub_cfg_v0 {
    uint32_t struct_size;
    test_pub_old_mode_t accept_mode;
    uint8_t default_publisher_priority;
} test_pub_cfg_v0_t;

typedef struct test_pub_cfg_callbacks {
    uint32_t struct_size;
    test_pub_old_mode_t accept_mode;
    uint8_t default_publisher_priority;
    test_pub_old_subscribe_cb on_subscribe;
    void *on_subscribe_ctx;
    test_pub_old_callbacks_t callbacks;
} test_pub_cfg_callbacks_t;

typedef struct test_pub_cfg_pre_v1 {
    uint32_t struct_size;
    test_pub_old_mode_t accept_mode;
    uint8_t default_publisher_priority;
    test_pub_old_subscribe_cb on_subscribe;
    void *on_subscribe_ctx;
    test_pub_old_callbacks_t callbacks;
    uint32_t max_fills;
} test_pub_cfg_pre_v1_t;

typedef struct test_pub_cfg_v1 {
    uint32_t struct_size;
    test_pub_old_mode_t accept_mode;
    uint8_t default_publisher_priority;
    test_pub_old_subscribe_cb on_subscribe;
    void *on_subscribe_ctx;
    test_pub_old_callbacks_t callbacks;
    uint32_t max_fills;
    test_pub_old_subscribe_v1_cb on_subscribe_v1;
    void *on_subscribe_v1_ctx;
} test_pub_cfg_v1_t;

#ifdef __cplusplus
#define OLD_PIN static_assert
#define OLD_ALIGN alignof
#else
#define OLD_PIN _Static_assert
#define OLD_ALIGN _Alignof
#endif
#define OLD_DIM(n64, s64, n32, s32) (sizeof(void *) == 8 ? \
    (sizeof(test_pub_old_mode_t) == 4 ? (n64) : (s64)) : \
    (sizeof(test_pub_old_mode_t) == 4 ? (n32) : (s32)))
#define OLD_FIELD(T, f, n64, s64, n32, s32, width) \
    OLD_PIN(offsetof(T, f) == OLD_DIM(n64, s64, n32, s32), #T "." #f " offset"); \
    OLD_PIN(sizeof(((T *)0)->f) == (width), #T "." #f " width")
#define OLD_BASE(T) \
    OLD_FIELD(T, struct_size, 0, 0, 0, 0, 4); \
    OLD_FIELD(T, accept_mode, 4, 4, 4, 4, sizeof(test_pub_old_mode_t)); \
    OLD_FIELD(T, default_publisher_priority, 8, 5, 8, 5, 1)
#define OLD_CALLBACKS(T) \
    OLD_BASE(T); \
    OLD_FIELD(T, on_subscribe, 16, 8, 12, 8, sizeof(void *)); \
    OLD_FIELD(T, on_subscribe_ctx, 24, 16, 16, 12, sizeof(void *)); \
    OLD_FIELD(T, callbacks, 32, 24, 20, 16, sizeof(test_pub_old_callbacks_t))
OLD_PIN(sizeof(void *) == 4 || sizeof(void *) == 8, "supported pointer widths");
OLD_PIN(sizeof(test_pub_old_mode_t) == 1 || sizeof(test_pub_old_mode_t) == 4,
        "separate normal/short enum ABIs");
OLD_PIN(sizeof(test_pub_cfg_v0_t) == OLD_DIM(12, 8, 12, 8), "v0 size");
OLD_PIN(OLD_ALIGN(test_pub_cfg_v0_t) == 4, "v0 alignment");
OLD_PIN(sizeof(test_pub_cfg_callbacks_t) == OLD_DIM(128, 120, 68, 64), "callbacks cfg size");
OLD_PIN(sizeof(test_pub_cfg_pre_v1_t) == OLD_DIM(136, 128, 72, 68), "pre-V1 size");
OLD_PIN(sizeof(test_pub_cfg_v1_t) == OLD_DIM(152, 144, 80, 76), "V1 size");
OLD_PIN(OLD_ALIGN(test_pub_cfg_callbacks_t) == (sizeof(void *) == 8 ? 8 : 4),
        "callbacks cfg alignment");
OLD_PIN(OLD_ALIGN(test_pub_cfg_pre_v1_t) == (sizeof(void *) == 8 ? 8 : 4), "pre-V1 alignment");
OLD_PIN(OLD_ALIGN(test_pub_cfg_v1_t) == (sizeof(void *) == 8 ? 8 : 4), "V1 alignment");
OLD_BASE(test_pub_cfg_v0_t);
OLD_CALLBACKS(test_pub_cfg_callbacks_t);
OLD_CALLBACKS(test_pub_cfg_pre_v1_t);
OLD_CALLBACKS(test_pub_cfg_v1_t);
OLD_FIELD(test_pub_cfg_pre_v1_t, max_fills, 128, 120, 68, 64, 4);
OLD_FIELD(test_pub_cfg_v1_t, max_fills, 128, 120, 68, 64, 4);
OLD_FIELD(test_pub_cfg_v1_t, on_subscribe_v1, 136, 128, 72, 68, sizeof(void *));
OLD_FIELD(test_pub_cfg_v1_t, on_subscribe_v1_ctx, 144, 136, 76, 72, sizeof(void *));
OLD_PIN(sizeof(test_pub_old_callbacks_t) == (sizeof(void *) == 8 ? 96 : 48),
        "frozen callbacks size");
OLD_PIN(OLD_ALIGN(test_pub_old_callbacks_t) == (sizeof(void *) == 8 ? 8 : 4),
        "frozen callbacks alignment");
#define OLD_CB(f, n64, n32, width) \
    OLD_FIELD(test_pub_old_callbacks_t, f, n64, n64, n32, n32, width)
OLD_CB(struct_size, 0, 0, 4);
OLD_CB(ctx, 8, 4, sizeof(void *));
OLD_CB(on_subscriber_joined, 16, 8, sizeof(void *));
OLD_CB(on_subscriber_left, 24, 12, sizeof(void *));
OLD_CB(on_draining, 32, 16, sizeof(void *));
OLD_CB(on_closed, 40, 20, sizeof(void *));
OLD_CB(on_subscriber_updated, 48, 24, sizeof(void *));
OLD_CB(on_publish_ok, 56, 28, sizeof(void *));
OLD_CB(on_publish_error, 64, 32, sizeof(void *));
OLD_CB(on_publish_forward_changed, 72, 36, sizeof(void *));
OLD_CB(on_publish_finished, 80, 40, sizeof(void *));
OLD_CB(on_namespace_terminal, 88, 44, sizeof(void *));
#undef OLD_CB
#undef OLD_CALLBACKS
#undef OLD_BASE
#undef OLD_FIELD
#undef OLD_DIM
#undef OLD_ALIGN
#undef OLD_PIN

#endif
