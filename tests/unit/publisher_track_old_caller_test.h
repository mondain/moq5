#ifndef TEST_PUBLISHER_TRACK_OLD_CALLER_TEST_H
#define TEST_PUBLISHER_TRACK_OLD_CALLER_TEST_H

#include <moq/session.h>

struct moq_publisher;
struct moq_pub_track;
struct moq_simpair;
typedef struct publisher_track_old_context {
    struct moq_publisher *pub;
    struct moq_simpair *pair;
    const moq_alloc_t *alloc;
    size_t *alloc_calls;
    size_t *live;
    size_t *fail_at;
    moq_result_t refusal;
} publisher_track_old_context_t;

enum { PUB_TRACK_OLD_INIT, PUB_TRACK_OLD_SIZED, PUB_TRACK_OLD_ADD, PUB_TRACK_OLD_REFUSAL };
int publisher_track_old_check(bool condition, const char *file, int line);
int publisher_track_old_behavior(publisher_track_old_context_t *ctx,
    struct moq_pub_track *track, uint64_t budget, uint8_t priority, bool monotonic);
int publisher_track_old_original(unsigned operation, publisher_track_old_context_t *ctx);
int publisher_track_old_pre_monotonic(unsigned operation, publisher_track_old_context_t *ctx);

#endif
