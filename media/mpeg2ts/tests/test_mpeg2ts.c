/*
 * Unit tests for moq-mpeg2ts against hand-built TS vectors (ts_builders.h)
 * and the PAT carried by the initialization-data example of
 * draft-gregoire-moq-msfts.
 */

#include <moq/mpeg2ts.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

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

/* The "Per-Program Track with Initialization Data" example's data value,
 * decoded: one PAT packet, program 1 on PMT PID 0x0100, transport_stream_id 1. */
static const uint8_t SPEC_PAT_PREFIX[] = {
    0x47, 0x40, 0x00, 0x10, 0x00, 0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00,
    0x00, 0x00, 0x01, 0xE1, 0x00, 0xE8, 0xF9, 0x5E, 0x7D, 0xFF, 0xFF, 0xFF,
};

static void spec_pat_packet(uint8_t pkt[188])
{
    memset(pkt, 0xFF, 188);
    memcpy(pkt, SPEC_PAT_PREFIX, sizeof(SPEC_PAT_PREFIX));
}

/* -- Packet parse ------------------------------------------------------- */

static void test_parse_packet(void)
{
    uint8_t pkt[192];
    moq_mpeg2ts_packet_t p;

    /* Plain payload packet. */
    {
        const uint8_t pl[] = { 1, 2, 3 };
        tb_pkt_t s = { .pid = 0x0101, .pusi = true, .cc = 7,
                       .payload = pl, .payload_len = 3 };
        tb_packet(pkt, &s);
        moq_mpeg2ts_packet_init(&p);
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p) == MOQ_OK);
        CHECK(p.pid == 0x0101);
        CHECK(p.payload_unit_start);
        CHECK(p.continuity_counter == 7);
        CHECK(!p.has_adaptation_field);
        CHECK(!p.has_prefix);
        CHECK(p.payload.len == 184);
        CHECK(p.payload.data == pkt + 4);
        CHECK(p.payload.data[0] == 1);
    }

    /* Adaptation field with PCR at the top of its range, RAI and DI. */
    {
        tb_pkt_t s = { .pid = 0x0100, .cc = 3, .rai = true, .di = true,
                       .pcr = true, .pcr_base = 0x1FFFFFFFFull,
                       .pcr_ext = 299 };
        tb_packet(pkt, &s);
        moq_mpeg2ts_packet_init(&p);
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p) == MOQ_OK);
        CHECK(p.has_adaptation_field);
        CHECK(p.random_access);
        CHECK(p.discontinuity);
        CHECK(p.has_pcr);
        CHECK(p.pcr_base == 0x1FFFFFFFFull);
        CHECK(p.pcr_ext == 299);
        CHECK(moq_mpeg2ts_pcr_27mhz(p.pcr_base, p.pcr_ext) ==
              0x1FFFFFFFFull * 300u + 299u);
        CHECK(p.payload.len == 188 - 4 - 8);
    }

    /* 192-octet source packet: prefix then the TS packet. */
    {
        tb_pkt_t s = { .pid = 0x0011, .prefix = true,
                       .prefix_value = 0xC0000123u };
        tb_packet(pkt, &s);
        moq_mpeg2ts_packet_init(&p);
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 192), 192, &p) == MOQ_OK);
        CHECK(p.has_prefix);
        CHECK(p.prefix == 0xC0000123u);
        CHECK(p.pid == 0x0011);
        CHECK(moq_mpeg2ts_prefix_arrival_time(p.prefix) == 0x123u);
        CHECK(moq_mpeg2ts_prefix_copy_permission(p.prefix) == 3);
    }

    /* Adaptation-only packet: adaptation_field_length must be 183. */
    {
        tb_pkt_t s = { .pid = 0x0100, .no_payload = true };
        tb_packet(pkt, &s);
        moq_mpeg2ts_packet_init(&p);
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p) == MOQ_OK);
        CHECK(p.payload.len == 0);
        pkt[4] = 182;
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p) == MOQ_ERR_PROTO);
    }

    /* Negative cases. */
    {
        tb_pkt_t s = { .pid = 0x0100 };
        tb_packet(pkt, &s);
        pkt[0] = 0x48;
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p) == MOQ_ERR_PROTO);

        tb_packet(pkt, &s);
        pkt[3] = (uint8_t)(pkt[3] & 0xCF);         /* afc = 0, reserved */
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p) == MOQ_ERR_PROTO);

        tb_pkt_t a = { .pid = 0x0100, .af = true };
        tb_packet(pkt, &a);
        pkt[4] = 184;                               /* overruns the packet */
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p) == MOQ_ERR_PROTO);
        pkt[4] = 183;                               /* afc 3 allows <= 182 */
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p) == MOQ_ERR_PROTO);

        tb_pkt_t c = { .pid = 0x0100, .pcr = true };
        tb_packet(pkt, &c);
        pkt[4] = 3;                                 /* PCR flag, no room */
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p) == MOQ_ERR_PROTO);

        tb_packet(pkt, &s);
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 187), 188, &p) == MOQ_ERR_INVAL);
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 190, &p) == MOQ_ERR_INVAL);
        CHECK(moq_mpeg2ts_parse_packet(B(NULL, 188), 188, &p) == MOQ_ERR_INVAL);
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, NULL) == MOQ_ERR_INVAL);

        /* 192 with 0x47 at offset 0 rather than 4. */
        tb_pkt_t q = { .pid = 0x0100 };
        tb_packet(pkt, &q);
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 192), 192, &p) == MOQ_ERR_PROTO);
    }
}

static void test_clock_helpers(void)
{
    CHECK(moq_mpeg2ts_pcr_delta(100, 250) == 150);
    /* A PCR base wrap is a forward step, not a discontinuity. */
    CHECK(moq_mpeg2ts_pcr_delta(MOQ_MPEG2TS_PCR_WRAP - 100, 50) == 150);
    CHECK(moq_mpeg2ts_pts_delta(MOQ_MPEG2TS_PTS_WRAP - 3000, 3000) == 6000);
    CHECK(moq_mpeg2ts_pts_delta(0, 0) == 0);
}

/* -- PES ---------------------------------------------------------------- */

static void test_pes(void)
{
    uint8_t b[64];
    moq_mpeg2ts_pes_header_t h;

    size_t n = tb_pes(b, 0xE0, true, 0x1FFFFFFFFull, true, 0x123456789ull);
    CHECK(moq_mpeg2ts_parse_pes_header(B(b, n), &h) == MOQ_OK);
    CHECK(h.stream_id == 0xE0);
    CHECK(h.has_pts && h.pts == 0x1FFFFFFFFull);
    CHECK(h.has_dts && h.dts == 0x123456789ull);

    n = tb_pes(b, 0xC0, true, 90000, false, 0);
    CHECK(moq_mpeg2ts_parse_pes_header(B(b, n), &h) == MOQ_OK);
    CHECK(h.has_pts && h.pts == 90000 && !h.has_dts);

    /* padding_stream carries no optional header. */
    const uint8_t pad[] = { 0, 0, 1, 0xBE, 0, 4, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(moq_mpeg2ts_parse_pes_header(B(pad, sizeof(pad)), &h) == MOQ_OK);
    CHECK(!h.has_pts && !h.has_dts);

    /* Negative: no start code, marker bit, PTS_DTS_flags 01, overrun. */
    n = tb_pes(b, 0xE0, true, 1000, false, 0);
    b[2] = 0x02;
    CHECK(moq_mpeg2ts_parse_pes_header(B(b, n), &h) == MOQ_ERR_PROTO);
    n = tb_pes(b, 0xE0, true, 1000, false, 0);
    b[9 + 4] &= 0xFE;
    CHECK(moq_mpeg2ts_parse_pes_header(B(b, n), &h) == MOQ_ERR_PROTO);
    n = tb_pes(b, 0xE0, true, 1000, false, 0);
    b[7] = 0x40;
    CHECK(moq_mpeg2ts_parse_pes_header(B(b, n), &h) == MOQ_ERR_PROTO);
    n = tb_pes(b, 0xE0, true, 1000, true, 900);
    CHECK(moq_mpeg2ts_parse_pes_header(B(b, 12), &h) == MOQ_ERR_PROTO);
    b[8] = 200;
    CHECK(moq_mpeg2ts_parse_pes_header(B(b, n), &h) == MOQ_ERR_PROTO);
    CHECK(moq_mpeg2ts_parse_pes_header(B(b, 5), &h) == MOQ_ERR_PROTO);
    CHECK(moq_mpeg2ts_parse_pes_header(B(b, n), NULL) == MOQ_ERR_INVAL);
}

/* -- PSI ---------------------------------------------------------------- */

static void test_pat(void)
{
    uint8_t pkt[188];
    spec_pat_packet(pkt);

    moq_mpeg2ts_program_t progs[4];
    moq_mpeg2ts_pat_t pat;
    moq_mpeg2ts_pat_init(&pat, progs, 4);
    CHECK(moq_mpeg2ts_parse_pat(B(pkt + 5, 16), &pat) == MOQ_OK);
    CHECK(pat.transport_stream_id == 1);
    CHECK(pat.version == 0 && pat.current_next);
    CHECK(pat.program_count == 1);
    CHECK(progs[0].program_number == 1 && progs[0].pmt_pid == 0x0100);
    CHECK(!pat.has_network_pid);

    /* Network PID entry and several programs. */
    uint8_t sec[256];
    tb_prog_t tp[] = { { 0, 0x0010 }, { 1, 0x0100 }, { 2, 0x0200 },
                       { 3, 0x0300 } };
    size_t n = tb_pat(sec, 0x1234, 5, tp, 4);
    moq_mpeg2ts_pat_init(&pat, progs, 4);
    CHECK(moq_mpeg2ts_parse_pat(B(sec, n), &pat) == MOQ_OK);
    CHECK(pat.has_network_pid && pat.network_pid == 0x0010);
    CHECK(pat.program_count == 3);
    CHECK(pat.version == 5);
    CHECK(progs[2].program_number == 3 && progs[2].pmt_pid == 0x0300);

    /* BUFFER: required count reported. */
    moq_mpeg2ts_pat_init(&pat, progs, 2);
    CHECK(moq_mpeg2ts_parse_pat(B(sec, n), &pat) == MOQ_ERR_BUFFER);
    CHECK(pat.program_count == 3);

    /* Negative: CRC, table_id, length disagreement, ragged loop. */
    moq_mpeg2ts_pat_init(&pat, progs, 4);
    sec[n - 1] ^= 1;
    CHECK(moq_mpeg2ts_parse_pat(B(sec, n), &pat) == MOQ_ERR_PROTO);
    n = tb_pat(sec, 0x1234, 5, tp, 4);
    sec[0] = 0x02;
    CHECK(moq_mpeg2ts_parse_pat(B(sec, n), &pat) == MOQ_ERR_PROTO);
    n = tb_pat(sec, 0x1234, 5, tp, 4);
    CHECK(moq_mpeg2ts_parse_pat(B(sec, n - 1), &pat) == MOQ_ERR_PROTO);
    /* section_length one octet longer than a whole entry loop, CRC fixed. */
    n = tb_pat(sec, 0x1234, 5, tp, 1);
    sec[n - 4] = 0xAA;
    n = tb_finish_section(sec, n - 4 + 1);
    CHECK(moq_mpeg2ts_parse_pat(B(sec, n), &pat) == MOQ_ERR_PROTO);
    /* section_syntax_indicator 0. */
    n = tb_pat(sec, 0x1234, 5, tp, 2);
    sec[1] &= 0x7F;
    n = tb_finish_section(sec, n - 4);
    CHECK(moq_mpeg2ts_parse_pat(B(sec, n), &pat) == MOQ_ERR_PROTO);
    CHECK(moq_mpeg2ts_parse_pat(B(NULL, 0), &pat) == MOQ_ERR_INVAL);
}

static void test_pmt(void)
{
    uint8_t sec[1100];
    const uint8_t ca[] = { 0x09, 0x04, 0x0B, 0x00, 0xE5, 0x00 }; /* CA_PID 0x500 */
    const uint8_t lang[] = { 0x0A, 0x04, 'e', 'n', 'g', 0x00 };
    tb_es_t es[] = {
        { 0x1B, 0x0101, NULL, 0 },
        { 0x0F, 0x0102, lang, sizeof(lang) },
        { 0x86, 0x0103, NULL, 0 },
    };
    size_t n = tb_pmt(sec, 1, 3, 0x0101, ca, sizeof(ca), es, 3);

    moq_mpeg2ts_stream_t st[4];
    moq_mpeg2ts_pmt_t pmt;
    moq_mpeg2ts_pmt_init(&pmt, st, 4);
    CHECK(moq_mpeg2ts_parse_pmt(B(sec, n), &pmt) == MOQ_OK);
    CHECK(pmt.program_number == 1);
    CHECK(pmt.version == 3);
    CHECK(pmt.pcr_pid == 0x0101);
    CHECK(pmt.stream_count == 3);
    CHECK(st[0].stream_type == 0x1B && st[0].pid == 0x0101);
    CHECK(st[1].stream_type == 0x0F && st[1].pid == 0x0102);
    CHECK(st[1].descriptors.len == sizeof(lang));
    CHECK(st[2].stream_type == 0x86 && st[2].pid == 0x0103);
    CHECK(pmt.program_descriptors.len == sizeof(ca));


    /* BUFFER. */
    moq_mpeg2ts_pmt_init(&pmt, st, 2);
    CHECK(moq_mpeg2ts_parse_pmt(B(sec, n), &pmt) == MOQ_ERR_BUFFER);
    CHECK(pmt.stream_count == 3);

    /* Negative: ES_info_length overruns, program_info overruns, CRC. */
    moq_mpeg2ts_pmt_init(&pmt, st, 4);
    {
        uint8_t bad[1100];
        memcpy(bad, sec, n);
        /* es[1] ES_info_length: header 12 + pdesc 6 + es0 5 = 23; es1 at 23,
         * its length low octet at 23 + 4. */
        bad[23 + 4] = 0xF0;
        size_t bn = tb_finish_section(bad, n - 4);
        CHECK(moq_mpeg2ts_parse_pmt(B(bad, bn), &pmt) == MOQ_ERR_PROTO);

        memcpy(bad, sec, n);
        bad[11] = 0xFF;
        bn = tb_finish_section(bad, n - 4);
        CHECK(moq_mpeg2ts_parse_pmt(B(bad, bn), &pmt) == MOQ_ERR_PROTO);

        memcpy(bad, sec, n);
        bad[5] ^= 0x02;
        CHECK(moq_mpeg2ts_parse_pmt(B(bad, n), &pmt) == MOQ_ERR_PROTO);

        memcpy(bad, sec, n);
        bad[0] = 0x00;
        bn = tb_finish_section(bad, n - 4);
        CHECK(moq_mpeg2ts_parse_pmt(B(bad, bn), &pmt) == MOQ_ERR_PROTO);
    }
}

static void test_section_buf_and_packetize(void)
{
    uint8_t storage[MOQ_MPEG2TS_SECTION_MAX];
    moq_mpeg2ts_section_buf_t sb;
    CHECK(moq_mpeg2ts_section_buf_init(&sb, sizeof(sb) - 1, storage,
                                       sizeof(storage)) == MOQ_ERR_INVAL);
    CHECK(moq_mpeg2ts_section_buf_init(&sb, sizeof(sb), storage, 2) ==
          MOQ_ERR_INVAL);
    CHECK(moq_mpeg2ts_section_buf_init(&sb, sizeof(sb), storage,
                                       sizeof(storage)) == MOQ_OK);

    /* A PMT long enough to span three packets. */
    uint8_t big_desc[400];
    for (size_t i = 0; i < sizeof(big_desc); i += 2) {
        big_desc[i] = 0x80;              /* user-private descriptor tag */
        big_desc[i + 1] = 0;
    }
    tb_es_t es[] = { { 0x1B, 0x0101, big_desc, sizeof(big_desc) } };
    uint8_t sec[1100];
    size_t n = tb_pmt(sec, 1, 0, 0x0101, NULL, 0, es, 1);
    CHECK(n > 2 * 183);

    uint8_t pkts[188 * 4];
    size_t pk_len = 0;
    uint8_t cc = 14;
    CHECK(moq_mpeg2ts_packetize_section(0x0100, &cc, B(sec, n), 188, pkts,
                                        sizeof(pkts), &pk_len) == MOQ_OK);
    CHECK(pk_len == 188 * 3);
    CHECK(cc == 1);                       /* 14, 15, 0 -> next is 1 */

    moq_bytes_t out = { NULL, 0 };
    bool complete = false;
    for (size_t i = 0; i < 3; i++) {
        moq_mpeg2ts_packet_t p;
        moq_mpeg2ts_packet_init(&p);
        CHECK(moq_mpeg2ts_parse_packet(B(pkts + i * 188, 188), 188, &p) == MOQ_OK);
        CHECK(p.pid == 0x0100);
        CHECK(p.payload_unit_start == (i == 0));
        CHECK(moq_mpeg2ts_section_buf_push(&sb, &p) == MOQ_OK);
        complete = moq_mpeg2ts_section_buf_next(&sb, &out) == MOQ_OK;
        CHECK(complete == (i == 2));
        if (complete) CHECK(out.len == n && memcmp(out.data, sec, n) == 0);
        moq_bytes_t none;
        CHECK(moq_mpeg2ts_section_buf_next(&sb, &none) == MOQ_DONE);
    }
    CHECK(out.len == n && memcmp(out.data, sec, n) == 0);

    moq_mpeg2ts_stream_t st[2];
    moq_mpeg2ts_pmt_t pmt;
    moq_mpeg2ts_pmt_init(&pmt, st, 2);
    CHECK(moq_mpeg2ts_parse_pmt(out, &pmt) == MOQ_OK);
    CHECK(pmt.stream_count == 1 && st[0].descriptors.len == sizeof(big_desc));

    /* A continuity gap abandons the partial section. */
    moq_mpeg2ts_section_buf_reset(&sb);
    {
        moq_mpeg2ts_packet_t p;
        moq_mpeg2ts_packet_init(&p);
        moq_mpeg2ts_parse_packet(B(pkts, 188), 188, &p);
        moq_mpeg2ts_section_buf_push(&sb, &p);
        CHECK(moq_mpeg2ts_section_buf_next(&sb, &out) == MOQ_DONE);
        moq_mpeg2ts_parse_packet(B(pkts + 376, 188), 188, &p);
        CHECK(moq_mpeg2ts_section_buf_push(&sb, &p) == MOQ_OK);
        CHECK(moq_mpeg2ts_section_buf_next(&sb, &out) == MOQ_DONE);
        CHECK(!sb._s._active);
    }

    /* pointer_field that runs past the payload. */
    {
        uint8_t pkt[188];
        spec_pat_packet(pkt);
        pkt[4] = 184;
        moq_mpeg2ts_packet_t p;
        moq_mpeg2ts_packet_init(&p);
        CHECK(moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p) == MOQ_OK);
        CHECK(moq_mpeg2ts_section_buf_push(&sb, &p) == MOQ_ERR_PROTO);
        CHECK(moq_mpeg2ts_section_buf_next(&sb, &out) == MOQ_DONE);
    }

    /* section_length above 1021 is abandoned, never collected. */
    {
        uint8_t pkt[188];
        spec_pat_packet(pkt);
        pkt[6] = 0xB3;                    /* section_length 0x3FF */
        pkt[7] = 0xFF;
        moq_mpeg2ts_packet_t p;
        moq_mpeg2ts_packet_init(&p);
        moq_mpeg2ts_parse_packet(B(pkt, 188), 188, &p);
        CHECK(moq_mpeg2ts_section_buf_push(&sb, &p) == MOQ_OK);
        CHECK(moq_mpeg2ts_section_buf_next(&sb, &out) == MOQ_DONE && !sb._s._active);
    }

    /* packetize_section BUFFER contract. */
    cc = 0;
    CHECK(moq_mpeg2ts_packetize_section(0x0100, &cc, B(sec, n), 188, pkts,
                                        188, &pk_len) == MOQ_ERR_BUFFER);
    CHECK(pk_len == 188 * 3);
    /* 192: zero prefix. */
    uint8_t pk192[192 * 3];
    cc = 0;
    CHECK(moq_mpeg2ts_packetize_section(0x0100, &cc, B(sec, n), 192, pk192,
                                        sizeof(pk192), &pk_len) == MOQ_OK);
    CHECK(pk_len == 192 * 3 && pk192[4] == 0x47 && pk192[0] == 0);
}

static void test_pmt_subset(void)
{
    /* PMT subset: keep video and one audio of three streams. */
    uint8_t pmt_sec[256];
    const uint8_t lang[] = { 0x0A, 0x04, 's', 'p', 'a', 0x00 };
    tb_es_t es[] = { { 0x1B, 0x0101, NULL, 0 },
                     { 0x0F, 0x0102, NULL, 0 },
                     { 0x0F, 0x0103, lang, sizeof(lang) } };
    size_t pn = tb_pmt(pmt_sec, 1, 31, 0x0101, NULL, 0, es, 3);
    uint16_t keep[] = { 0x0101, 0x0103 };
    uint8_t out[256];
    size_t on = 0;
    CHECK(moq_mpeg2ts_pmt_subset(B(pmt_sec, pn), keep, 2, out, sizeof(out), &on) == MOQ_OK);
    CHECK(tb_crc32(out, on) == 0);
    moq_mpeg2ts_stream_t st[4];
    moq_mpeg2ts_pmt_t pmt;
    moq_mpeg2ts_pmt_init(&pmt, st, 4);
    CHECK(moq_mpeg2ts_parse_pmt(B(out, on), &pmt) == MOQ_OK);
    CHECK(pmt.version == 0);              /* 31 + 1 wraps to 0 */
    CHECK(pmt.stream_count == 2);
    CHECK(st[0].pid == 0x0101 && st[1].pid == 0x0103);
    CHECK(st[1].descriptors.len == sizeof(lang));
    CHECK(pmt.pcr_pid == 0x0101);

    /* Byte-exact against the reference builder. */
    tb_es_t kept[] = { es[0], es[2] };
    uint8_t ref[256];
    size_t refn = tb_pmt(ref, 1, 0, 0x0101, NULL, 0, kept, 2);
    CHECK(refn == on && memcmp(ref, out, on) == 0);

    CHECK(moq_mpeg2ts_pmt_subset(B(pmt_sec, pn), keep, 2, out, 4, &on) == MOQ_ERR_BUFFER);
    pmt_sec[pn - 1] ^= 0x55;
    CHECK(moq_mpeg2ts_pmt_subset(B(pmt_sec, pn), keep, 2, out, sizeof(out), &on) == MOQ_ERR_PROTO);
}

/* -- Object validation -------------------------------------------------- */

static size_t fill_packets(uint8_t *buf, size_t count, uint16_t pid,
                           bool prefix)
{
    size_t o = 0;
    for (size_t i = 0; i < count; i++) {
        tb_pkt_t s = { .pid = pid, .cc = (uint8_t)i, .prefix = prefix,
                       .prefix_value = (uint32_t)(i * 1000) };
        o += tb_packet(buf + o, &s);
    }
    return o;
}

static void test_validate_object(void)
{
    uint8_t buf[192 * 4];
    moq_mpeg2ts_object_rules_t rules;
    moq_mpeg2ts_object_report_t rep;
    moq_mpeg2ts_object_rules_init(&rules);
    CHECK(rules.packet_size == 188);

    /* The report carries the verdict once, in reason: it follows
     * struct_size directly. */
    CHECK(offsetof(moq_mpeg2ts_object_report_t, reason) == sizeof(uint32_t));

    size_t n = fill_packets(buf, 3, 0x0100, false);
    moq_mpeg2ts_object_report_init(&rep);
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_OK);
    CHECK(rep.packet_count == 3 && rep.reason == MOQ_MPEG2TS_OK);

    /* Object Payload Format: an empty payload and a partial packet are
     * rejected. */
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, 0), &rep, sizeof(rep)) == MOQ_ERR_PROTO);
    CHECK(rep.reason == MOQ_MPEG2TS_ERR_EMPTY);
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n - 1), &rep, sizeof(rep)) == MOQ_ERR_PROTO);
    CHECK(rep.reason == MOQ_MPEG2TS_ERR_PARTIAL_PACKET);
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, 100), &rep, sizeof(rep)) == MOQ_ERR_PROTO);
    CHECK(rep.reason == MOQ_MPEG2TS_ERR_PARTIAL_PACKET);

    /* Sync byte at offset 0 of every 188-octet packet. */
    buf[188] = 0x46;
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_ERR_PROTO);
    CHECK(rep.reason == MOQ_MPEG2TS_ERR_SYNC_BYTE && rep.bad_packet == 1);

    /* 192: sync byte at offset 4 of every source packet. */
    rules.packet_size = 192;
    n = fill_packets(buf, 4, 0x0100, true);
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_OK);
    CHECK(rep.packet_count == 4);
    buf[3 * 192 + 4] = 0x00;
    buf[3 * 192] = 0x47;
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_ERR_PROTO);
    CHECK(rep.reason == MOQ_MPEG2TS_ERR_SYNC_BYTE && rep.bad_packet == 3);

    /* 188-octet packets read as a 192 track: 2 * 188 is not a multiple. */
    n = fill_packets(buf, 2, 0x0100, false);
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_ERR_PROTO);
    CHECK(rep.reason == MOQ_MPEG2TS_ERR_PARTIAL_PACKET);
    /* 4 * 188 == 752 is not a multiple of 192 either; 192 * 47 == 188 * 48
     * is, and must still fail on the sync byte. */
    {
        static uint8_t many[188 * 48];
        size_t mn = fill_packets(many, 48, 0x0100, false);
        CHECK(mn == 192 * 47);
        CHECK(moq_mpeg2ts_validate_object(&rules, B(many, mn), &rep, sizeof(rep)) == MOQ_ERR_PROTO);
        CHECK(rep.reason == MOQ_MPEG2TS_ERR_SYNC_BYTE);
    }

    /* es-packets: only es_pid, never a null packet. */
    rules.packet_size = 188;
    rules.es_packets = true;
    rules.es_pid = 0x0101;
    n = fill_packets(buf, 3, 0x0101, false);
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_OK);
    {
        tb_pkt_t s = { .pid = MOQ_MPEG2TS_PID_NULL };
        tb_packet(buf + 188, &s);
    }
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_ERR_PROTO);
    CHECK(rep.reason == MOQ_MPEG2TS_ERR_NULL_PACKET && rep.bad_packet == 1);
    {
        tb_pkt_t s = { .pid = 0x0102 };
        tb_packet(buf + 188, &s);
    }
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_ERR_PROTO);
    CHECK(rep.reason == MOQ_MPEG2TS_ERR_FOREIGN_PID && rep.bad_packet == 1);
    /* Not es-packets: null packets and other PIDs are fine. */
    rules.es_packets = false;
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_OK);

    /* Arguments. */
    rules.packet_size = 190;
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_ERR_INVAL);
    rules.packet_size = 188;
    rules.es_packets = true;
    rules.es_pid = 0x2000;
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), &rep, sizeof(rep)) == MOQ_ERR_INVAL);
    CHECK(moq_mpeg2ts_validate_object(NULL, B(buf, n), &rep, sizeof(rep)) == MOQ_ERR_INVAL);
    CHECK(moq_mpeg2ts_validate_object(&rules, B(buf, n), NULL, 0) == MOQ_ERR_INVAL);
}

/* -- A small single-program stream used by the inspection, receiver,
 * switcher and packetizer tests. ------------------------------------------ */

#define VPID 0x0101
#define APID 0x0102
#define PMTPID 0x0100

typedef struct stream_builder {
    uint8_t *buf;
    size_t   len;
    size_t   cap;
    uint8_t  cc[0x2000];
    bool     prefix;
} stream_builder_t;

static void sb_add(stream_builder_t *s, tb_pkt_t pk)
{
    size_t need = s->prefix ? 192 : 188;
    if (s->len + need > s->cap) {
        fprintf(stderr, "FAIL: stream builder overflow\n");
        exit(1);
    }
    pk.prefix = s->prefix;
    pk.prefix_value = (uint32_t)s->len;
    if (!pk.no_payload) {
        pk.cc = s->cc[pk.pid];
        s->cc[pk.pid] = (uint8_t)((s->cc[pk.pid] + 1) & 0x0F);
    } else {
        pk.cc = (uint8_t)((s->cc[pk.pid] + 15) & 0x0F);
    }
    s->len += tb_packet(s->buf + s->len, &pk);
}

static void sb_psi(stream_builder_t *s, uint8_t pat_version, uint8_t pmt_version)
{
    uint8_t sec[256], pl[188];
    tb_prog_t tp[] = { { 1, PMTPID } };
    size_t n = tb_pat(sec, 1, pat_version, tp, 1);
    size_t pn = tb_psi_payload(pl, sec, n);
    sb_add(s, (tb_pkt_t){ .pid = 0, .pusi = true, .payload = pl,
                          .payload_len = pn });
    tb_es_t es[] = { { 0x1B, VPID, NULL, 0 }, { 0x0F, APID, NULL, 0 } };
    n = tb_pmt(sec, 1, pmt_version, VPID, NULL, 0, es, 2);
    pn = tb_psi_payload(pl, sec, n);
    sb_add(s, (tb_pkt_t){ .pid = PMTPID, .pusi = true, .payload = pl,
                          .payload_len = pn });
}

static void sb_video(stream_builder_t *s, bool rap, uint64_t pts,
                     bool pcr, uint64_t pcr_base, bool di)
{
    uint8_t pes[64];
    size_t n = tb_pes(pes, 0xE0, true, pts, true, pts - 3000);
    sb_add(s, (tb_pkt_t){ .pid = VPID, .pusi = true, .rai = rap, .pcr = pcr,
                          .pcr_base = pcr_base, .di = di, .payload = pes,
                          .payload_len = n });
}

static void sb_cont(stream_builder_t *s, uint16_t pid)
{
    const uint8_t data[] = { 0xAB, 0xCD };
    sb_add(s, (tb_pkt_t){ .pid = pid, .payload = data, .payload_len = 2 });
}

static void sb_audio(stream_builder_t *s, uint64_t pts)
{
    uint8_t pes[64];
    size_t n = tb_pes(pes, 0xC0, true, pts, false, 0);
    sb_add(s, (tb_pkt_t){ .pid = APID, .pusi = true, .payload = pes,
                          .payload_len = n });
}

static stream_builder_t *sb_new(size_t packets, bool prefix)
{
    stream_builder_t *s = calloc(1, sizeof(*s));
    s->cap = packets * 192;
    s->buf = calloc(1, s->cap);
    s->prefix = prefix;
    return s;
}

static void sb_free(stream_builder_t *s)
{
    free(s->buf);
    free(s);
}

/* -- Inspection --------------------------------------------------------- */

static void test_inspect(void)
{
    for (int pf = 0; pf < 2; pf++) {
        bool prefix = pf == 1;
        uint32_t ps = prefix ? 192 : 188;
        stream_builder_t *s = sb_new(16, prefix);
        sb_audio(s, 1000);                       /* 0: audio before the RAP */
        sb_psi(s, 0, 0);                         /* 1, 2 */
        sb_video(s, true, 900000, true, 899000, false); /* 3: RAP + PCR */
        sb_cont(s, VPID);                        /* 4 */
        sb_add(s, (tb_pkt_t){ .pid = MOQ_MPEG2TS_PID_NULL });   /* 5 */
        sb_video(s, false, 903003, true, 902000, false);         /* 6 */

        moq_mpeg2ts_program_t progs[2];
        moq_mpeg2ts_pat_t pat;
        moq_mpeg2ts_pat_init(&pat, progs, 2);
        moq_mpeg2ts_stream_t st[4];
        moq_mpeg2ts_pmt_t pmt;
        moq_mpeg2ts_pmt_init(&pmt, st, 4);

        moq_mpeg2ts_inspect_cfg_t ip;
        moq_mpeg2ts_inspect_cfg_init(&ip);
        CHECK(ip.pmt_pid == MOQ_MPEG2TS_PID_NONE);
        ip.packet_size = ps;
        ip.pat = &pat;
        ip.pmt = &pmt;
        moq_mpeg2ts_object_info_t info;
        moq_mpeg2ts_object_info_init(&info);
        CHECK(moq_mpeg2ts_inspect_object(&ip, B(s->buf, s->len), &info, sizeof(info)) == MOQ_OK);
        CHECK(info.packet_count == 7);
        CHECK(info.null_packets == 1);
        CHECK(info.malformed_packets == 0);
        CHECK(info.has_pat && info.has_pmt && info.psi_complete);
        CHECK(info.pmt_pid == PMTPID && info.program_number == 1);
        CHECK(info.pmt_pcr_pid == VPID);
        CHECK(pat.program_count == 1 && progs[0].pmt_pid == PMTPID);
        CHECK(pmt.stream_count == 2);
        CHECK(st[0].stream_type == 0x1B && st[0].pid == VPID);
        CHECK(st[1].stream_type == 0x0F && st[1].pid == APID);
        CHECK(info.has_random_access && info.random_access_index == 3);
        CHECK(info.random_access_pid == VPID);
        /* Timing comes from the PES at the random access point, not from the
         * audio PES that precedes it. */
        CHECK(info.timing_pid == VPID);
        CHECK(info.has_pts && info.pts == 900000);
        CHECK(info.has_dts && info.dts == 897000);
        CHECK(info.pcr_pid == VPID);
        CHECK(info.has_pcr && info.pcr_count == 2);
        CHECK(info.first_pcr == 899000ull * 300u);
        CHECK(info.first_pcr_index == 3);
        CHECK(info.last_pcr == 902000ull * 300u && info.last_pcr_index == 6);
        CHECK(!info.pcr_discontinuity);
        sb_free(s);
    }

    /* No random access point: timing from the first PES with a PTS; a
     * discontinuity_indicator on a PCR packet is flagged; the PMT is found
     * through cfg->pmt_pid without a PAT in the object. */
    {
        stream_builder_t *s = sb_new(8, false);
        sb_psi(s, 0, 0);
        uint8_t keep_pmt[188];
        memcpy(keep_pmt, s->buf + 188, 188);
        s->len = 0;
        memcpy(s->buf, keep_pmt, 188);
        s->len = 188;
        sb_audio(s, 5000);
        sb_video(s, false, 9000, true, 8000, true);
        /* random_access_indicator without a PES start is not a RAP. */
        sb_add(s, (tb_pkt_t){ .pid = VPID, .rai = true,
                              .payload = (const uint8_t *)"\x01", .payload_len = 1 });

        moq_mpeg2ts_inspect_cfg_t ip;
        moq_mpeg2ts_inspect_cfg_init(&ip);
        ip.pmt_pid = PMTPID;
        moq_mpeg2ts_object_info_t info;
        moq_mpeg2ts_object_info_init(&info);
        CHECK(moq_mpeg2ts_inspect_object(&ip, B(s->buf, s->len), &info, sizeof(info)) == MOQ_OK);
        CHECK(!info.has_pat && info.has_pmt && !info.psi_complete);
        CHECK(!info.has_random_access);
        CHECK(info.timing_pid == APID && info.pts == 5000 && !info.has_dts);
        CHECK(info.pcr_discontinuity && info.pcr_discontinuity_index == 2);

        /* timing_pid pins the PES. */
        ip.timing_pid = VPID;
        CHECK(moq_mpeg2ts_inspect_object(&ip, B(s->buf, s->len), &info, sizeof(info)) == MOQ_OK);
        CHECK(info.timing_pid == VPID && info.pts == 9000 && info.dts == 6000);

        /* random_access_pid excludes other PIDs. */
        sb_free(s);
    }

    /* Multi-program: program_number selects the PMT; a corrupt PAT CRC is
     * counted and the PAT is ignored. */
    {
        stream_builder_t *s = sb_new(8, false);
        uint8_t sec[256], pl[188];
        tb_prog_t tp[] = { { 1, 0x0100 }, { 2, 0x0200 } };
        size_t n = tb_pat(sec, 1, 0, tp, 2);
        size_t pn = tb_psi_payload(pl, sec, n);
        sb_add(s, (tb_pkt_t){ .pid = 0, .pusi = true, .payload = pl, .payload_len = pn });
        tb_es_t es2[] = { { 0x24, 0x0201, NULL, 0 } };
        n = tb_pmt(sec, 2, 4, 0x0201, NULL, 0, es2, 1);
        pn = tb_psi_payload(pl, sec, n);
        sb_add(s, (tb_pkt_t){ .pid = 0x0200, .pusi = true, .payload = pl, .payload_len = pn });

        moq_mpeg2ts_stream_t st[2];
        moq_mpeg2ts_pmt_t pmt;
        moq_mpeg2ts_pmt_init(&pmt, st, 2);
        moq_mpeg2ts_inspect_cfg_t ip;
        moq_mpeg2ts_inspect_cfg_init(&ip);
        ip.program_number = 2;
        ip.pmt = &pmt;
        moq_mpeg2ts_object_info_t info;
        moq_mpeg2ts_object_info_init(&info);
        CHECK(moq_mpeg2ts_inspect_object(&ip, B(s->buf, s->len), &info, sizeof(info)) == MOQ_OK);
        CHECK(info.psi_complete && info.program_number == 2);
        CHECK(info.pmt_pid == 0x0200 && info.pmt_pcr_pid == 0x0201);
        CHECK(pmt.stream_count == 1 && st[0].stream_type == 0x24);

        s->buf[5 + 8] ^= 0x01;            /* corrupt the PAT body */
        moq_mpeg2ts_object_info_init(&info);
        CHECK(moq_mpeg2ts_inspect_object(&ip, B(s->buf, s->len), &info, sizeof(info)) == MOQ_OK);
        CHECK(!info.has_pat && info.bad_crc_sections == 1);
        CHECK(!info.psi_complete);

        /* PMT storage too small: still inspected, BUFFER reported. */
        s->buf[5 + 8] ^= 0x01;
        moq_mpeg2ts_pmt_init(&pmt, st, 0);
        CHECK(moq_mpeg2ts_inspect_object(&ip, B(s->buf, s->len), &info, sizeof(info)) == MOQ_ERR_BUFFER);
        CHECK(info.psi_complete && pmt.stream_count == 1);
        sb_free(s);
    }

    /* Malformed adaptation field is counted and skipped. */
    {
        stream_builder_t *s = sb_new(4, false);
        sb_video(s, true, 1000, true, 900, false);
        sb_cont(s, VPID);
        s->buf[188 + 3] |= 0x20;          /* claim an adaptation field */
        s->buf[188 + 4] = 190;            /* that overruns */
        moq_mpeg2ts_inspect_cfg_t ip;
        moq_mpeg2ts_inspect_cfg_init(&ip);
        ip.random_access_pid = APID;      /* RAP on VPID does not count */
        moq_mpeg2ts_object_info_t info;
        moq_mpeg2ts_object_info_init(&info);
        CHECK(moq_mpeg2ts_inspect_object(&ip, B(s->buf, s->len), &info, sizeof(info)) == MOQ_OK);
        CHECK(info.malformed_packets == 1);
        CHECK(!info.has_random_access);
        CHECK(moq_mpeg2ts_inspect_object(&ip, B(s->buf, 100), &info, sizeof(info)) == MOQ_ERR_INVAL);
        sb_free(s);
    }
}

/* -- Receiver ------------------------------------------------------------ */

static void test_receiver(void)
{
    stream_builder_t *s = sb_new(32, false);
    sb_psi(s, 0, 0);
    sb_video(s, true, 90000, true, 89000, false);          /* obj A: 3 pkts */
    size_t a_len = s->len;
    sb_cont(s, VPID);
    sb_video(s, false, 93003, true, 92000, false);         /* obj B: 2 pkts */
    size_t b_len = s->len;
    sb_cont(s, VPID);
    sb_video(s, false, 96006, true, 95000, false);         /* obj C */
    size_t c_len = s->len;
    sb_psi(s, 0, 0);
    sb_video(s, true, 99009, true, 98000, false);          /* obj D: RAP */
    size_t d_len = s->len;

    moq_bytes_t A = B(s->buf, a_len);
    moq_bytes_t Bo = B(s->buf + a_len, b_len - a_len);
    moq_bytes_t C = B(s->buf + b_len, c_len - b_len);
    moq_bytes_t D = B(s->buf + c_len, d_len - c_len);

    moq_mpeg2ts_receiver_cfg_t cfg;
    moq_mpeg2ts_receiver_cfg_init(&cfg);
    moq_mpeg2ts_receiver_t rx;
    CHECK(moq_mpeg2ts_receiver_init(&rx, sizeof(rx) - 1, &cfg) == MOQ_ERR_INVAL);
    cfg.packet_size = 190;
    CHECK(moq_mpeg2ts_receiver_init(&rx, sizeof(rx), &cfg) == MOQ_ERR_INVAL);
    cfg.packet_size = 188;
    CHECK(moq_mpeg2ts_receiver_init(&rx, sizeof(rx), &cfg) == MOQ_OK);

    uint8_t snapshot[192 * 32];
    memcpy(snapshot, s->buf, s->len);

    moq_mpeg2ts_receive_result_t r;
    moq_mpeg2ts_receive_result_init(&r);
    CHECK(moq_mpeg2ts_receiver_push(&rx, 5, 0, 0, A, &r, sizeof(r)) == MOQ_OK);
    CHECK(r.accepted && !r.gap && !r.discontinuous && r.random_access);
    CHECK(!r.joined_mid_group);
    CHECK(moq_mpeg2ts_receiver_push(&rx, 5, 1, 0, Bo, &r, sizeof(r)) == MOQ_OK);
    CHECK(r.accepted && !r.gap && !r.random_access);

    /* Object 2 lost: Object 3 arrives. The gap is located at (5, 2) and the
     * stream stays discontinuous until a random access point. */
    CHECK(moq_mpeg2ts_receiver_push(&rx, 5, 3, 0, C, &r, sizeof(r)) == MOQ_OK);
    CHECK(r.accepted && r.gap && r.gap_group == 5 && r.gap_object == 2);
    CHECK(r.discontinuous);
    CHECK(r.gap_duration_known);
    CHECK(r.gap_duration_27mhz == (95000ull - 92000ull) * 300u);

    /* Group 5 never signalled its end: moving to Group 6 reports the
     * possible tail (5, 4) as unknown, not as a loss. */
    CHECK(moq_mpeg2ts_receiver_push(&rx, 6, 0, 0, D, &r, sizeof(r)) == MOQ_OK);
    CHECK(r.gap && r.gap_tail_unknown && r.gap_group == 5 && r.gap_object == 4);
    CHECK(r.random_access && !r.discontinuous);

    /* Continuity counters are never touched. */
    CHECK(memcmp(snapshot, s->buf, s->len) == 0);

    /* A known final Object makes Group + 1, Object 0 contiguous. */
    CHECK(moq_mpeg2ts_receiver_push(&rx, 6, 1, MOQ_MPEG2TS_OBJ_LAST_IN_GROUP,
                                    Bo, &r, sizeof(r)) == MOQ_OK);
    CHECK(!r.gap);
    CHECK(moq_mpeg2ts_receiver_push(&rx, 7, 0, 0, C, &r, sizeof(r)) == MOQ_OK);
    CHECK(!r.gap && !r.gap_tail_unknown);
    CHECK(!r.discontinuous);

    /* Out of order. */
    CHECK(moq_mpeg2ts_receiver_push(&rx, 6, 9, 0, C, &r, sizeof(r)) == MOQ_ERR_PROTO);
    CHECK(!r.accepted && r.out_of_order);
    CHECK(moq_mpeg2ts_receiver_push(&rx, 7, 0, 0, C, &r, sizeof(r)) == MOQ_ERR_PROTO);
    CHECK(r.out_of_order);

    /* An invalid Object is rejected and counts as the gap. */
    CHECK(moq_mpeg2ts_receiver_push(&rx, 7, 1, 0, B(s->buf, 187), &r, sizeof(r)) ==
          MOQ_ERR_PROTO);
    CHECK(!r.accepted && r.reason == MOQ_MPEG2TS_ERR_PARTIAL_PACKET);
    CHECK(moq_mpeg2ts_receiver_push(&rx, 7, 2, 0, Bo, &r, sizeof(r)) == MOQ_OK);
    CHECK(r.accepted && r.gap && r.gap_group == 7 && r.gap_object == 1);
    CHECK(r.discontinuous);

    /* A skipped Group is a gap at its first Object. */
    CHECK(moq_mpeg2ts_receiver_push(&rx, 7, 3, MOQ_MPEG2TS_OBJ_LAST_IN_GROUP,
                                    Bo, &r, sizeof(r)) == MOQ_OK);
    CHECK(moq_mpeg2ts_receiver_push(&rx, 9, 0, 0, D, &r, sizeof(r)) == MOQ_OK);
    CHECK(r.gap && r.gap_group == 8 && r.gap_object == 0);
    CHECK(!r.discontinuous);

    /* Joining in the middle of a Group is reported. */
    moq_mpeg2ts_receiver_t rx2;
    CHECK(moq_mpeg2ts_receiver_init(&rx2, sizeof(rx2), &cfg) == MOQ_OK);
    CHECK(moq_mpeg2ts_receiver_push(&rx2, 3, 4, 0, Bo, &r, sizeof(r)) == MOQ_OK);
    CHECK(r.accepted && r.joined_mid_group && !r.gap);

    /* es-packets rules apply through the receiver. */
    cfg.es_packets = true;
    cfg.es_pid = VPID;
    moq_mpeg2ts_receiver_t rx3;
    CHECK(moq_mpeg2ts_receiver_init(&rx3, sizeof(rx3), &cfg) == MOQ_OK);
    CHECK(moq_mpeg2ts_receiver_push(&rx3, 0, 0, 0, A, &r, sizeof(r)) == MOQ_ERR_PROTO);
    CHECK(r.reason == MOQ_MPEG2TS_ERR_FOREIGN_PID);
    CHECK(moq_mpeg2ts_receiver_push(&rx3, 0, 1, 0, Bo, &r, sizeof(r)) == MOQ_OK);

    CHECK(moq_mpeg2ts_receiver_push(NULL, 0, 0, 0, A, &r, sizeof(r)) == MOQ_ERR_INVAL);
    sb_free(s);
}

/* -- Prefix removal and join PSI --------------------------------------- */

static void test_strip_prefix(void)
{
    stream_builder_t *s = sb_new(4, true);
    sb_psi(s, 0, 0);
    uint8_t out[188 * 2];
    size_t n = 0;
    CHECK(moq_mpeg2ts_strip_prefix(B(s->buf, s->len), out, sizeof(out), &n) == MOQ_OK);
    CHECK(n == 376);
    CHECK(memcmp(out, s->buf + 4, 188) == 0);
    CHECK(memcmp(out + 188, s->buf + 196, 188) == 0);

    uint8_t inplace[384];
    memcpy(inplace, s->buf, 384);
    CHECK(moq_mpeg2ts_strip_prefix(B(inplace, 384), inplace, 384, &n) == MOQ_OK);
    CHECK(n == 376 && memcmp(inplace, out, 376) == 0);

    CHECK(moq_mpeg2ts_strip_prefix(B(s->buf, 383), out, sizeof(out), &n) == MOQ_ERR_PROTO);
    CHECK(moq_mpeg2ts_strip_prefix(B(s->buf, 384), out, 300, &n) == MOQ_ERR_BUFFER);
    CHECK(n == 376);
    s->buf[196] = 0x00;
    CHECK(moq_mpeg2ts_strip_prefix(B(s->buf, 384), out, sizeof(out), &n) == MOQ_ERR_PROTO);
    sb_free(s);
}

static void test_align_join_psi(void)
{
    /* Catalog init data: PAT (cc 0), PMT part 1 (cc 0), PMT part 2 (cc 1). */
    uint8_t init[188 * 3];
    tb_pkt_t a = { .pid = 0, .pusi = true, .cc = 0 };
    tb_pkt_t b = { .pid = PMTPID, .pusi = true, .cc = 0 };
    tb_pkt_t c = { .pid = PMTPID, .cc = 1 };
    tb_packet(init, &a);
    tb_packet(init + 188, &b);
    tb_packet(init + 376, &c);

    /* First Object: PMT at cc 0 then PAT at cc 9 then video. */
    uint8_t first[188 * 3];
    tb_pkt_t f1 = { .pid = PMTPID, .pusi = true, .cc = 0 };
    tb_pkt_t f2 = { .pid = 0, .pusi = true, .cc = 9 };
    tb_pkt_t f3 = { .pid = VPID, .pusi = true, .cc = 4 };
    tb_packet(first, &f1);
    tb_packet(first + 188, &f2);
    tb_packet(first + 376, &f3);

    CHECK(moq_mpeg2ts_align_join_psi(init, sizeof(init), 188,
                                     B(first, sizeof(first))) == MOQ_OK);
    CHECK((init[3] & 0x0F) == 8);
    CHECK((init[188 + 3] & 0x0F) == 14);  /* counts back across the wrap */
    CHECK((init[376 + 3] & 0x0F) == 15);
    CHECK(init[3] >> 4 == 1);             /* afc untouched */

    /* A PID absent from the first Object is left alone. */
    tb_pkt_t other = { .pid = 0x0011, .cc = 6 };
    tb_packet(init, &other);
    CHECK(moq_mpeg2ts_align_join_psi(init, 188, 188, B(first, sizeof(first))) == MOQ_OK);
    CHECK((init[3] & 0x0F) == 6);

    CHECK(moq_mpeg2ts_align_join_psi(init, 187, 188, B(first, sizeof(first))) ==
          MOQ_ERR_PROTO);
    CHECK(moq_mpeg2ts_align_join_psi(init, 188, 188, B(first, 100)) == MOQ_ERR_PROTO);
}

/* -- Switcher ------------------------------------------------------------ */

static void parse_psi_packet(const uint8_t *pkt, uint8_t *version, bool *crc_ok)
{
    size_t ptr = pkt[4];
    const uint8_t *sec = pkt + 5 + ptr;
    size_t len = 3u + (((size_t)(sec[1] & 0x0F) << 8) | sec[2]);
    *version = (uint8_t)((sec[5] >> 1) & 0x1F);
    *crc_ok = tb_crc32(sec, len) == 0;
}

static void test_switcher(void)
{
    moq_mpeg2ts_switcher_t sw;
    CHECK(moq_mpeg2ts_switcher_init(&sw, sizeof(sw) - 1, 188) == MOQ_ERR_INVAL);
    CHECK(moq_mpeg2ts_switcher_init(&sw, sizeof(sw), 190) == MOQ_ERR_INVAL);
    CHECK(moq_mpeg2ts_switcher_init(&sw, sizeof(sw), 188) == MOQ_OK);

    /* Track 1: PAT v0, PMT v0, PCR packet. Passed through unchanged. */
    stream_builder_t *t1 = sb_new(8, false);
    sb_psi(t1, 0, 0);
    sb_video(t1, true, 1000, true, 900, false);
    uint8_t copy[188 * 3];
    memcpy(copy, t1->buf, t1->len);
    for (size_t i = 0; i < 3; i++)
        CHECK(moq_mpeg2ts_switcher_process(&sw, t1->buf + i * 188) == MOQ_OK);
    CHECK(memcmp(copy, t1->buf, t1->len) == 0);

    /* Track 2 also uses PAT v0 / PMT v0, so after the switch both must be
     * re-versioned; the first PCR packet gets the discontinuity_indicator. */
    moq_mpeg2ts_switcher_switch(&sw, NULL, 0);
    stream_builder_t *t2 = sb_new(16, false);
    sb_psi(t2, 0, 0);
    sb_video(t2, true, 5000, true, 4000, false);
    sb_video(t2, false, 8000, true, 7000, false);
    sb_psi(t2, 0, 0);
    for (size_t i = 0; i < 6; i++)
        CHECK(moq_mpeg2ts_switcher_process(&sw, t2->buf + i * 188) == MOQ_OK);

    uint8_t v; bool ok;
    parse_psi_packet(t2->buf, &v, &ok);
    CHECK(ok && v != 0);
    uint8_t pat_v = v;
    parse_psi_packet(t2->buf + 188, &v, &ok);
    CHECK(ok && v != 0);
    uint8_t pmt_v = v;

    moq_mpeg2ts_packet_t p;
    moq_mpeg2ts_packet_init(&p);
    CHECK(moq_mpeg2ts_parse_packet(B(t2->buf + 376, 188), 188, &p) == MOQ_OK);
    CHECK(p.has_pcr && p.discontinuity);
    CHECK(moq_mpeg2ts_parse_packet(B(t2->buf + 564, 188), 188, &p) == MOQ_OK);
    CHECK(p.has_pcr && !p.discontinuity);

    /* Later PAT/PMT of the same track keep the same rewritten version. */
    parse_psi_packet(t2->buf + 752, &v, &ok);
    CHECK(ok && v == pat_v);
    parse_psi_packet(t2->buf + 940, &v, &ok);
    CHECK(ok && v == pmt_v);

    /* A source version bump on the new track still differs and stays
     * consistent: v1 maps to pat_v + 1. */
    stream_builder_t *t3 = sb_new(4, false);
    sb_psi(t3, 1, 1);
    CHECK(moq_mpeg2ts_switcher_process(&sw, t3->buf) == MOQ_OK);
    parse_psi_packet(t3->buf, &v, &ok);
    CHECK(ok && v == ((pat_v + 1) & 0x1F));

    /* A second switch back to a track whose PAT version equals the last
     * emitted one is re-versioned again. */
    moq_mpeg2ts_switcher_switch(&sw, NULL, 0);
    stream_builder_t *t4 = sb_new(4, false);
    sb_psi(t4, (uint8_t)((pat_v + 1) & 0x1F), 0);
    CHECK(moq_mpeg2ts_switcher_process(&sw, t4->buf) == MOQ_OK);
    parse_psi_packet(t4->buf, &v, &ok);
    CHECK(ok && v != ((pat_v + 1) & 0x1F));

    /* A PMT that spans packets is re-versioned too (see
     * test_switcher_tables for the whole-section checks). */
    {
        uint8_t big[400];
        memset(big, 0, sizeof(big));
        for (size_t i = 0; i < sizeof(big); i += 2) big[i] = 0x80;
        tb_es_t es[] = { { 0x1B, VPID, big, sizeof(big) } };
        uint8_t sec[600], pk[188 * 4];
        size_t n = tb_pmt(sec, 1, 2, VPID, NULL, 0, es, 1);
        uint8_t cc = 0;
        size_t pl = 0;
        moq_mpeg2ts_packetize_section(PMTPID, &cc, B(sec, n), 188, pk, sizeof(pk), &pl);
        moq_mpeg2ts_switcher_switch(&sw, NULL, 0);
        for (size_t i = 0; i < pl / 188; i++)
            CHECK(moq_mpeg2ts_switcher_process(&sw, pk + i * 188) == MOQ_OK);
    }

    uint8_t junk[188] = { 0 };
    CHECK(moq_mpeg2ts_switcher_process(&sw, junk) == MOQ_ERR_PROTO);
    CHECK(moq_mpeg2ts_switcher_process(&sw, NULL) == MOQ_ERR_INVAL);
    sb_free(t1); sb_free(t2); sb_free(t3); sb_free(t4);
}

/* -- Egress timing ------------------------------------------------------- */

static void test_egress(void)
{
    uint64_t v = 0;
    CHECK(moq_mpeg2ts_pcr_interpolate(0, 0, 27000000, 1000000, 500000, &v) == MOQ_OK);
    CHECK(v == 13500000);
    /* Across a wrap. */
    CHECK(moq_mpeg2ts_pcr_interpolate(MOQ_MPEG2TS_PCR_WRAP - 1000, 0,
                                      1000, 2000, 1500, &v) == MOQ_OK);
    CHECK(v == 500);
    CHECK(moq_mpeg2ts_pcr_interpolate(MOQ_MPEG2TS_PCR_WRAP - 1000, 0,
                                      1000, 2000, 500, &v) == MOQ_OK);
    CHECK(v == MOQ_MPEG2TS_PCR_WRAP - 500);
    CHECK(moq_mpeg2ts_pcr_interpolate(0, 10, 100, 10, 10, &v) == MOQ_ERR_INVAL);
    CHECK(moq_mpeg2ts_pcr_interpolate(0, 0, 100, 10, 10, NULL) == MOQ_ERR_INVAL);

    /* 1 504 000 bit/s is 1000 TS packets per second. */
    CHECK(moq_mpeg2ts_stuffing_packets(1504000, 27000000, 900) == 100);
    CHECK(moq_mpeg2ts_stuffing_packets(1504000, 27000000, 1000) == 0);
    CHECK(moq_mpeg2ts_stuffing_packets(1504000, 27000000, 1200) == 0);
    CHECK(moq_mpeg2ts_stuffing_packets(0, 27000000, 0) == 0);
    /* No overflow at the extremes. */
    CHECK(moq_mpeg2ts_stuffing_packets(UINT64_MAX, UINT64_MAX, 0) > 0);

    uint8_t np[192];
    CHECK(moq_mpeg2ts_null_packet(188, np, sizeof(np)) == MOQ_OK);
    moq_mpeg2ts_packet_t p;
    moq_mpeg2ts_packet_init(&p);
    CHECK(moq_mpeg2ts_parse_packet(B(np, 188), 188, &p) == MOQ_OK);
    CHECK(p.pid == MOQ_MPEG2TS_PID_NULL && p.payload.len == 184);
    CHECK(moq_mpeg2ts_null_packet(192, np, sizeof(np)) == MOQ_OK);
    CHECK(np[4] == 0x47);
    CHECK(moq_mpeg2ts_null_packet(192, np, 188) == MOQ_ERR_BUFFER);
}

/* -- Per-program filter -------------------------------------------------- */

typedef struct filt_out {
    moq_mpeg2ts_filter_action_t action;
    uint8_t pkt[192];
} filt_out_t;

static filt_out_t filt(moq_mpeg2ts_program_filter_t *f, const uint8_t *in)
{
    filt_out_t o;
    memset(&o, 0, sizeof(o));
    moq_result_t rc = moq_mpeg2ts_program_filter_push(f, B(in, 188), o.pkt,
                                                      sizeof(o.pkt), &o.action);
    CHECK(rc == MOQ_OK);
    return o;
}

static void test_program_filter(void)
{
    uint8_t pk[188];
    uint8_t sec[256], pl[188];

    moq_mpeg2ts_program_filter_cfg_t cfg;
    moq_mpeg2ts_program_filter_cfg_init(&cfg);
    CHECK(cfg.keep_cat);
    uint16_t si[] = { 0x0011 };
    cfg.program_number = 1;
    cfg.si_pids = si;
    cfg.si_pid_count = 1;

    moq_mpeg2ts_program_filter_t *f = calloc(1, sizeof(*f));
    CHECK(moq_mpeg2ts_program_filter_init(f, sizeof(*f) - 1, &cfg) == MOQ_ERR_INVAL);
    cfg.program_number = 0;
    CHECK(moq_mpeg2ts_program_filter_init(f, sizeof(*f), &cfg) == MOQ_ERR_INVAL);
    cfg.program_number = 1;
    CHECK(moq_mpeg2ts_program_filter_init(f, sizeof(*f), &cfg) == MOQ_OK);

    /* Before any PAT/PMT, elementary stream packets are dropped. */
    tb_pkt_t v0 = { .pid = 0x0101 };
    tb_packet(pk, &v0);
    CHECK(filt(f, pk).action == MOQ_MPEG2TS_FILTER_DROP);

    /* Two-program PAT. */
    tb_prog_t tp[] = { { 0, 0x0010 }, { 1, 0x0100 }, { 2, 0x0200 } };
    size_t n = tb_pat(sec, 0x0042, 7, tp, 3);
    size_t pn = tb_psi_payload(pl, sec, n);
    tb_pkt_t pat_pk = { .pid = 0, .pusi = true, .cc = 5, .payload = pl,
                        .payload_len = pn };
    tb_packet(pk, &pat_pk);
    filt_out_t o = filt(f, pk);
    CHECK(o.action == MOQ_MPEG2TS_FILTER_PAT);
    {
        moq_mpeg2ts_packet_t p;
        moq_mpeg2ts_packet_init(&p);
        CHECK(moq_mpeg2ts_parse_packet(B(o.pkt, 188), 188, &p) == MOQ_OK);
        CHECK(p.pid == 0 && p.payload_unit_start);
        CHECK(p.continuity_counter == 0);
        const uint8_t *s = p.payload.data + 1 + p.payload.data[0];
        moq_mpeg2ts_program_t progs[4];
        moq_mpeg2ts_pat_t pat;
        moq_mpeg2ts_pat_init(&pat, progs, 4);
        CHECK(moq_mpeg2ts_parse_pat(B(s, 16), &pat) == MOQ_OK);
        CHECK(pat.transport_stream_id == 0x0042);
        CHECK(pat.version == 0);
        CHECK(!pat.has_network_pid);
        CHECK(pat.program_count == 1);
        CHECK(progs[0].program_number == 1 && progs[0].pmt_pid == 0x0100);
    }

    /* PMT of program 1 lists video, audio, and a CA_descriptor. */
    const uint8_t ca[] = { 0x09, 0x04, 0x0B, 0x00, 0xE5, 0x00 };
    tb_es_t es1[] = { { 0x1B, 0x0101, NULL, 0 }, { 0x0F, 0x0102, NULL, 0 } };
    n = tb_pmt(sec, 1, 0, 0x0101, ca, sizeof(ca), es1, 2);
    pn = tb_psi_payload(pl, sec, n);
    tb_pkt_t pmt1 = { .pid = 0x0100, .pusi = true, .payload = pl, .payload_len = pn };
    tb_packet(pk, &pmt1);
    o = filt(f, pk);
    CHECK(o.action == MOQ_MPEG2TS_FILTER_PASS && memcmp(o.pkt, pk, 188) == 0);

    tb_es_t es2[] = { { 0x1B, 0x0201, NULL, 0 } };
    n = tb_pmt(sec, 2, 0, 0x0201, NULL, 0, es2, 1);
    pn = tb_psi_payload(pl, sec, n);
    tb_pkt_t pmt2 = { .pid = 0x0200, .pusi = true, .payload = pl, .payload_len = pn };
    tb_packet(pk, &pmt2);
    CHECK(filt(f, pk).action == MOQ_MPEG2TS_FILTER_DROP);

    struct { uint16_t pid; moq_mpeg2ts_filter_action_t want; } cases[] = {
        { 0x0101, MOQ_MPEG2TS_FILTER_PASS },   /* video, PCR */
        { 0x0102, MOQ_MPEG2TS_FILTER_PASS },   /* audio */
        { 0x0500, MOQ_MPEG2TS_FILTER_PASS },   /* CA_PID */
        { 0x0001, MOQ_MPEG2TS_FILTER_PASS },   /* CAT */
        { 0x0011, MOQ_MPEG2TS_FILTER_PASS },   /* retained SI */
        { 0x0012, MOQ_MPEG2TS_FILTER_DROP },   /* other SI */
        { 0x0201, MOQ_MPEG2TS_FILTER_DROP },   /* program 2 */
        { 0x0010, MOQ_MPEG2TS_FILTER_DROP },   /* network PID */
        { 0x1FFF, MOQ_MPEG2TS_FILTER_DROP },   /* null, not kept */
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        tb_pkt_t s = { .pid = cases[i].pid, .cc = 3 };
        tb_packet(pk, &s);
        o = filt(f, pk);
        CHECK(o.action == cases[i].want);
        if (o.action == MOQ_MPEG2TS_FILTER_PASS)
            CHECK(memcmp(o.pkt, pk, 188) == 0);
    }

    /* Same PAT again: version unchanged, continuity counter advances. */
    n = tb_pat(sec, 0x0042, 8, tp, 3);     /* source version changes only */
    pn = tb_psi_payload(pl, sec, n);
    pat_pk.cc = 6;
    pat_pk.payload_len = pn;
    tb_packet(pk, &pat_pk);
    o = filt(f, pk);
    CHECK(o.action == MOQ_MPEG2TS_FILTER_PAT);
    CHECK((o.pkt[3] & 0x0F) == 1);
    CHECK(((o.pkt[5 + 5] >> 1) & 0x1F) == 0);

    /* Program 1 moves its PMT: rewritten content changes, version 1. */
    tb_prog_t tp2[] = { { 1, 0x0110 }, { 2, 0x0200 } };
    n = tb_pat(sec, 0x0042, 9, tp2, 2);
    pn = tb_psi_payload(pl, sec, n);
    pat_pk.payload_len = pn;
    tb_packet(pk, &pat_pk);
    o = filt(f, pk);
    CHECK(o.action == MOQ_MPEG2TS_FILTER_PAT);
    CHECK(((o.pkt[5 + 5] >> 1) & 0x1F) == 1);
    CHECK(tb_crc32(o.pkt + 5, 16) == 0);
    /* The old PMT PID is no longer carried, the new one is. */
    tb_packet(pk, &pmt1);
    CHECK(filt(f, pk).action == MOQ_MPEG2TS_FILTER_DROP);
    pmt1.pid = 0x0110;
    /* PMT v1 drops the audio stream. */
    tb_es_t es1b[] = { { 0x1B, 0x0101, NULL, 0 } };
    n = tb_pmt(sec, 1, 1, 0x0101, NULL, 0, es1b, 1);
    pn = tb_psi_payload(pl, sec, n);
    pmt1.payload = pl;
    pmt1.payload_len = pn;
    tb_packet(pk, &pmt1);
    CHECK(filt(f, pk).action == MOQ_MPEG2TS_FILTER_PASS);
    {
        tb_pkt_t s = { .pid = 0x0102 };
        tb_packet(pk, &s);
        CHECK(filt(f, pk).action == MOQ_MPEG2TS_FILTER_DROP);
        tb_pkt_t ca_s = { .pid = 0x0500 };
        tb_packet(pk, &ca_s);
        CHECK(filt(f, pk).action == MOQ_MPEG2TS_FILTER_DROP);
        tb_pkt_t vv = { .pid = 0x0101 };
        tb_packet(pk, &vv);
        CHECK(filt(f, pk).action == MOQ_MPEG2TS_FILTER_PASS);
    }
    CHECK(!moq_mpeg2ts_program_filter_program_gone(f));

    /* The PAT stops listing program 1. */
    tb_prog_t tp3[] = { { 2, 0x0200 } };
    n = tb_pat(sec, 0x0042, 10, tp3, 1);
    pn = tb_psi_payload(pl, sec, n);
    pat_pk.payload_len = pn;
    tb_packet(pk, &pat_pk);
    CHECK(filt(f, pk).action == MOQ_MPEG2TS_FILTER_DROP);
    CHECK(moq_mpeg2ts_program_filter_program_gone(f));

    /* keep_null. */
    cfg.keep_null = true;
    CHECK(moq_mpeg2ts_program_filter_init(f, sizeof(*f), &cfg) == MOQ_OK);
    tb_pkt_t nul = { .pid = 0x1FFF };
    tb_packet(pk, &nul);
    CHECK(filt(f, pk).action == MOQ_MPEG2TS_FILTER_PASS);

    /* Malformed input is dropped with PROTO. */
    pk[0] = 0;
    moq_mpeg2ts_filter_action_t act = MOQ_MPEG2TS_FILTER_PASS;
    uint8_t out[188];
    CHECK(moq_mpeg2ts_program_filter_push(f, B(pk, 188), out, sizeof(out), &act) ==
          MOQ_ERR_PROTO);
    CHECK(act == MOQ_MPEG2TS_FILTER_DROP);
    CHECK(moq_mpeg2ts_program_filter_push(f, B(pk, 188), out, 100, &act) ==
          MOQ_ERR_INVAL);
    free(f);
}

/* -- Packetizer ---------------------------------------------------------- */

typedef struct emitted {
    size_t   count;
    uint64_t group[64];
    uint64_t object[64];
    bool     new_group[64];
    bool     rap[64];
    size_t   len[64];
    uint16_t first_pid[64];
    uint8_t  bytes[188 * 256];
    size_t   total;
} emitted_t;

static void on_emit(void *ctx, const moq_mpeg2ts_emitted_object_t *o)
{
    emitted_t *e = ctx;
    if (e->count >= 64 || e->total + o->payload.len > sizeof(e->bytes)) {
        fprintf(stderr, "FAIL: emit overflow\n");
        exit(1);
    }
    size_t i = e->count++;
    e->group[i] = o->group;
    e->object[i] = o->object;
    e->new_group[i] = o->new_group;
    e->rap[i] = o->random_access;
    e->len[i] = o->payload.len;
    e->first_pid[i] = (uint16_t)(((o->payload.data[1] & 0x1F) << 8) |
                                 o->payload.data[2]);
    memcpy(e->bytes + e->total, o->payload.data, o->payload.len);
    e->total += o->payload.len;
}

static void test_packetizer(void)
{
    stream_builder_t *s = sb_new(64, false);
    /* Lead-in before the first random access point. */
    sb_cont(s, VPID);                                  /* 0 */
    sb_audio(s, 100);                                  /* 1 */
    /* GOP 1 at PCR 0 s. */
    sb_psi(s, 0, 0);                                   /* 2, 3 */
    sb_video(s, true, 90000, true, 90000, false);      /* 4 RAP */
    sb_cont(s, VPID);                                  /* 5 */
    sb_audio(s, 91000);                                /* 6 */
    sb_cont(s, VPID);                                  /* 7 */
    sb_video(s, false, 93003, true, 93003, false);     /* 8 */
    /* GOP 2 at PCR + 1 s. */
    sb_psi(s, 0, 0);                                   /* 9, 10 */
    sb_video(s, true, 180000, true, 180000, false);    /* 11 RAP */
    sb_cont(s, VPID);                                  /* 12 */
    /* GOP 3 only 0.1 s later. */
    sb_psi(s, 0, 0);                                   /* 13, 14 */
    sb_video(s, true, 189000, true, 189000, false);    /* 15 RAP */
    sb_cont(s, VPID);                                  /* 16 */

    static emitted_t e;
    memset(&e, 0, sizeof(e));
    uint8_t obj_buf[188 * 3];
    uint8_t lead_buf[188 * 4];

    moq_mpeg2ts_packetizer_cfg_t cfg;
    moq_mpeg2ts_packetizer_cfg_init(&cfg);
    CHECK(cfg.wait_for_random_access && cfg.group_at_random_access);
    cfg.object_packets = 3;
    cfg.object_buf = obj_buf;
    cfg.object_cap = sizeof(obj_buf);
    cfg.lead_buf = lead_buf;
    cfg.lead_cap = sizeof(lead_buf);
    cfg.min_group_ms = 500;
    cfg.emit = on_emit;
    cfg.ctx = &e;

    moq_mpeg2ts_packetizer_t pz;
    CHECK(moq_mpeg2ts_packetizer_init(&pz, sizeof(pz) - 1, &cfg) == MOQ_ERR_INVAL);
    cfg.object_cap = 188 * 2;
    CHECK(moq_mpeg2ts_packetizer_init(&pz, sizeof(pz), &cfg) == MOQ_ERR_INVAL);
    cfg.object_cap = sizeof(obj_buf);
    CHECK(moq_mpeg2ts_packetizer_init(&pz, sizeof(pz), &cfg) == MOQ_OK);

    for (size_t i = 0; i < s->len / 188; i++)
        CHECK(moq_mpeg2ts_packetizer_push(&pz, B(s->buf + i * 188, 188)) == MOQ_OK);
    CHECK(moq_mpeg2ts_packetizer_flush(&pz) == MOQ_OK);

    /* Packets 0, 1 precede the first random access point and are dropped.
     * Group 0 opens with the PAT/PMT run before the RAP: packets 2..8
     * (7 packets -> objects of 3, 3, 1). Group 1 opens at packet 9 and,
     * because GOP 3 comes only 100 ms later (min 500 ms), runs to the end:
     * packets 9..16 (8 packets -> 3, 3, 2). */
    CHECK(e.count == 6);
    CHECK(e.total == (17 - 2) * 188);
    CHECK(memcmp(e.bytes, s->buf + 2 * 188, e.total) == 0);  /* never split */
    CHECK(e.group[0] == 0 && e.object[0] == 0 && e.new_group[0]);
    CHECK(e.first_pid[0] == 0);           /* the Group starts with the PAT */
    CHECK(e.rap[0]);
    CHECK(e.group[1] == 0 && e.object[1] == 1 && !e.new_group[1]);
    CHECK(e.group[2] == 0 && e.object[2] == 2 && e.len[2] == 188);
    CHECK(e.group[3] == 1 && e.object[3] == 0 && e.new_group[3]);
    CHECK(e.first_pid[3] == 0 && e.rap[3]);
    CHECK(e.group[5] == 1 && e.object[5] == 2 && e.len[5] == 2 * 188);
    for (size_t i = 0; i < e.count; i++)
        CHECK(e.len[i] % 188 == 0 && e.len[i] > 0);

    moq_mpeg2ts_packetizer_stats_t st;
    CHECK(moq_mpeg2ts_packetizer_get_stats(&pz, &st, sizeof(st)) == MOQ_OK);
    CHECK(st.groups == 2 && st.objects == 6);
    CHECK(st.packets_in == 17);
    CHECK(st.packets_dropped_before_random_access == 2);
    CHECK(st.groups_over_2s == 0);
    CHECK(st.peak_rate_bps > 0);

    /* Without lead-in storage the Group starts at the RAP packet itself, and
     * with min_group_ms 0 every RAP opens a Group. */
    memset(&e, 0, sizeof(e));
    cfg.lead_buf = NULL;
    cfg.lead_cap = 0;
    cfg.min_group_ms = 0;
    CHECK(moq_mpeg2ts_packetizer_init(&pz, sizeof(pz), &cfg) == MOQ_OK);
    for (size_t i = 0; i < s->len / 188; i++)
        moq_mpeg2ts_packetizer_push(&pz, B(s->buf + i * 188, 188));
    moq_mpeg2ts_packetizer_flush(&pz);
    CHECK(e.first_pid[0] == VPID && e.rap[0] && e.new_group[0]);
    CHECK(moq_mpeg2ts_packetizer_get_stats(&pz, &st, sizeof(st)) == MOQ_OK);
    CHECK(st.groups == 3);
    CHECK(st.packets_dropped_before_random_access == 4);

    /* A packet without the sync byte is refused. */
    uint8_t bad[188] = { 0 };
    CHECK(moq_mpeg2ts_packetizer_push(&pz, B(bad, 188)) == MOQ_ERR_PROTO);
    CHECK(moq_mpeg2ts_packetizer_push(&pz, B(bad, 100)) == MOQ_ERR_INVAL);
    sb_free(s);
}

/* A Group longer than 2 s is reported; a PCR base wrap inside a Group is not
 * a discontinuity and is not reported as one; a source discontinuity is
 * passed through unchanged. */
static void test_packetizer_timing(void)
{
    stream_builder_t *s = sb_new(16, false);
    uint64_t near_wrap = MOQ_MPEG2TS_PTS_WRAP - 45000;   /* 0.5 s before */
    sb_video(s, true, 1000, true, near_wrap, false);
    sb_video(s, false, 2000, true, 45000, false);       /* wrapped: +1 s */
    sb_video(s, false, 3000, true, 45000 + 180000, false); /* +3 s */
    sb_video(s, false, 4000, true, 500, true);          /* source DI */

    static emitted_t e;
    memset(&e, 0, sizeof(e));
    uint8_t obj_buf[188 * 8];
    moq_mpeg2ts_packetizer_cfg_t cfg;
    moq_mpeg2ts_packetizer_cfg_init(&cfg);
    cfg.object_packets = 8;
    cfg.object_buf = obj_buf;
    cfg.object_cap = sizeof(obj_buf);
    cfg.emit = on_emit;
    cfg.ctx = &e;
    moq_mpeg2ts_packetizer_t pz;
    CHECK(moq_mpeg2ts_packetizer_init(&pz, sizeof(pz), &cfg) == MOQ_OK);
    for (size_t i = 0; i < 4; i++)
        CHECK(moq_mpeg2ts_packetizer_push(&pz, B(s->buf + i * 188, 188)) == MOQ_OK);
    CHECK(moq_mpeg2ts_packetizer_flush(&pz) == MOQ_OK);

    CHECK(e.count == 1);
    CHECK(memcmp(e.bytes, s->buf, 4 * 188) == 0);
    moq_mpeg2ts_packetizer_stats_t st;
    CHECK(moq_mpeg2ts_packetizer_get_stats(&pz, &st, sizeof(st)) == MOQ_OK);
    CHECK(st.groups == 1);
    CHECK(st.groups_over_2s == 1);
    CHECK(st.max_group_27mhz == (45000ull + 180000ull + 45000ull) * 300u);
    CHECK(st.source_pcr_discontinuities == 1);
    /* Consecutive PCR packets are 188 octets apart: 188 * 8 bit over the
     * wrapped 1 s step is the peak; the 2 s step gives half that, and the
     * step into the source discontinuity is not a rate sample. */
    CHECK(st.peak_rate_bps == 188u * 8u);
    sb_free(s);
}

/* -- The sizing contract --------------------------------------------------- */

#define CANARY 0xA5u
#define SPAN(t, f) offsetof(t, f), sizeof(((t *)0)->f)

/* A field the initializer gives a default. */
typedef struct dflt { size_t off, len; } dflt_t;

/* Every prefix 0 .. full + 16 of a sized initializer: below struct_size
 * nothing is written; otherwise exactly min(n, full) octets are cleared,
 * struct_size is stamped with that count, a default is set only on a field
 * the prefix covers whole, and nothing past the prefix is touched. `ref` is
 * the struct initialized at full size. */
static void check_init(const char *what,
                       void (*init)(void *p, size_t n), size_t full,
                       const uint8_t *ref, const dflt_t *d, size_t nd)
{
    uint8_t buf[512];
    for (size_t n = 0; n <= full + 16 && n <= sizeof(buf); n++) {
        memset(buf, CANARY, sizeof(buf));
        init(buf, n);
        size_t w = n < sizeof(uint32_t) ? 0 : (n < full ? n : full);
        bool ok = true;
        for (size_t i = 0; i < sizeof(buf); i++) {
            uint8_t want = CANARY;
            if (i < w) {
                want = 0;
                for (size_t k = 0; k < nd; k++)
                    if (i >= d[k].off && i < d[k].off + d[k].len &&
                        d[k].off + d[k].len <= w)
                        want = ref[i];
            }
            if (i < sizeof(uint32_t) && w) {
                uint32_t stamp = (uint32_t)w;
                want = ((const uint8_t *)&stamp)[i];
            }
            if (buf[i] != want) ok = false;
        }
        if (!ok) {
            fprintf(stderr, "FAIL: %s init prefix %zu\n", what, n);
            failures++;
        }
    }
}

#define INIT_FN(name, type)                                              \
    static void name##_thunk(void *p, size_t n)                          \
    { name##_init_sized((type *)p, n); }
INIT_FN(moq_mpeg2ts_object_rules, moq_mpeg2ts_object_rules_t)
INIT_FN(moq_mpeg2ts_object_report, moq_mpeg2ts_object_report_t)
INIT_FN(moq_mpeg2ts_inspect_cfg, moq_mpeg2ts_inspect_cfg_t)
INIT_FN(moq_mpeg2ts_object_info, moq_mpeg2ts_object_info_t)
INIT_FN(moq_mpeg2ts_receiver_cfg, moq_mpeg2ts_receiver_cfg_t)
INIT_FN(moq_mpeg2ts_receive_result, moq_mpeg2ts_receive_result_t)
INIT_FN(moq_mpeg2ts_delivery, moq_mpeg2ts_delivery_t)
INIT_FN(moq_mpeg2ts_program_filter_cfg, moq_mpeg2ts_program_filter_cfg_t)
INIT_FN(moq_mpeg2ts_packetizer_cfg, moq_mpeg2ts_packetizer_cfg_t)

static void test_sized_initializers(void)
{
    {
        typedef moq_mpeg2ts_object_rules_t T;
        T r; moq_mpeg2ts_object_rules_init(&r);
        CHECK(r.struct_size == sizeof(T) && r.packet_size == 188);
        const dflt_t d[] = { { SPAN(T, packet_size) } };
        check_init("rules", moq_mpeg2ts_object_rules_thunk, sizeof(T),
                   (const uint8_t *)&r, d, 1);
    }
    {
        typedef moq_mpeg2ts_object_report_t T;
        T r; moq_mpeg2ts_object_report_init(&r);
        check_init("report", moq_mpeg2ts_object_report_thunk, sizeof(T),
                   (const uint8_t *)&r, NULL, 0);
    }
    {
        typedef moq_mpeg2ts_inspect_cfg_t T;
        T r; moq_mpeg2ts_inspect_cfg_init(&r);
        const dflt_t d[] = { { SPAN(T, packet_size) }, { SPAN(T, pmt_pid) },
                             { SPAN(T, timing_pid) }, { SPAN(T, pcr_pid) },
                             { SPAN(T, random_access_pid) } };
        check_init("inspect_cfg", moq_mpeg2ts_inspect_cfg_thunk, sizeof(T),
                   (const uint8_t *)&r, d, 5);
    }
    {
        typedef moq_mpeg2ts_object_info_t T;
        T r; moq_mpeg2ts_object_info_init(&r);
        const dflt_t d[] = { { SPAN(T, pmt_pid) }, { SPAN(T, pmt_pcr_pid) },
                             { SPAN(T, pmt_video_pid) },
                             { SPAN(T, timing_pid) }, { SPAN(T, pcr_pid) },
                             { SPAN(T, random_access_pid) } };
        check_init("info", moq_mpeg2ts_object_info_thunk, sizeof(T),
                   (const uint8_t *)&r, d, 6);
    }
    {
        typedef moq_mpeg2ts_receiver_cfg_t T;
        T r; moq_mpeg2ts_receiver_cfg_init(&r);
        CHECK(r.random_access_pid == MOQ_MPEG2TS_PID_AUTO);
        const dflt_t d[] = { { SPAN(T, packet_size) }, { SPAN(T, pcr_pid) },
                             { SPAN(T, random_access_pid) } };
        check_init("receiver cfg", moq_mpeg2ts_receiver_cfg_thunk, sizeof(T),
                   (const uint8_t *)&r, d, 3);
    }
    {
        typedef moq_mpeg2ts_receive_result_t T;
        T r; moq_mpeg2ts_receive_result_init(&r);
        check_init("result", moq_mpeg2ts_receive_result_thunk, sizeof(T),
                   (const uint8_t *)&r, NULL, 0);
    }
    {
        typedef moq_mpeg2ts_delivery_t T;
        T r; moq_mpeg2ts_delivery_init(&r);
        CHECK(r.result.struct_size == sizeof(r.result));
        /* The embedded result's stamp is set only when the whole result is
         * covered. */
        const dflt_t d[] = { { SPAN(T, result) } };
        check_init("delivery", moq_mpeg2ts_delivery_thunk, sizeof(T),
                   (const uint8_t *)&r, d, 1);
    }
    {
        typedef moq_mpeg2ts_program_filter_cfg_t T;
        T r; moq_mpeg2ts_program_filter_cfg_init(&r);
        CHECK(r.keep_cat);
        const dflt_t d[] = { { SPAN(T, packet_size) }, { SPAN(T, keep_cat) } };
        check_init("filter cfg", moq_mpeg2ts_program_filter_cfg_thunk,
                   sizeof(T), (const uint8_t *)&r, d, 2);
    }
    {
        typedef moq_mpeg2ts_packetizer_cfg_t T;
        T r; moq_mpeg2ts_packetizer_cfg_init(&r);
        CHECK(r.object_packets == 64 && r.wait_for_random_access);
        const dflt_t d[] = { { SPAN(T, packet_size) },
                             { SPAN(T, object_packets) },
                             { SPAN(T, group_at_random_access) },
                             { SPAN(T, random_access_pid) },
                             { SPAN(T, wait_for_random_access) },
                             { SPAN(T, pcr_pid) } };
        check_init("packetizer cfg", moq_mpeg2ts_packetizer_cfg_thunk,
                   sizeof(T), (const uint8_t *)&r, d, 6);
    }
    /* NULL is ignored. */
    moq_mpeg2ts_object_rules_init_sized(NULL, 64);
}

/* An output of `size` octets inside canary storage: the call either refuses
 * it and writes nothing, or writes exactly min(size, full) octets with the
 * stamp equal to that count. */
static bool out_ok(const uint8_t *buf, size_t cap, size_t size, size_t full,
                   bool written)
{
    size_t w = written ? (size < full ? size : full) : 0;
    for (size_t i = w; i < cap; i++)
        if (buf[i] != CANARY) return false;
    if (!written) return true;
    uint32_t stamp;
    memcpy(&stamp, buf, sizeof(stamp));
    return stamp == w;
}

/* The sizes every output is driven at: one below the floor, the floor, a
 * partial tail between the floor and this build's size (when the floor ends
 * in padding), this build's size, and a larger caller. */
static size_t out_sizes(size_t floor, size_t full, size_t *sz)
{
    size_t n = 0;
    sz[n++] = floor - 1;
    sz[n++] = floor;
    if (full > floor + 1) sz[n++] = floor + 1;
    sz[n++] = full;
    sz[n++] = full + 16;
    return n;
}

static void test_sized_outputs(void)
{
    uint8_t pkt[188];
    tb_pkt_t ps = { .pid = 0x0101, .pusi = true, .cc = 0 };
    tb_packet(pkt, &ps);
    union { uint8_t b[512]; uint64_t align; } u;
    size_t sz[5], n;

    /* validate_object */
    moq_mpeg2ts_object_rules_t rules;
    moq_mpeg2ts_object_rules_init(&rules);
    n = out_sizes(MOQ_MPEG2TS_OBJECT_REPORT_V0_SIZE,
                  sizeof(moq_mpeg2ts_object_report_t), sz);
    for (size_t k = 0; k < n; k++) {
        memset(u.b, CANARY, sizeof(u.b));
        moq_result_t rc = moq_mpeg2ts_validate_object(
            &rules, B(pkt, 188), (moq_mpeg2ts_object_report_t *)u.b, sz[k]);
        bool written = sz[k] >= MOQ_MPEG2TS_OBJECT_REPORT_V0_SIZE;
        CHECK(rc == (written ? MOQ_OK : MOQ_ERR_INVAL));
        CHECK(out_ok(u.b, sizeof(u.b), sz[k],
                     sizeof(moq_mpeg2ts_object_report_t), written));
    }

    /* inspect_object: the Object holds the PAT and the matching PMT, so an
     * accepted inspection rewrites both cfg->pat and cfg->pmt; an
     * undersized info leaves both untouched. */
    uint8_t psi[2 * 188];
    spec_pat_packet(psi);
    {
        tb_es_t es[] = { { 0x1B, 0x0101, NULL, 0 } };
        uint8_t sec[64];
        size_t sn = tb_pmt(sec, 1, 0, 0x0101, NULL, 0, es, 1);
        uint8_t cc = 0;
        size_t pk_len = 0;
        CHECK(moq_mpeg2ts_packetize_section(0x0100, &cc, B(sec, sn), 188,
                                            psi + 188, 188, &pk_len) ==
              MOQ_OK);
        CHECK(pk_len == 188);
    }
    moq_mpeg2ts_inspect_cfg_t ip;
    moq_mpeg2ts_inspect_cfg_init(&ip);
    moq_mpeg2ts_program_t progs[2];
    moq_mpeg2ts_pat_t pat;
    moq_mpeg2ts_stream_t streams[2];
    moq_mpeg2ts_pmt_t pmt;
    n = out_sizes(MOQ_MPEG2TS_OBJECT_INFO_V0_SIZE,
                  sizeof(moq_mpeg2ts_object_info_t), sz);
    for (size_t k = 0; k < n; k++) {
        memset(u.b, CANARY, sizeof(u.b));
        moq_mpeg2ts_pat_init(&pat, progs, 2);
        pat.program_count = 77;
        ip.pat = &pat;
        moq_mpeg2ts_pmt_init(&pmt, streams, 2);
        pmt.stream_count = 77;
        pmt.pcr_pid = 0x0ABC;
        ip.pmt = &pmt;
        moq_result_t rc = moq_mpeg2ts_inspect_object(
            &ip, B(psi, sizeof(psi)), (moq_mpeg2ts_object_info_t *)u.b, sz[k]);
        bool written = sz[k] >= MOQ_MPEG2TS_OBJECT_INFO_V0_SIZE;
        CHECK(rc == (written ? MOQ_OK : MOQ_ERR_INVAL));
        CHECK(out_ok(u.b, sizeof(u.b), sz[k],
                     sizeof(moq_mpeg2ts_object_info_t), written));
        CHECK(written ? pat.program_count == 1 : pat.program_count == 77);
        if (written) {
            CHECK(pmt.stream_count == 1 && pmt.pcr_pid == 0x0101 &&
                  streams[0].pid == 0x0101);
        } else {
            CHECK(pmt.stream_count == 77 && pmt.pcr_pid == 0x0ABC &&
                  pmt.streams == streams && pmt.stream_cap == 2);
        }
    }
    ip.pat = NULL;
    ip.pmt = NULL;

    /* receiver_push: an undersized result is refused before the Object is
     * accounted, so the same Location is accepted afterwards. */
    moq_mpeg2ts_receiver_cfg_t rc0;
    moq_mpeg2ts_receiver_cfg_init(&rc0);
    static moq_mpeg2ts_receiver_t rx;
    n = out_sizes(MOQ_MPEG2TS_RECEIVE_RESULT_V0_SIZE,
                  sizeof(moq_mpeg2ts_receive_result_t), sz);
    for (size_t k = 0; k < n; k++) {
        CHECK(moq_mpeg2ts_receiver_init(&rx, sizeof(rx), &rc0) == MOQ_OK);
        memset(u.b, CANARY, sizeof(u.b));
        moq_result_t rc = moq_mpeg2ts_receiver_push(
            &rx, 0, 0, 0, B(pkt, 188), (moq_mpeg2ts_receive_result_t *)u.b,
            sz[k]);
        bool written = sz[k] >= MOQ_MPEG2TS_RECEIVE_RESULT_V0_SIZE;
        CHECK(rc == (written ? MOQ_OK : MOQ_ERR_INVAL));
        CHECK(out_ok(u.b, sizeof(u.b), sz[k],
                     sizeof(moq_mpeg2ts_receive_result_t), written));
        moq_mpeg2ts_receive_result_t r;
        moq_result_t again = moq_mpeg2ts_receiver_push(&rx, 0, 0, 0,
                                                       B(pkt, 188), &r,
                                                       sizeof(r));
        /* Accounted once: a repeat is out of order; refused: accepted now. */
        CHECK(written ? (again == MOQ_ERR_PROTO && r.out_of_order)
                      : (again == MOQ_OK && r.accepted));
    }

    /* receiver_poll: an undersized delivery releases nothing -- the held
     * Object, its payload and ctx, its Location and the deadline stay. */
    moq_mpeg2ts_reorder_slot_t slots[2];
    rc0.reorder_slots = slots;
    rc0.reorder_slot_count = 2;
    rc0.reorder_window_us = 100;
    n = out_sizes(MOQ_MPEG2TS_DELIVERY_V0_SIZE,
                  sizeof(moq_mpeg2ts_delivery_t), sz);
    int tag = 0;
    for (size_t k = 0; k < n; k++) {
        CHECK(moq_mpeg2ts_receiver_init(&rx, sizeof(rx), &rc0) == MOQ_OK);
        /* (0,1) waits for (0,0); it is due at 10 + 100. */
        CHECK(moq_mpeg2ts_receiver_offer(&rx, 10, 0, 1, 0, B(pkt, 188),
                                         &tag) == MOQ_OK);
        uint64_t dl = moq_mpeg2ts_receiver_next_deadline_us(&rx);
        CHECK(dl == 110);
        memset(u.b, CANARY, sizeof(u.b));
        moq_result_t rc = moq_mpeg2ts_receiver_poll(
            &rx, 200, (moq_mpeg2ts_delivery_t *)u.b, sz[k]);
        bool written = sz[k] >= MOQ_MPEG2TS_DELIVERY_V0_SIZE;
        CHECK(rc == (written ? MOQ_OK : MOQ_ERR_INVAL));
        CHECK(out_ok(u.b, sizeof(u.b), sz[k], sizeof(moq_mpeg2ts_delivery_t),
                     written));
        if (written) {
            const moq_mpeg2ts_delivery_t *d =
                (const moq_mpeg2ts_delivery_t *)u.b;
            CHECK(d->object == 1 && d->ctx == &tag &&
                  d->payload.data == pkt);
            CHECK(d->result.struct_size == sizeof(d->result));
            CHECK(moq_mpeg2ts_receiver_next_deadline_us(&rx) == UINT64_MAX);
        } else {
            CHECK(moq_mpeg2ts_receiver_next_deadline_us(&rx) == dl);
            CHECK(slots[0].used || slots[1].used);
            moq_mpeg2ts_delivery_t d;
            CHECK(moq_mpeg2ts_receiver_poll(&rx, 200, &d, sizeof(d)) ==
                  MOQ_OK);
            CHECK(d.group == 0 && d.object == 1 && d.ctx == &tag &&
                  d.payload.data == pkt);
        }
    }
}

int main(void)
{
    test_parse_packet();
    test_clock_helpers();
    test_pes();
    test_pat();
    test_pmt();
    test_section_buf_and_packetize();
    test_pmt_subset();
    test_validate_object();
    test_sized_initializers();
    test_sized_outputs();
    test_inspect();
    test_receiver();
    test_strip_prefix();
    test_align_join_psi();
    test_switcher();
    test_egress();
    test_program_filter();
    test_packetizer();
    test_packetizer_timing();

    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("mpeg2ts: all tests passed\n");
    return 0;
}
