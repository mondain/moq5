/* The object-relay verifier accepts exactly the expected sequence and its
 * lawful end, and rejects every deviation the live case must catch: a changed
 * byte, a wrong length, a missing, duplicate, extra or reordered object, a
 * FIN before the group's tail, a missing or second done, and a done with
 * another status. Each mutation starts from the passing sequence. */
#include "object_relay.h"

#include <stdio.h>
#include <string.h>

static int g_failures;

#define CHECK(cond) do { \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        g_failures++; \
    } \
} while (0)

typedef enum {
    MUT_NONE,
    MUT_CORRUPT_BYTE,
    MUT_SHORT_PAYLOAD,
    MUT_DROP_OBJECT,
    MUT_DUPLICATE_OBJECT,
    MUT_EXTRA_OBJECT,
    MUT_REORDER,
    MUT_EARLY_FIN,
    MUT_NO_FIN,
    MUT_NO_DONE,
    MUT_WRONG_DONE_STATUS,
    MUT_FOREIGN_TRACK,
    MUT_NONZERO_STATUS,
    MUT_OBJECT_AFTER_DONE,
    MUT_COUNT
} mutation_t;

static uint8_t g_buf[ORL_MAX_PAYLOAD + 1];

static void feed(orl_verifier_t *v, uint64_t g, uint64_t o, mutation_t m)
{
    size_t len = orl_payload_len(g, o);
    orl_payload_fill(g, o, g_buf);
    if (m == MUT_CORRUPT_BYTE && g == 1 && o == 2) g_buf[len / 2] ^= 0x01;
    if (m == MUT_SHORT_PAYLOAD && g == 2 && o == 1) len--;
    orl_verifier_object(v, !(m == MUT_FOREIGN_TRACK && g == 0 && o == 0), g, 0,
                        o, (m == MUT_NONZERO_STATUS && g == 2 && o == 3) ? 3 : 0,
                        g_buf, len);
}

static bool run(mutation_t m, char *msg, size_t n)
{
    orl_verifier_t v;
    orl_verifier_init(&v);
    for (uint64_t g = 0; g < ORL_GROUPS; g++) {
        for (uint64_t o = 0; o < ORL_OBJECTS; o++) {
            if (m == MUT_DROP_OBJECT && g == 1 && o == 3) continue;
            if (m == MUT_REORDER && g == 0 && o == 1) {
                feed(&v, g, 2, m);
                feed(&v, g, 1, m);
                o = 2;
                continue;
            }
            if (m == MUT_EARLY_FIN && g == 2 && o == 2)
                orl_verifier_finished(&v, true, g, 0);
            feed(&v, g, o, m);
            if (m == MUT_DUPLICATE_OBJECT && g == 0 && o == 3) feed(&v, g, o, m);
        }
        if (m == MUT_EXTRA_OBJECT && g == 2) feed(&v, g, ORL_OBJECTS, m);
        if (!(m == MUT_NO_FIN && g == 1) && !(m == MUT_EARLY_FIN && g == 2))
            orl_verifier_finished(&v, true, g, 0);
    }
    if (m != MUT_NO_DONE)
        orl_verifier_done(&v, m == MUT_WRONG_DONE_STATUS ? 0x0 :
                              ORL_DONE_TRACK_ENDED);
    if (m == MUT_OBJECT_AFTER_DONE) {
        /* A late copy of an object after the done is not a lawful tail. */
        orl_verifier_object(&v, true, 0, 0, 0, 0, g_buf, 0);
    }
    return orl_verifier_pass(&v, msg, n);
}

int main(void)
{
    /* The sequence itself: distinct nonzero lengths within the bound, and
     * payloads that differ between objects of equal offset. */
    for (uint64_t a = 0; a < ORL_TOTAL; a++) {
        size_t la = orl_payload_len(a / ORL_OBJECTS, a % ORL_OBJECTS);
        CHECK(la > 0 && la <= ORL_MAX_PAYLOAD);
        for (uint64_t b = a + 1; b < ORL_TOTAL; b++)
            CHECK(la != orl_payload_len(b / ORL_OBJECTS, b % ORL_OBJECTS));
    }
    CHECK(orl_payload_byte(0, 0, 0) != orl_payload_byte(0, 1, 0));
    CHECK(orl_payload_byte(0, 0, 0) != orl_payload_byte(1, 0, 0));

    char msg[256];
    CHECK(run(MUT_NONE, msg, sizeof(msg)));
    printf("baseline: %s\n", msg);
    static const char *const names[MUT_COUNT] = {
        "none", "corrupt-byte", "short-payload", "drop-object",
        "duplicate-object", "extra-object", "reorder", "early-fin", "no-fin",
        "no-done", "wrong-done-status", "foreign-track", "nonzero-status",
        "object-after-done",
    };
    for (int m = MUT_CORRUPT_BYTE; m < MUT_COUNT; m++) {
        bool passed = run((mutation_t)m, msg, sizeof(msg));
        CHECK(!passed);
        printf("%-18s rejected=%s: %s\n", names[m], passed ? "no" : "yes", msg);
    }
    if (g_failures) {
        fprintf(stderr, "test_object_relay_verify: %d failure(s)\n", g_failures);
        return 1;
    }
    printf("PASS: object_relay_verify\n");
    return 0;
}
