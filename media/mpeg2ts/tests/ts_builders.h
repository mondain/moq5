#ifndef MOQ_MPEG2TS_TEST_BUILDERS_H
#define MOQ_MPEG2TS_TEST_BUILDERS_H

/*
 * Hand-built MPEG-2 TS vectors for the mpeg2ts tests. Everything here is
 * written from ISO/IEC 13818-1 directly and shares no code with the library:
 * the CRC is a bitwise reference, the section and packet layouts are spelled
 * out field by field.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Bitwise CRC_32/MPEG-2 reference. */
static inline uint32_t tb_crc32(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint32_t)d[i] << 24;
        for (int b = 0; b < 8; b++)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
    }
    return c;
}

static inline void tb_put32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

typedef struct tb_pkt {
    uint16_t       pid;
    bool           pusi;
    uint8_t        cc;
    uint8_t        scrambling;
    bool           af;         /* force an adaptation field */
    bool           rai;
    bool           di;
    bool           pcr;
    uint64_t       pcr_base;
    uint16_t       pcr_ext;
    bool           no_payload; /* adaptation field only */
    const uint8_t *payload;
    size_t         payload_len;
    bool           prefix;     /* write a 192-octet source packet */
    uint32_t       prefix_value;
} tb_pkt_t;

/* Writes 188 (or 192) octets. Payload is followed by 0xFF fill. */
static inline size_t tb_packet(uint8_t *out, const tb_pkt_t *s)
{
    uint8_t *p = out;
    if (s->prefix) { tb_put32(p, s->prefix_value); p += 4; }
    memset(p, 0xFF, 188);
    bool af = s->af || s->rai || s->di || s->pcr || s->no_payload;
    uint8_t afc = s->no_payload ? 2 : (af ? 3 : 1);
    p[0] = 0x47;
    p[1] = (uint8_t)((s->pusi ? 0x40 : 0) | ((s->pid >> 8) & 0x1F));
    p[2] = (uint8_t)s->pid;
    p[3] = (uint8_t)((s->scrambling << 6) | (afc << 4) | (s->cc & 0x0F));
    size_t off = 4;
    if (af) {
        size_t af_body = 1 + (s->pcr ? 6 : 0);
        size_t af_len = s->no_payload ? 183 : af_body;
        p[4] = (uint8_t)af_len;
        p[5] = (uint8_t)((s->di ? 0x80 : 0) | (s->rai ? 0x40 : 0) |
                         (s->pcr ? 0x10 : 0));
        if (s->pcr) {
            uint64_t b = s->pcr_base;
            p[6]  = (uint8_t)(b >> 25);
            p[7]  = (uint8_t)(b >> 17);
            p[8]  = (uint8_t)(b >> 9);
            p[9]  = (uint8_t)(b >> 1);
            p[10] = (uint8_t)(((b & 1u) << 7) | 0x7E | ((s->pcr_ext >> 8) & 1u));
            p[11] = (uint8_t)s->pcr_ext;
        }
        off = 5 + af_len;
    }
    if (!s->no_payload && s->payload) {
        size_t n = s->payload_len;
        if (n > 188 - off) n = 188 - off;
        memcpy(p + off, s->payload, n);
    }
    return s->prefix ? 192 : 188;
}

/* Section with its CRC_32 appended. hdr is everything from table_id up to
 * the CRC; section_length in hdr[1..2] is filled in. Returns total length. */
static inline size_t tb_finish_section(uint8_t *sec, size_t body_len)
{
    size_t section_length = body_len - 3 + 4;
    sec[1] = (uint8_t)((sec[1] & 0xF0) | ((section_length >> 8) & 0x0F));
    sec[2] = (uint8_t)section_length;
    tb_put32(sec + body_len, tb_crc32(sec, body_len));
    return body_len + 4;
}

typedef struct tb_prog { uint16_t number; uint16_t pid; } tb_prog_t;

static inline size_t tb_pat(uint8_t *sec, uint16_t tsid, uint8_t version,
                            const tb_prog_t *progs, size_t n)
{
    size_t o = 0;
    sec[o++] = 0x00;
    sec[o++] = 0xB0;
    sec[o++] = 0x00;
    sec[o++] = (uint8_t)(tsid >> 8);
    sec[o++] = (uint8_t)tsid;
    sec[o++] = (uint8_t)(0xC1 | ((version & 0x1F) << 1));
    sec[o++] = 0x00;
    sec[o++] = 0x00;
    for (size_t i = 0; i < n; i++) {
        sec[o++] = (uint8_t)(progs[i].number >> 8);
        sec[o++] = (uint8_t)progs[i].number;
        sec[o++] = (uint8_t)(0xE0 | ((progs[i].pid >> 8) & 0x1F));
        sec[o++] = (uint8_t)progs[i].pid;
    }
    return tb_finish_section(sec, o);
}

typedef struct tb_es {
    uint8_t        type;
    uint16_t       pid;
    const uint8_t *desc;
    size_t         desc_len;
} tb_es_t;

static inline size_t tb_pmt(uint8_t *sec, uint16_t program, uint8_t version,
                            uint16_t pcr_pid, const uint8_t *pdesc,
                            size_t pdesc_len, const tb_es_t *es, size_t n)
{
    size_t o = 0;
    sec[o++] = 0x02;
    sec[o++] = 0xB0;
    sec[o++] = 0x00;
    sec[o++] = (uint8_t)(program >> 8);
    sec[o++] = (uint8_t)program;
    sec[o++] = (uint8_t)(0xC1 | ((version & 0x1F) << 1));
    sec[o++] = 0x00;
    sec[o++] = 0x00;
    sec[o++] = (uint8_t)(0xE0 | ((pcr_pid >> 8) & 0x1F));
    sec[o++] = (uint8_t)pcr_pid;
    sec[o++] = (uint8_t)(0xF0 | ((pdesc_len >> 8) & 0x0F));
    sec[o++] = (uint8_t)pdesc_len;
    if (pdesc_len) { memcpy(sec + o, pdesc, pdesc_len); o += pdesc_len; }
    for (size_t i = 0; i < n; i++) {
        sec[o++] = es[i].type;
        sec[o++] = (uint8_t)(0xE0 | ((es[i].pid >> 8) & 0x1F));
        sec[o++] = (uint8_t)es[i].pid;
        sec[o++] = (uint8_t)(0xF0 | ((es[i].desc_len >> 8) & 0x0F));
        sec[o++] = (uint8_t)es[i].desc_len;
        if (es[i].desc_len) {
            memcpy(sec + o, es[i].desc, es[i].desc_len);
            o += es[i].desc_len;
        }
    }
    return tb_finish_section(sec, o);
}

/* Set section_number / last_section_number of a long-form section of total
 * length n and recompute its CRC_32. */
static inline size_t tb_section_numbers(uint8_t *sec, size_t n, uint8_t number,
                                        uint8_t last)
{
    sec[6] = number;
    sec[7] = last;
    return tb_finish_section(sec, n - 4);
}

/* Set current_next_indicator of a long-form section and recompute CRC_32. */
static inline size_t tb_section_current(uint8_t *sec, size_t n, bool current)
{
    sec[5] = (uint8_t)((sec[5] & 0xFE) | (current ? 1 : 0));
    return tb_finish_section(sec, n - 4);
}

/* A CAT (table_id 0x01) whose descriptor loop is `desc`. */
static inline size_t tb_cat(uint8_t *sec, uint8_t version, const uint8_t *desc,
                            size_t desc_len)
{
    size_t o = 0;
    sec[o++] = 0x01;
    sec[o++] = 0xB0;
    sec[o++] = 0x00;
    sec[o++] = 0xFF;
    sec[o++] = 0xFF;
    sec[o++] = (uint8_t)(0xC1 | ((version & 0x1F) << 1));
    sec[o++] = 0x00;
    sec[o++] = 0x00;
    if (desc_len) { memcpy(sec + o, desc, desc_len); o += desc_len; }
    return tb_finish_section(sec, o);
}

/* A CA_descriptor (tag 0x09) for CA_system_ID `system` and CA_PID `pid`. */
static inline size_t tb_ca_desc(uint8_t *out, uint16_t system, uint16_t pid)
{
    out[0] = 0x09;
    out[1] = 0x04;
    out[2] = (uint8_t)(system >> 8);
    out[3] = (uint8_t)system;
    out[4] = (uint8_t)(0xE0 | ((pid >> 8) & 0x1F));
    out[5] = (uint8_t)pid;
    return 6;
}

/* PSI payload of a packet with payload_unit_start: pointer_field 0 then the
 * section. */
static inline size_t tb_psi_payload(uint8_t *out, const uint8_t *sec, size_t n)
{
    out[0] = 0x00;
    memcpy(out + 1, sec, n);
    return n + 1;
}

static inline void tb_ts(uint8_t *p, uint8_t prefix4, uint64_t ts)
{
    p[0] = (uint8_t)((prefix4 << 4) | (((ts >> 30) & 7u) << 1) | 1u);
    p[1] = (uint8_t)(ts >> 22);
    p[2] = (uint8_t)((((ts >> 15) & 0x7Fu) << 1) | 1u);
    p[3] = (uint8_t)(ts >> 7);
    p[4] = (uint8_t)(((ts & 0x7Fu) << 1) | 1u);
}

/* PES header start: start code, stream_id, length 0, '10' flags, PTS/DTS. */
static inline size_t tb_pes(uint8_t *out, uint8_t stream_id, bool has_pts,
                            uint64_t pts, bool has_dts, uint64_t dts)
{
    size_t o = 0;
    out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x01;
    out[o++] = stream_id;
    out[o++] = 0x00; out[o++] = 0x00;
    out[o++] = 0x80;
    out[o++] = (uint8_t)((has_pts ? 0x80 : 0) | (has_dts ? 0x40 : 0));
    out[o++] = (uint8_t)((has_pts ? 5 : 0) + (has_dts ? 5 : 0));
    if (has_pts) { tb_ts(out + o, has_dts ? 3 : 2, pts); o += 5; }
    if (has_dts) { tb_ts(out + o, 1, dts); o += 5; }
    /* A few octets of elementary stream data. */
    out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x00; out[o++] = 0x01;
    return o;
}

#endif /* MOQ_MPEG2TS_TEST_BUILDERS_H */
