/*
 * The functions moq-mpeg2ts keeps out of its public API (mpeg2ts_internal.h):
 * CRC_32, CA_descriptor collection and PAT authoring. Built with the
 * library's source, because a shared build does not export them.
 */

#include <moq/mpeg2ts.h>

#include <stdio.h>
#include <string.h>

#include "../src/mpeg2ts_internal.h"
#include "ts_builders.h"

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

static moq_bytes_t B(const uint8_t *d, size_t n)
{
    moq_bytes_t b = { d, n };
    return b;
}

static void test_crc32(void)
{
    const uint8_t check[] = "123456789";
    CHECK(moq_mpeg2ts_crc32(check, 9) == 0x0376E6E7u);
    CHECK(moq_mpeg2ts_crc32(check, 9) == tb_crc32(check, 9));

    /* The PAT section of the draft's initialization-data example. */
    static const uint8_t pat[] = {
        0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00, 0x00,
        0x00, 0x01, 0xE1, 0x00, 0xE8, 0xF9, 0x5E, 0x7D,
    };
    CHECK(moq_mpeg2ts_crc32(pat, 12) == 0xE8F95E7Du);
    CHECK(moq_mpeg2ts_crc32(pat, 16) == 0);
    CHECK(moq_mpeg2ts_crc32(NULL, 0) == 0xFFFFFFFFu);
}

static void test_ca_pids(void)
{
    const uint8_t ca[] = { 0x09, 0x04, 0x0B, 0x00, 0xE5, 0x00 }; /* CA_PID 0x500 */
    const uint8_t lang[] = { 0x0A, 0x04, 'e', 'n', 'g', 0x00 };
    uint16_t pids[2];
    size_t n = 0;
    CHECK(moq_mpeg2ts_ca_pids(B(ca, sizeof(ca)), pids, 2, &n) == MOQ_OK);
    CHECK(n == 1 && pids[0] == 0x0500);
    CHECK(moq_mpeg2ts_ca_pids(B(lang, sizeof(lang)), pids, 2, &n) == MOQ_OK);
    CHECK(n == 0);
    const uint8_t bad_ca[] = { 0x09, 0x02, 0x0B, 0x00 };
    CHECK(moq_mpeg2ts_ca_pids(B(bad_ca, 4), pids, 2, &n) == MOQ_ERR_PROTO);
    const uint8_t over[] = { 0x0A, 0x09, 0x00 };
    CHECK(moq_mpeg2ts_ca_pids(B(over, 3), pids, 2, &n) == MOQ_ERR_PROTO);
    const uint8_t two_ca[] = { 0x09, 0x04, 0, 1, 0xE0, 0x10,
                               0x09, 0x04, 0, 2, 0xE0, 0x11 };
    CHECK(moq_mpeg2ts_ca_pids(B(two_ca, 12), pids, 1, &n) == MOQ_ERR_BUFFER);
    CHECK(n == 2);
}

static void test_build_pat(void)
{
    moq_mpeg2ts_program_t in[] = { { 7, 0x0700 } };
    uint8_t sec[64];
    size_t n = 0;
    CHECK(moq_mpeg2ts_build_pat(0x00AB, 9, in, 1, sec, sizeof(sec), &n) == MOQ_OK);
    CHECK(n == 16);
    CHECK(moq_mpeg2ts_crc32(sec, n) == 0);
    uint8_t ref[64];
    tb_prog_t tp[] = { { 7, 0x0700 } };
    size_t rn = tb_pat(ref, 0x00AB, 9, tp, 1);
    CHECK(rn == n && memcmp(ref, sec, n) == 0);

    CHECK(moq_mpeg2ts_build_pat(1, 0, in, 1, sec, 8, &n) == MOQ_ERR_BUFFER);
    CHECK(n == 16);
    moq_mpeg2ts_program_t zero[] = { { 0, 0x0010 } };
    CHECK(moq_mpeg2ts_build_pat(1, 0, zero, 1, sec, sizeof(sec), &n) == MOQ_ERR_INVAL);
    moq_mpeg2ts_program_t badpid[] = { { 1, 0x2000 } };
    CHECK(moq_mpeg2ts_build_pat(1, 0, badpid, 1, sec, sizeof(sec), &n) == MOQ_ERR_INVAL);
    CHECK(moq_mpeg2ts_build_pat(1, 32, in, 1, sec, sizeof(sec), &n) == MOQ_ERR_INVAL);
}

int main(void)
{
    test_crc32();
    test_ca_pids();
    test_build_pat();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("mpeg2ts internal: all tests passed\n");
    return 0;
}
