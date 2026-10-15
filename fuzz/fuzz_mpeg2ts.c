/*
 * Fuzz the MPEG-2 TS packaging helpers with adversarial source packets.
 *
 * Input: one control octet, then the payload.
 *   bit 0: 192-octet source packets (else 188)
 *   bit 1: es-packets rules on PID 0x0101
 *   bits 2..7: Object split, in packets (0 = one Object); bits 2 and 3 also
 *   pick packetizer, switcher and inspection options (bit 3: PMT on PID
 *   0x0100), bits 4..7 the reorder window's arrival order and the
 *   packetizer's Object size
 * Every entry point that reads packet bytes is driven over the payload; the
 * oracles are memory safety plus invariants that must hold for any input.
 */

#include <moq/mpeg2ts.h>

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

typedef struct emit_state {
    size_t total;
    size_t limit;
    size_t object_packets;
    bool   rap_groups;          /* every Group is opened at a random access */
} emit_state_t;

static void on_emit(void *ctx, const moq_mpeg2ts_emitted_object_t *o)
{
    emit_state_t *e = ctx;
    e->total += o->payload.len;
    /* Packets are never split, and Objects respect their bound. */
    if (o->payload.len == 0 || o->payload.len % e->limit != 0) abort();
    if (o->packet_count > e->object_packets) abort();
    /* A Group opened at a random access point has it in its first Object. */
    if (e->rap_groups && o->new_group && !o->random_access) abort();
}

static void check_section(moq_bytes_t sec)
{
    if (sec.len < 3 || sec.len > MOQ_MPEG2TS_SECTION_MAX) abort();
    size_t sl = ((size_t)(sec.data[1] & 0x0Fu) << 8) | sec.data[2];
    if (3 + sl != sec.len) abort();
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size < 1) return 0;
    uint8_t ctl = data[0];
    const uint8_t *pl = data + 1;
    size_t n = size - 1;
    uint32_t ps = (ctl & 1u) ? 192u : 188u;
    size_t off_ts = ps == 192 ? 4 : 0;
    size_t whole = n - n % ps;
    moq_bytes_t payload = { pl, n };
    moq_bytes_t framed = { pl, whole };

    /* Object validation. */
    moq_mpeg2ts_object_rules_t rules;
    moq_mpeg2ts_object_rules_init(&rules);
    rules.packet_size = ps;
    rules.es_packets = (ctl & 2u) != 0;
    rules.es_pid = 0x0101;
    moq_mpeg2ts_object_report_t rep;
    moq_mpeg2ts_object_report_init(&rep);
    moq_result_t vrc = moq_mpeg2ts_validate_object(&rules, payload, &rep, sizeof(rep));
    if ((vrc == MOQ_OK) != (rep.reason == MOQ_MPEG2TS_OK)) abort();
    if (vrc == MOQ_OK && (n == 0 || n % ps != 0)) abort();

    /* Inspection, with small and large PAT and PMT tables: the caller's
     * table bounds every write. */
    static moq_mpeg2ts_program_t progs[300];
    static moq_mpeg2ts_stream_t streams[300];
    for (int big = 0; big < 2; big++) {
        size_t cap = big ? 300 : 8;
        moq_mpeg2ts_pat_t pat;
        moq_mpeg2ts_pat_init(&pat, progs, cap);
        moq_mpeg2ts_pmt_t pmt;
        moq_mpeg2ts_pmt_init(&pmt, streams, cap);
        moq_mpeg2ts_inspect_cfg_t ip;
        moq_mpeg2ts_inspect_cfg_init(&ip);
        ip.packet_size = ps;
        ip.pat = &pat;
        ip.pmt = &pmt;
        /* The large pass can also name the PMT PID, so a PMT without its PAT
         * is read straight into the large table. */
        if (big && (ctl & 8u)) ip.pmt_pid = 0x0100;
        ip.random_access_pid = big ? MOQ_MPEG2TS_PID_AUTO : MOQ_MPEG2TS_PID_NONE;
        ip.timing_pid = big ? MOQ_MPEG2TS_PID_AUTO : MOQ_MPEG2TS_PID_NONE;
        moq_mpeg2ts_object_info_t info;
        moq_mpeg2ts_object_info_init(&info);
        moq_result_t irc = moq_mpeg2ts_inspect_object(&ip, framed, &info, sizeof(info));
        if (irc == MOQ_OK || irc == MOQ_ERR_BUFFER) {
            if (info.packet_count != whole / ps) abort();
            if (info.has_random_access &&
                info.random_access_index >= info.packet_count)
                abort();
            if (info.psi_complete && !(info.has_pat && info.has_pmt)) abort();
            if (info.has_pmt) {
                size_t w = pmt.stream_count < cap ? pmt.stream_count : cap;
                if (pmt.stream_count > cap && irc != MOQ_ERR_BUFFER) abort();
                for (size_t i = 0; i < w; i++)
                    if (streams[i].descriptors.data) abort();
            }
        }
    }

    /* Per-packet parsers and section reassembly. */
    uint8_t storage[MOQ_MPEG2TS_SECTION_MAX];
    moq_mpeg2ts_section_buf_t sb;
    if (moq_mpeg2ts_section_buf_init(&sb, sizeof(sb), storage, sizeof(storage)) !=
        MOQ_OK)
        abort();
    for (size_t off = 0; off + ps <= whole; off += ps) {
        moq_mpeg2ts_packet_t p;
        moq_bytes_t raw = { pl + off, ps };
        if (moq_mpeg2ts_parse_packet(raw, ps, &p) != MOQ_OK) continue;
        if (p.payload.len > 184) abort();
        if (p.has_pcr && p.pcr_ext >= 300) abort();
        moq_mpeg2ts_pes_header_t h;
        (void)moq_mpeg2ts_parse_pes_header(p.payload, &h);
        if (moq_mpeg2ts_section_buf_push(&sb, &p) != MOQ_OK) continue;
        moq_bytes_t sec;
        while (moq_mpeg2ts_section_buf_next(&sb, &sec) == MOQ_OK) {
            check_section(sec);
            moq_mpeg2ts_pat_t pt;
            moq_mpeg2ts_pat_init(&pt, progs, 8);
            (void)moq_mpeg2ts_parse_pat(sec, &pt);
            moq_mpeg2ts_pmt_t pm;
            moq_mpeg2ts_pmt_init(&pm, streams, 8);
            if (moq_mpeg2ts_parse_pmt(sec, &pm) == MOQ_OK) {
                uint16_t keep[2] = { 0x0101, 0x0102 };
                uint8_t out[MOQ_MPEG2TS_SECTION_MAX];
                size_t on = 0;
                if (moq_mpeg2ts_pmt_subset(sec, keep, 2, out, sizeof(out), &on) ==
                    MOQ_OK) {
                    /* The rewritten section parses: its CRC_32 checks. */
                    moq_mpeg2ts_pmt_t sub;
                    moq_mpeg2ts_pmt_init(&sub, NULL, 0);
                    moq_result_t src = moq_mpeg2ts_parse_pmt(
                        (moq_bytes_t){ out, on }, &sub);
                    if (src != MOQ_OK && src != MOQ_ERR_BUFFER) abort();
                }
            }
        }
    }

    /* Receiver over the payload cut into Objects, in order. */
    size_t split = (size_t)(ctl >> 2);
    size_t step = split ? split * ps : (n ? n : 1);
    moq_mpeg2ts_receiver_cfg_t rc;
    moq_mpeg2ts_receiver_cfg_init(&rc);
    rc.packet_size = ps;
    rc.es_packets = rules.es_packets;
    rc.es_pid = 0x0101;
    static moq_mpeg2ts_receiver_t rx;
    if (moq_mpeg2ts_receiver_init(&rx, sizeof(rx), &rc) == MOQ_OK) {
        uint64_t obj = 0;
        for (size_t off = 0; off < n; off += step, obj++) {
            size_t len = n - off < step ? n - off : step;
            moq_mpeg2ts_receive_result_t r;
            moq_bytes_t b = { pl + off, len };
            uint32_t flags = (obj % 3 == 2) ? MOQ_MPEG2TS_OBJ_LAST_IN_GROUP
                                            : MOQ_MPEG2TS_OBJ_SUBGROUP_ENDS_GROUP;
            moq_result_t prc = moq_mpeg2ts_receiver_push(&rx, obj / 3, obj % 3,
                                                         flags, b, &r, sizeof(r));
            if ((prc == MOQ_OK) != r.accepted) abort();
        }
    }

    /* The same Objects offered to a reorder window out of order: released
     * in ascending order, each exactly once. */
    {
        enum { SLOTS = 4, MAXOBJ = 64 };
        moq_mpeg2ts_reorder_slot_t slots[SLOTS];
        rc.reorder_slots = slots;
        rc.reorder_slot_count = SLOTS;
        rc.reorder_window_us = 50;
        if (moq_mpeg2ts_receiver_init(&rx, sizeof(rx), &rc) != MOQ_OK) abort();
        size_t count = 0;
        for (size_t off = 0; off < n && count < MAXOBJ; off += step) count++;
        uint64_t order[MAXOBJ];
        for (size_t i = 0; i < count; i++) order[i] = i;
        unsigned seed = (unsigned)(ctl >> 4);
        for (size_t i = 0; i + 1 < count; i += 2)
            if ((seed >> (i / 2 % 4)) & 1u) {
                uint64_t t = order[i];
                order[i] = order[i + 1];
                order[i + 1] = t;
            }
        bool delivered[MAXOBJ];
        memset(delivered, 0, sizeof(delivered));
        uint64_t last = 0;
        bool have_last = false;
        uint64_t now = 0;
        for (size_t i = 0; i <= count; i++) {
            moq_mpeg2ts_delivery_t d;
            if (i < count) {
                uint64_t k = order[i];
                size_t off = (size_t)k * step;
                size_t len = n - off < step ? n - off : step;
                moq_bytes_t b = { pl + off, len };
                moq_result_t orc;
                while ((orc = moq_mpeg2ts_receiver_offer(&rx, now, k / 3, k % 3,
                            MOQ_MPEG2TS_OBJ_SUBGROUP_ENDS_GROUP, b,
                            (void *)(uintptr_t)(k + 1))) == MOQ_ERR_BUFFER) {
                    if (moq_mpeg2ts_receiver_poll(&rx, now, &d, sizeof(d)) != MOQ_OK) abort();
                    uint64_t id = (uint64_t)(uintptr_t)d.ctx - 1;
                    if (id >= count || delivered[id]) abort();
                    if (have_last && id <= last) abort();
                    delivered[id] = true;
                    last = id;
                    have_last = true;
                }
                if (orc != MOQ_OK && orc != MOQ_ERR_PROTO) abort();
                if (orc == MOQ_ERR_PROTO) delivered[k] = true;   /* late */
            } else {
                moq_mpeg2ts_receiver_finish(&rx);
            }
            now += 10;
            while (moq_mpeg2ts_receiver_poll(&rx, now, &d, sizeof(d)) == MOQ_OK) {
                uint64_t id = (uint64_t)(uintptr_t)d.ctx - 1;
                if (id >= count || delivered[id]) abort();
                if (have_last && id <= last) abort();
                delivered[id] = true;
                last = id;
                have_last = true;
            }
        }
        for (size_t i = 0; i < count; i++)
            if (!delivered[i]) abort();
        if (moq_mpeg2ts_receiver_next_deadline_us(&rx) != UINT64_MAX) abort();
    }

    /* Publisher and subscriber helpers that rewrite packets work on a copy. */
    if (whole > 0) {
        uint8_t *copy = malloc(whole);
        if (!copy) return 0;
        memcpy(copy, pl, whole);

        moq_mpeg2ts_program_filter_t *f = malloc(sizeof(*f));
        moq_mpeg2ts_program_filter_cfg_t fc;
        moq_mpeg2ts_program_filter_cfg_init(&fc);
        fc.packet_size = ps;
        fc.program_number = 1;
        if (f && moq_mpeg2ts_program_filter_init(f, sizeof(*f), &fc) == MOQ_OK) {
            uint8_t out[192];
            for (size_t off = 0; off < whole; off += ps) {
                moq_mpeg2ts_filter_action_t act;
                moq_bytes_t b = { copy + off, ps };
                if (moq_mpeg2ts_program_filter_push(f, b, out, sizeof(out), &act) !=
                    MOQ_OK)
                    continue;
                /* A passed packet is the input; a PAT is one valid section
                 * listing program 1 only. */
                if (act == MOQ_MPEG2TS_FILTER_PASS && memcmp(out, b.data, ps) != 0)
                    abort();
                if (act == MOQ_MPEG2TS_FILTER_PAT) {
                    const uint8_t *s = out + off_ts + 5;
                    moq_mpeg2ts_program_t one[2];
                    moq_mpeg2ts_pat_t pt;
                    moq_mpeg2ts_pat_init(&pt, one, 2);
                    if (moq_mpeg2ts_parse_pat((moq_bytes_t){ s, 16 }, &pt) != MOQ_OK ||
                        pt.program_count != 1 || one[0].program_number != 1)
                        abort();
                }
            }
        }
        free(f);

        /* Switcher: a refused packet is unchanged; an accepted one keeps
         * its sync byte, PID and continuity counter. */
        static moq_mpeg2ts_switcher_t sw;
        if (moq_mpeg2ts_switcher_init(&sw, sizeof(sw), ps) == MOQ_OK) {
            const uint16_t hint[] = { 0x0100 };
            uint8_t before[192];
            for (size_t off = 0; off < whole; off += ps) {
                if (off == (whole / ps / 2) * ps)
                    moq_mpeg2ts_switcher_switch(&sw, (ctl & 4u) ? hint : NULL,
                                                (ctl & 4u) ? 1 : 0);
                memcpy(before, copy + off, ps);
                moq_result_t src = moq_mpeg2ts_switcher_process(&sw, copy + off);
                if (src != MOQ_OK && memcmp(before, copy + off, ps) != 0) abort();
                const uint8_t *a = before + off_ts, *b = copy + off + off_ts;
                if (a[0] != b[0] || a[1] != b[1] || a[2] != b[2] || a[3] != b[3])
                    abort();
            }
        }

        size_t half = (whole / ps / 2) * ps;
        if (half > 0)
            (void)moq_mpeg2ts_align_join_psi(copy, half, ps,
                                             (moq_bytes_t){ copy + half, whole - half });

        uint8_t obj_buf[192 * 4], lead_buf[192 * 4];
        moq_mpeg2ts_packetizer_cfg_t pc;
        moq_mpeg2ts_packetizer_cfg_init(&pc);
        pc.packet_size = ps;
        pc.object_packets = 1u + (uint32_t)((ctl >> 4) & 3u);
        pc.object_buf = obj_buf;
        pc.object_cap = sizeof(obj_buf);
        pc.lead_buf = lead_buf;
        pc.lead_cap = sizeof(lead_buf);
        pc.wait_for_random_access = (ctl & 4u) != 0;
        pc.min_group_ms = (ctl & 8u) ? 100u : 0u;
        pc.random_access_pid = (ctl & 64u) ? MOQ_MPEG2TS_PID_NONE
                                           : MOQ_MPEG2TS_PID_AUTO;
        emit_state_t es = { 0, ps, pc.object_packets, pc.wait_for_random_access };
        pc.emit = on_emit;
        pc.ctx = &es;
        static moq_mpeg2ts_packetizer_t pz;
        if (moq_mpeg2ts_packetizer_init(&pz, sizeof(pz), &pc) == MOQ_OK) {
            size_t taken = 0;
            for (size_t off = 0; off < whole; off += ps)
                if (moq_mpeg2ts_packetizer_push(&pz, (moq_bytes_t){ pl + off, ps }) == MOQ_OK)
                    taken++;
            (void)moq_mpeg2ts_packetizer_flush(&pz);
            moq_mpeg2ts_packetizer_stats_t st;
            if (moq_mpeg2ts_packetizer_get_stats(&pz, &st, sizeof(st)) != MOQ_OK) abort();
            /* Every taken packet is emitted or counted as dropped. */
            if (es.total / ps + st.packets_dropped_before_random_access != taken)
                abort();
        }

        if (ps == 192) {
            size_t on = 0;
            (void)moq_mpeg2ts_strip_prefix((moq_bytes_t){ copy, whole }, copy, whole, &on);
        }
        free(copy);
    }
    return 0;
}
