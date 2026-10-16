/* The endpoint-failure text names the connect's result (description, symbolic
 * name and code), adds the backend hint only for MOQ_ERR_UNSUPPORTED with a
 * pinned draft, stays NUL-terminated when the buffer is short, and never
 * carries a double quote into the YAML message string. */
#include "endpoint_error.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

static void check_text(moq_result_t rc, uint32_t pin, const char *what,
                       const char *want)
{
    char buf[256];
    const char *got = interop_endpoint_error(buf, sizeof(buf), what, rc, pin);
    CHECK(got == buf);
    if (strcmp(buf, want) != 0) {
        fprintf(stderr, "FAIL: rc=%d pin=%u\n  got:  %s\n  want: %s\n",
                (int)rc, (unsigned)pin, buf, want);
        g_failures++;
    }
    CHECK(strchr(buf, '"') == NULL);
}

int main(void)
{
    check_text(MOQ_ERR_UNSUPPORTED, 22, "endpoint",
               "failed to create endpoint: not supported by this build "
               "(MOQ_ERR_UNSUPPORTED -14); requested draft-22 is not "
               "supported by this transport backend");
    /* AUTO offer: the refusal is not about a requested draft. */
    check_text(MOQ_ERR_UNSUPPORTED, 0, "publisher",
               "failed to create publisher: not supported by this build "
               "(MOQ_ERR_UNSUPPORTED -14)");
    /* Another refusal with a pinned draft carries no backend hint. */
    check_text(MOQ_ERR_INVAL, 18, "subscriber",
               "failed to create subscriber: invalid argument "
               "(MOQ_ERR_INVAL -2)");
    check_text((moq_result_t)-1234, 22, "endpoint",
               "failed to create endpoint: unknown error (result -1234)");
    CHECK(interop_result_name(MOQ_ERR_UNSUPPORTED) != NULL &&
          strcmp(interop_result_name(MOQ_ERR_UNSUPPORTED),
                 "MOQ_ERR_UNSUPPORTED") == 0);
    CHECK(interop_result_name((moq_result_t)-1234) == NULL);

    /* A short buffer is truncated, terminated, and never overrun. */
    char small[24];
    memset(small, 'x', sizeof(small));
    interop_endpoint_error(small, 16, "endpoint", MOQ_ERR_UNSUPPORTED, 22);
    CHECK(strlen(small) == 15);
    CHECK(small[16] == 'x');
    CHECK(interop_endpoint_error(NULL, 0, "endpoint", MOQ_ERR_INVAL, 0) == NULL);

    if (g_failures) {
        fprintf(stderr, "test_endpoint_error: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("PASS: endpoint_error\n");
    return 0;
}
