#include "../../adapters/common/moq_adapter_binding.h"
#include "test_support.h"
#include <stdlib.h>

typedef struct copy_memory {
    bool fail_first;
    bool mismatch;
    size_t calls;
    size_t last_size;
    size_t allocated_bytes;
    size_t freed_bytes;
    size_t free_calls;
    void *live;
    size_t live_size;
} copy_memory_t;

static void *copy_alloc(size_t size, void *ctx)
{
    copy_memory_t *m = ctx;
    m->calls++;
    m->last_size = size;
    if (m->fail_first && m->calls == 1) return NULL;
    if (m->live || size == 0 || size > 1024) {
        m->mismatch = true;
        return NULL;
    }
    void *p = malloc(size);
    if (p) {
        m->live = p;
        m->live_size = size;
        m->allocated_bytes += size;
    }
    return p;
}

static void copy_free(void *ptr, size_t size, void *ctx)
{
    copy_memory_t *m = ctx;
    m->free_calls++;
    if (!ptr || ptr != m->live || size != m->live_size) {
        m->mismatch = true;
        return;
    }
    m->freed_bytes += size;
    free(ptr);
    m->live = NULL;
    m->live_size = 0;
}

static moq_alloc_t copy_allocator(copy_memory_t *m)
{
    return (moq_alloc_t){.ctx = m, .alloc = copy_alloc, .free = copy_free};
}

static bool balanced(const copy_memory_t *m)
{
    return !m->mismatch && !m->live && m->allocated_bytes == m->freed_bytes;
}

static bool empty_target(const moq_connection_target_t *out,
                         const uint8_t *bytes, size_t len)
{
    moq_connection_target_t expected;
    memset(&expected, 0, sizeof(expected));
    expected.struct_size = (uint32_t)sizeof(expected);
    return memcmp(out, &expected, sizeof(expected)) == 0 && !bytes && len == 0;
}

static void dirty_outputs(moq_connection_target_t *out, uint8_t **bytes,
                           size_t *len, uint8_t *sentinel)
{
    memset(out, 0xa5, sizeof(*out));
    *bytes = sentinel;
    *len = 123;
}

static uint32_t policy(void *ctx, uint32_t present, moq_bytes_t authority,
                        moq_bytes_t path_query)
{
    (void)ctx; (void)present; (void)authority; (void)path_query;
    return MOQ_URI_ACCEPT;
}

static int test_overflow_before_allocation(void)
{
    int failures = 0;
    uint8_t sentinel[2] = {0x52, 0x53};
    const size_t lengths[][2] = {{SIZE_MAX, 2}, {2, SIZE_MAX}};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        /* Even the old wrapped one-byte request is refused before any copy. */
        copy_memory_t memory = {.fail_first = true};
        moq_alloc_t alloc = copy_allocator(&memory);
        moq_connection_target_t in = {
            .struct_size = sizeof(in), .target_present = MOQ_BINDING_TARGET_COMPLETE,
            .authority = {sentinel, lengths[i][0]},
            .path_query = {sentinel, lengths[i][1]}, .accept_uri = policy,
            .accept_uri_ctx = &memory};
        moq_connection_target_t out;
        uint8_t *bytes;
        size_t len;
        dirty_outputs(&out, &bytes, &len, sentinel);
        moq_result_t rc = moq_adapter_target_copy(&alloc, &in, &out, &bytes, &len);
        if (rc != MOQ_ERR_INVAL || memory.calls != 0)
            fprintf(stderr, "overflow order %zu: result=%d alloc_calls=%zu last_size=%zu\n",
                    i, (int)rc, memory.calls, memory.last_size);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_ERR_INVAL);
        MOQ_TEST_CHECK_EQ_SIZE(memory.calls, 0);
        MOQ_TEST_CHECK(empty_target(&out, bytes, len));
        MOQ_TEST_CHECK_EQ_SIZE(memory.free_calls, 0);
        MOQ_TEST_CHECK(balanced(&memory));
        MOQ_TEST_CHECK(sentinel[0] == 0x52 && sentinel[1] == 0x53);
    }
    MOQ_TEST_PASS("adapter_target_copy.overflow_before_allocation_both_orders");
    return failures;
}

static int test_no_input_and_empty(void)
{
    int failures = 0;
    uint8_t sentinel = 0x52;
    copy_memory_t memory = {0};
    moq_alloc_t alloc = copy_allocator(&memory);
    moq_connection_target_t out;
    uint8_t *bytes;
    size_t len;
    dirty_outputs(&out, &bytes, &len, &sentinel);
    MOQ_TEST_CHECK_EQ_INT(moq_adapter_target_copy(&alloc, NULL, &out, &bytes, &len), MOQ_OK);
    MOQ_TEST_CHECK(empty_target(&out, bytes, len));
    for (size_t i = 0; i < 2; ++i) {
        moq_connection_target_t in = {
            .struct_size = sizeof(in), .target_present = MOQ_BINDING_TARGET_COMPLETE,
            .authority = {i ? &sentinel : NULL, 0},
            .path_query = {i ? &sentinel : NULL, 0},
            .accept_uri = policy, .accept_uri_ctx = &memory};
        dirty_outputs(&out, &bytes, &len, &sentinel);
        MOQ_TEST_CHECK_EQ_INT(moq_adapter_target_copy(&alloc, &in, &out, &bytes, &len), MOQ_OK);
        MOQ_TEST_CHECK(out.struct_size == sizeof(out) && out.target_present == in.target_present);
        MOQ_TEST_CHECK(!out.authority.data && !out.authority.len &&
                       !out.path_query.data && !out.path_query.len && !bytes && !len);
        MOQ_TEST_CHECK(out.accept_uri == policy && out.accept_uri_ctx == &memory);
    }
    MOQ_TEST_CHECK_EQ_SIZE(memory.calls, 0);
    MOQ_TEST_CHECK_EQ_SIZE(memory.free_calls, 0);
    MOQ_TEST_CHECK(balanced(&memory));
    MOQ_TEST_PASS("adapter_target_copy.no_input_empty_spans");
    return failures;
}

static int test_deep_copy_and_cleanup(void)
{
    int failures = 0;
    const size_t lengths[][2] = {{3, 2}, {3, 0}, {0, 2}};
    const uint32_t present[] = {MOQ_BINDING_TARGET_COMPLETE,
        MOQ_BINDING_AUTHORITY_PRESENT, MOQ_BINDING_PATH_PRESENT};
    const uint8_t authority_expected[] = {'h', 0, 'x'};
    const uint8_t path_expected[] = {'/', '?'};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        copy_memory_t memory = {0};
        moq_alloc_t alloc = copy_allocator(&memory);
        uint8_t authority[3], path[2];
        memcpy(authority, authority_expected, sizeof(authority));
        memcpy(path, path_expected, sizeof(path));
        moq_connection_target_t in = {
            .struct_size = (uint32_t)(sizeof(in) + 8), .target_present = present[i],
            .authority = {authority, lengths[i][0]}, .path_query = {path, lengths[i][1]},
            .accept_uri = policy, .accept_uri_ctx = &memory};
        moq_connection_target_t before;
        memcpy(&before, &in, sizeof(in));
        moq_connection_target_t out;
        uint8_t *bytes = NULL;
        size_t len = 0;
        moq_result_t rc = moq_adapter_target_copy(&alloc, &in, &out, &bytes, &len);
        MOQ_TEST_CHECK_EQ_INT(rc, MOQ_OK);
        MOQ_TEST_CHECK(memcmp(&before, &in, sizeof(in)) == 0);
        const size_t expected_len = lengths[i][0] + lengths[i][1];
        MOQ_TEST_CHECK_EQ_SIZE(memory.calls, 1);
        MOQ_TEST_CHECK_EQ_SIZE(memory.last_size, expected_len);
        MOQ_TEST_CHECK_EQ_SIZE(memory.allocated_bytes, expected_len);
        MOQ_TEST_CHECK(bytes && bytes == memory.live && len == expected_len);
        if (rc == MOQ_OK && bytes && len == expected_len) {
            MOQ_TEST_CHECK(out.struct_size == sizeof(out) && out.target_present == present[i]);
            MOQ_TEST_CHECK(out.authority.len == lengths[i][0] && out.path_query.len == lengths[i][1]);
            MOQ_TEST_CHECK(out.authority.data == (lengths[i][0] ? bytes : NULL));
            MOQ_TEST_CHECK(out.path_query.data == (lengths[i][1] ? bytes + lengths[i][0] : NULL));
            MOQ_TEST_CHECK(out.accept_uri == policy && out.accept_uri_ctx == &memory);
            memset(authority, 0x11, sizeof(authority));
            memset(path, 0x22, sizeof(path));
            memset(&in, 0, sizeof(in));
            if (lengths[i][0])
                MOQ_TEST_CHECK(memcmp(out.authority.data, authority_expected, lengths[i][0]) == 0);
            if (lengths[i][1])
                MOQ_TEST_CHECK(memcmp(out.path_query.data, path_expected, lengths[i][1]) == 0);
        }
        if (bytes) alloc.free(bytes, len, alloc.ctx);
        MOQ_TEST_CHECK_EQ_SIZE(memory.free_calls, 1);
        MOQ_TEST_CHECK_EQ_SIZE(memory.freed_bytes, expected_len);
        MOQ_TEST_CHECK(balanced(&memory));
    }
    MOQ_TEST_PASS("adapter_target_copy.deep_copy_exact_allocation_cleanup");
    return failures;
}

static int test_allocation_refusal_and_nonoverflow_boundary(void)
{
    int failures = 0;
    uint8_t sentinel[2] = {0x52, 0x53};
    const size_t lengths[][2] = {{2, 2}, {SIZE_MAX, 0}, {0, SIZE_MAX}};
    for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
        copy_memory_t memory = {.fail_first = true};
        moq_alloc_t alloc = copy_allocator(&memory);
        moq_connection_target_t in = {
            .struct_size = MOQ_CONNECTION_TARGET_V0_SIZE,
            .target_present = MOQ_BINDING_TARGET_COMPLETE,
            .authority = {sentinel, lengths[i][0]}, .path_query = {sentinel, lengths[i][1]},
            .accept_uri = policy, .accept_uri_ctx = &memory};
        moq_connection_target_t out;
        uint8_t *bytes;
        size_t len;
        dirty_outputs(&out, &bytes, &len, sentinel);
        MOQ_TEST_CHECK_EQ_INT(moq_adapter_target_copy(&alloc, &in, &out, &bytes, &len), MOQ_ERR_NOMEM);
        MOQ_TEST_CHECK(empty_target(&out, bytes, len));
        MOQ_TEST_CHECK_EQ_SIZE(memory.calls, 1);
        MOQ_TEST_CHECK_EQ_SIZE(memory.last_size, lengths[i][0] + lengths[i][1]);
        MOQ_TEST_CHECK_EQ_SIZE(memory.free_calls, 0);
        MOQ_TEST_CHECK(balanced(&memory));
    }
    MOQ_TEST_PASS("adapter_target_copy.allocation_refusal_nonoverflow_boundary");
    return failures;
}

static int test_existing_refusal_controls(void)
{
    int failures = 0;
    uint8_t sentinel = 0x52;
    for (size_t i = 0; i < 6; ++i) {
        copy_memory_t memory = {0};
        moq_alloc_t alloc = copy_allocator(&memory);
        moq_connection_target_t in = {.struct_size = sizeof(in),
            .target_present = MOQ_BINDING_TARGET_COMPLETE};
        switch (i) {
        case 0: in.struct_size = 0; break;
        case 1: in.struct_size = MOQ_CONNECTION_TARGET_V0_SIZE - 1; break;
        case 2: in.target_present |= UINT32_C(4); break;
        case 3: in.target_present = UINT32_C(0x80000000); break;
        case 4: in.authority.len = 1; break;
        case 5: in.path_query.len = 1; break;
        }
        moq_connection_target_t out;
        uint8_t *bytes;
        size_t len;
        dirty_outputs(&out, &bytes, &len, &sentinel);
        MOQ_TEST_CHECK_EQ_INT(moq_adapter_target_copy(&alloc, &in, &out, &bytes, &len), MOQ_ERR_INVAL);
        MOQ_TEST_CHECK(empty_target(&out, bytes, len));
        MOQ_TEST_CHECK_EQ_SIZE(memory.calls, 0);
        MOQ_TEST_CHECK_EQ_SIZE(memory.free_calls, 0);
        MOQ_TEST_CHECK(balanced(&memory));
    }
    MOQ_TEST_PASS("adapter_target_copy.existing_size_presence_pointer_refusal");
    return failures;
}

int main(void)
{
    int failures = test_overflow_before_allocation();
    failures += test_no_input_and_empty();
    failures += test_deep_copy_and_cleanup();
    failures += test_allocation_refusal_and_nonoverflow_boundary();
    failures += test_existing_refusal_controls();
    return failures ? 1 : 0;
}
