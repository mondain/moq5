#ifndef TEST_PUBLISHER_OLD_CALLER_TEST_H
#define TEST_PUBLISHER_OLD_CALLER_TEST_H

#include <moq/session.h>

typedef struct publisher_old_test_context {
    moq_session_t *session;
    const moq_alloc_t *alloc;
    size_t *alloc_calls;
    size_t *live;
    bool *fail_alloc;
} publisher_old_test_context_t;

enum { PUB_OLD_INIT, PUB_OLD_SIZED, PUB_OLD_CREATE };
int publisher_old_check(bool condition, const char *file, int line);
int publisher_old_v0(unsigned operation, publisher_old_test_context_t *context);
int publisher_old_callbacks(unsigned operation, publisher_old_test_context_t *context);
int publisher_old_pre_v1(unsigned operation, publisher_old_test_context_t *context);
int publisher_old_v1(unsigned operation, publisher_old_test_context_t *context);

#endif
