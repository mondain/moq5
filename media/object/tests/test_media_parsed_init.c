#include "media_object_layout_probe.h"
#include "../../../tests/unit/test_support.h"
#include <stdlib.h>
#include <string.h>

#if defined(moq_media_parsed_object_init) || defined(moq_media_parsed_object_init_sized)
#error "Parsed-object initializers must remain addressable functions"
#endif

static void (*const legacy_init)(moq_media_parsed_object_t *) =
    &moq_media_parsed_object_init;
static void (*const sized_init)(moq_media_parsed_object_t *, size_t) =
    &moq_media_parsed_object_init_sized;

enum { GUARD_SIZE = 32, FUTURE_SIZE = 16, POISON = 0xa5 };
MEDIA_ASSERT(GUARD_SIZE % MEDIA_ALIGN(moq_media_parsed_object_t) == 0,
             "guarded allocation keeps parsed-object alignment");

static void call_sized(moq_media_parsed_object_t *out, size_t size, bool indirect)
{
    if (indirect) sized_init(out, size);
    else moq_media_parsed_object_init_sized(out, size);
}

static void call_legacy(moq_media_parsed_object_t *out, bool indirect)
{
    if (indirect) legacy_init(out);
    else moq_media_parsed_object_init(out);
}

/* Build expected bytes from the contract, never from an initializer result. */
static void expected_sized(unsigned char *bytes, size_t size)
{
    const size_t current_size = sizeof(moq_media_parsed_object_t);
    const size_t written = size < current_size ? size : current_size;
    if (written < sizeof(uint32_t)) return;
    memset(bytes, 0, written);
    const uint32_t stamp = (uint32_t)written;
    memcpy(bytes, &stamp, sizeof(stamp));

    const moq_sap_type_t sap = MOQ_SAP_UNKNOWN;
    const size_t sap_offset = offsetof(media_old_parsed_t, sap_type);
    if (written >= sap_offset + sizeof(sap))
        memcpy(bytes + sap_offset, &sap, sizeof(sap));

    if (written >= sizeof(media_old_parsed_t) + sizeof(moq_media_metadata_t)) {
        moq_media_metadata_t empty;
        memset(&empty, 0, sizeof(empty));
        empty.struct_size = (uint32_t)sizeof(empty);
        empty.loc.struct_size = (uint32_t)sizeof(empty.loc);
        memcpy(bytes + sizeof(media_old_parsed_t), &empty, sizeof(empty));
    }
}

static int sized_case(size_t size, bool indirect)
{
    int failures = 0;
    unsigned char expected[GUARD_SIZE + sizeof(moq_media_parsed_object_t) +
                           FUTURE_SIZE + GUARD_SIZE];
    unsigned char *guarded = (unsigned char *)malloc(sizeof(expected));
    const size_t extent = sizeof(moq_media_parsed_object_t) + FUTURE_SIZE;
    const size_t exact_size = size < extent ? (size ? size : 1) : extent;
    unsigned char *exact = (unsigned char *)malloc(exact_size);
    MOQ_TEST_CHECK(guarded != NULL && exact != NULL);
    if (!guarded || !exact) {
        free(guarded);
        free(exact);
        return failures;
    }

    memset(expected, POISON, sizeof(expected));
    expected_sized(expected + GUARD_SIZE, size);
    memset(guarded, POISON, sizeof(expected));
    memset(exact, POISON, exact_size);
    call_sized((moq_media_parsed_object_t *)(guarded + GUARD_SIZE), size, indirect);
    /* Exact allocations give ASan a boundary at every caller prefix. */
    call_sized((moq_media_parsed_object_t *)exact, size, indirect);
    MOQ_TEST_CHECK(memcmp(guarded, expected, sizeof(expected)) == 0);
    MOQ_TEST_CHECK(memcmp(exact, expected + GUARD_SIZE, exact_size) == 0);
    if (failures)
        fprintf(stderr, "sized init: size=%zu indirect=%d\n", size, indirect);
    free(guarded);
    free(exact);
    return failures;
}

static int all_sizes(void)
{
    int failures = 0;
    for (size_t size = 0; size <= sizeof(moq_media_parsed_object_t) + FUTURE_SIZE;
         ++size) {
        for (int indirect = 0; indirect < 2; ++indirect) {
            call_sized(NULL, size, indirect != 0);
            failures += sized_case(size, indirect != 0);
        }
    }
    call_sized(NULL, SIZE_MAX, false);
    call_sized(NULL, SIZE_MAX, true);
    failures += sized_case(SIZE_MAX, false);
    failures += sized_case(SIZE_MAX, true);
    MOQ_TEST_PASS("sized init: NULL, every prefix, exact allocations and guards");
    return failures;
}

static int legacy_allocations(void)
{
    int failures = 0;
    /* This original declaration includes the old tail padding independently
     * of the current type, floor macro, and either initializer's stamp. */
    media_old_parsed_t old_expected;
    memset(&old_expected, 0, sizeof(old_expected));
    old_expected.struct_size = (uint32_t)sizeof(old_expected);
    old_expected.sap_type = MOQ_SAP_UNKNOWN;

    for (int indirect = 0; indirect < 2; ++indirect) {
        unsigned char expected[GUARD_SIZE + sizeof(moq_media_parsed_object_t) +
                               FUTURE_SIZE + GUARD_SIZE];
        unsigned char *guarded = (unsigned char *)malloc(sizeof(expected));
        media_old_parsed_t *exact = (media_old_parsed_t *)malloc(sizeof(*exact));
        MOQ_TEST_CHECK(guarded != NULL && exact != NULL);
        if (!guarded || !exact) {
            free(guarded);
            free(exact);
            continue;
        }

        memset(expected, POISON, sizeof(expected));
        memcpy(expected + GUARD_SIZE, &old_expected, sizeof(old_expected));
        memset(guarded, POISON, sizeof(expected));
        memset(exact, POISON, sizeof(*exact));
        call_legacy(NULL, indirect != 0);
        call_legacy((moq_media_parsed_object_t *)exact, indirect != 0);
        call_legacy((moq_media_parsed_object_t *)(guarded + GUARD_SIZE), indirect != 0);
        MOQ_TEST_CHECK(memcmp(exact, &old_expected, sizeof(old_expected)) == 0);
        MOQ_TEST_CHECK(memcmp(guarded, expected, sizeof(expected)) == 0);
        free(guarded);
        free(exact);
    }
    MOQ_TEST_PASS("legacy init: complete original extent and function pointer");
    return failures;
}

static int complete_metadata(void)
{
    int failures = 0;
    struct future_parsed {
        moq_media_parsed_object_t current;
        unsigned char tail[FUTURE_SIZE];
    } future;
    moq_media_metadata_t empty;
    memset(&empty, 0, sizeof(empty));
    empty.struct_size = (uint32_t)sizeof(empty);
    empty.loc.struct_size = (uint32_t)sizeof(empty.loc);

    for (int indirect = 0; indirect < 2; ++indirect) {
        memset(&future, POISON, sizeof(future));
        call_sized(&future.current, sizeof(future), indirect != 0);
        MOQ_TEST_CHECK(future.current.struct_size == sizeof(future.current));
        MOQ_TEST_CHECK(future.current.sap_type == MOQ_SAP_UNKNOWN);
        MOQ_TEST_CHECK(memcmp(&future.current.metadata, &empty, sizeof(empty)) == 0);
        for (size_t i = 0; i < sizeof(future.tail); ++i)
            MOQ_TEST_CHECK(future.tail[i] == POISON);

        memset(&future.current, POISON, sizeof(future.current));
        call_sized(&future.current, sizeof(future.current), indirect != 0);
        MOQ_TEST_CHECK(future.current.struct_size == sizeof(future.current));
        MOQ_TEST_CHECK(memcmp(&future.current.metadata, &empty, sizeof(empty)) == 0);
    }
    MOQ_TEST_PASS("current/future init: zero metadata, nested stamps, untouched tail");
    return failures;
}

int main(void)
{
    int failures = 0;
    failures += all_sizes();
    failures += legacy_allocations();
    failures += complete_metadata();
    printf("parsed init: ptr=%zu enum=%zu old=%zu current=%zu, %d failed checks\n",
           sizeof(void *), sizeof(moq_media_packaging_t), sizeof(media_old_parsed_t),
           sizeof(moq_media_parsed_object_t), failures);
    return failures != 0;
}
