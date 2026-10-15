#ifndef MOQ_SERVICE_TEST_TS_STREAM_H
#define MOQ_SERVICE_TEST_TS_STREAM_H

/*
 * A generated single-program (or two-program) MPEG-2 TS for the service
 * tests, built packet by packet with the media/mpeg2ts test builders (no
 * library code). Each GOP is: PAT, PMT, a video PES start carrying the
 * random_access_indicator and a PCR, `video_cont` video continuation
 * packets, and one audio PES packet. Continuity counters advance per PID.
 * The second program (when asked for) interleaves its own PMT and video PES
 * on other PIDs, so a per-program filter has something to remove.
 */

#include "ts_builders.h"

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#define TS_PMT_PID    0x0100u
#define TS_VIDEO_PID  0x0101u
#define TS_AUDIO_PID  0x0102u
#define TS_PMT2_PID   0x0200u
#define TS_VIDEO2_PID 0x0201u

typedef struct ts_gen {
    uint32_t packet_size;   /* 188 or 192 */
    uint16_t program;       /* program number of the main program */
    bool     two_programs;  /* add program `program + 1` on other PIDs */
    uint8_t  cc[0x2000];
    uint64_t pcr_base;      /* 90 kHz */
    uint32_t prefix_clock;  /* 192-octet prefix value */
} ts_gen_t;

static inline void ts_gen_init(ts_gen_t *g, uint32_t packet_size,
                               uint16_t program, bool two_programs)
{
    memset(g, 0, sizeof(*g));
    g->packet_size = packet_size;
    g->program = program;
    g->two_programs = two_programs;
    g->pcr_base = 900000;
}

static inline size_t ts_gen_put(ts_gen_t *g, uint8_t *out, tb_pkt_t *p)
{
    p->cc = g->cc[p->pid] & 0x0F;
    if (!p->no_payload) g->cc[p->pid]++;
    p->prefix = g->packet_size == 192u;
    p->prefix_value = g->prefix_clock++;
    return tb_packet(out, p);
}

/* The PAT and the main program's PMT: the stream's PSI, as an
 * initialization-data run. Returns octets written. */
static inline size_t ts_gen_psi(ts_gen_t *g, uint8_t *out)
{
    uint8_t sec[256], pl[256];
    size_t o = 0;
    tb_prog_t progs[2] = { { g->program, TS_PMT_PID },
                           { (uint16_t)(g->program + 1), TS_PMT2_PID } };
    size_t n = tb_pat(sec, 1, 0, progs, g->two_programs ? 2 : 1);
    tb_pkt_t p;
    memset(&p, 0, sizeof(p));
    p.pid = 0; p.pusi = true;
    p.payload = pl; p.payload_len = tb_psi_payload(pl, sec, n);
    o += ts_gen_put(g, out + o, &p);

    tb_es_t es[2] = { { 0x1B, TS_VIDEO_PID, NULL, 0 },
                      { 0x0F, TS_AUDIO_PID, NULL, 0 } };
    n = tb_pmt(sec, g->program, 0, TS_VIDEO_PID, NULL, 0, es, 2);
    memset(&p, 0, sizeof(p));
    p.pid = TS_PMT_PID; p.pusi = true;
    p.payload = pl; p.payload_len = tb_psi_payload(pl, sec, n);
    o += ts_gen_put(g, out + o, &p);

    if (g->two_programs) {
        tb_es_t es2[1] = { { 0x1B, TS_VIDEO2_PID, NULL, 0 } };
        n = tb_pmt(sec, (uint16_t)(g->program + 1), 0, TS_VIDEO2_PID, NULL, 0,
                   es2, 1);
        memset(&p, 0, sizeof(p));
        p.pid = TS_PMT2_PID; p.pusi = true;
        p.payload = pl; p.payload_len = tb_psi_payload(pl, sec, n);
        o += ts_gen_put(g, out + o, &p);
    }
    return o;
}

/* One GOP. `out` must hold (5 + video_cont + two_programs) packets.
 * Returns octets written. */
static inline size_t ts_gen_gop(ts_gen_t *g, uint8_t *out, size_t video_cont)
{
    size_t o = ts_gen_psi(g, out);
    uint8_t pes[32];
    tb_pkt_t p;

    /* Video random access point: RAI and PCR on the PES start. */
    memset(&p, 0, sizeof(p));
    p.pid = TS_VIDEO_PID; p.pusi = true; p.rai = true;
    p.pcr = true; p.pcr_base = g->pcr_base;
    p.payload = pes;
    p.payload_len = tb_pes(pes, 0xE0, true, g->pcr_base + 9000, true,
                           g->pcr_base + 6000);
    o += ts_gen_put(g, out + o, &p);
    for (size_t i = 0; i < video_cont; i++) {
        uint8_t es[8] = { 0x11, (uint8_t)i, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77 };
        memset(&p, 0, sizeof(p));
        p.pid = TS_VIDEO_PID;
        p.payload = es; p.payload_len = sizeof(es);
        o += ts_gen_put(g, out + o, &p);
    }
    memset(&p, 0, sizeof(p));
    p.pid = TS_AUDIO_PID; p.pusi = true;
    p.payload = pes;
    p.payload_len = tb_pes(pes, 0xC0, true, g->pcr_base + 9000, false, 0);
    o += ts_gen_put(g, out + o, &p);
    if (g->two_programs) {
        memset(&p, 0, sizeof(p));
        p.pid = TS_VIDEO2_PID; p.pusi = true; p.rai = true;
        p.payload = pes;
        p.payload_len = tb_pes(pes, 0xE0, true, g->pcr_base, false, 0);
        o += ts_gen_put(g, out + o, &p);
    }
    g->pcr_base += 90000;   /* one second per GOP */
    return o;
}

#endif /* MOQ_SERVICE_TEST_TS_STREAM_H */
