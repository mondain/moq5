#include <moq/mpeg2ts.hpp>
#ifdef MOQ_HAS_MSF
#include <moq/msf.hpp>
#endif
#include "test_support.hpp"

#include <array>
#include <cstring>
#include <utility>

// CRC_32 of PSI sections (ISO/IEC 13818-1 Annex A), computed bit by bit as
// an independent reference.
static uint32_t ref_crc32(moq::bytes_view data)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < data.size(); i++) {
        c ^= (uint32_t)data.data()[i] << 24;
        for (int b = 0; b < 8; b++)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
    }
    return c;
}

// The PAT packet of draft-gregoire-moq-msfts' initialization-data example.
static void spec_pat(uint8_t pkt[188])
{
    static const uint8_t head[] = {
        0x47, 0x40, 0x00, 0x10, 0x00, 0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1, 0x00,
        0x00, 0x00, 0x01, 0xE1, 0x00, 0xE8, 0xF9, 0x5E, 0x7D,
    };
    std::memset(pkt, 0xFF, 188);
    std::memcpy(pkt, head, sizeof(head));
}

// A video packet on PID 0x101 at a random access point, PES PTS 90000.
static void rap_packet(uint8_t pkt[188], uint8_t cc)
{
    std::memset(pkt, 0xFF, 188);
    pkt[0] = 0x47; pkt[1] = 0x41; pkt[2] = 0x01; pkt[3] = (uint8_t)(0x30 | cc);
    pkt[4] = 1;     // adaptation_field_length
    pkt[5] = 0x40;  // random_access_indicator
    const uint8_t pes[] = { 0, 0, 1, 0xE0, 0, 0, 0x80, 0x80, 5,
                            0x21, 0x00, 0x05, 0xBF, 0x21 };  // PTS 90000
    std::memcpy(pkt + 6, pes, sizeof(pes));
}

// A three-packet PAT (100 programs) on PID 0, continuity counters 0..2.
static void long_pat(uint8_t out[188 * 3])
{
    uint8_t sec[1024];
    size_t n = 8;
    sec[0] = 0x00; sec[1] = 0xB0; sec[2] = 0; sec[3] = 0; sec[4] = 1;
    sec[5] = 0xC1; sec[6] = 0; sec[7] = 0;
    for (uint16_t i = 0; i < 100; i++) {
        uint16_t num = (uint16_t)(i + 1), pid = (uint16_t)(0x100 + i);
        sec[n++] = (uint8_t)(num >> 8); sec[n++] = (uint8_t)num;
        sec[n++] = (uint8_t)(0xE0 | (pid >> 8)); sec[n++] = (uint8_t)pid;
    }
    size_t sl = n - 3 + 4;
    sec[1] = (uint8_t)(0xB0 | (sl >> 8)); sec[2] = (uint8_t)sl;
    uint32_t crc = ref_crc32(moq::bytes_view(sec, n));
    sec[n++] = (uint8_t)(crc >> 24); sec[n++] = (uint8_t)(crc >> 16);
    sec[n++] = (uint8_t)(crc >> 8);  sec[n++] = (uint8_t)crc;
    uint8_t cc = 0;
    size_t len = 0;
    moq_mpeg2ts_packetize_section(0, &cc, moq_bytes_t{ sec, n }, 188, out,
                                  188 * 3, &len);
}

// A filter that has collected part of a section, relocated by the result
// factory, a move and a copy, completes it in its new place.
static moq::mpeg2ts::program_filter filter_after_first_packet(const uint8_t *pk)
{
    auto f = moq::mpeg2ts::program_filter::make(1);
    std::array<uint8_t, 188> out{};
    (void)f->push(moq::bytes_view(pk, 188), out);
    return std::move(f.value());
}

int main()
{
    int failures = 0;

    uint8_t pat[188];
    spec_pat(pat);

    // CRC and packet parse.
    MOQ_CHECK(ref_crc32(moq::bytes_view(pat + 5, 16)) == 0);
    {
        auto p = moq::mpeg2ts::parse_packet(moq::bytes_view(pat, 188));
        MOQ_CHECK(p.ok());
        MOQ_CHECK(p->pid == 0 && p->payload_unit_start);
        auto bad = moq::mpeg2ts::parse_packet(moq::bytes_view(pat, 100));
        MOQ_CHECK(!bad.ok() && bad.error().code() == moq::errc::invalid);
    }

    // PAT.
    {
        std::array<moq::mpeg2ts::program, 4> progs{};
        auto r = moq::mpeg2ts::parse_pat(moq::bytes_view(pat + 5, 16), progs);
        MOQ_CHECK(r.ok());
        MOQ_CHECK(r->transport_stream_id == 1);
        MOQ_CHECK(r->programs.size() == 1);
        MOQ_CHECK(r->programs[0].program_number == 1);
        MOQ_CHECK(r->programs[0].pmt_pid == 0x0100);
        std::array<moq::mpeg2ts::program, 0> none{};
        size_t need = 0;
        auto s = moq::mpeg2ts::parse_pat(moq::bytes_view(pat + 5, 16), none, &need);
        MOQ_CHECK(!s.ok() && need == 1);
    }

    // Object validation: valid, partial, es-packets foreign PID.
    uint8_t obj[188 * 2];
    std::memcpy(obj, pat, 188);
    rap_packet(obj + 188, 0);
    {
        auto v = moq::mpeg2ts::validate_object(moq::bytes_view(obj, sizeof(obj)));
        MOQ_CHECK(v.ok() && v->reason == moq::mpeg2ts::validity::ok && v->packet_count == 2);
        auto part = moq::mpeg2ts::validate_object(moq::bytes_view(obj, 200));
        MOQ_CHECK(part.ok() &&
                  part->reason == moq::mpeg2ts::validity::partial_packet);
        auto es = moq::mpeg2ts::validate_object(moq::bytes_view(obj, sizeof(obj)),
                                                188, uint16_t{0x0101});
        MOQ_CHECK(es.ok() &&
                  es->reason == moq::mpeg2ts::validity::foreign_pid &&
                  es->bad_packet == 0);
        auto arg = moq::mpeg2ts::validate_object(moq::bytes_view(obj, sizeof(obj)), 190);
        MOQ_CHECK(!arg.ok());
    }

    // Inspection: PAT seen, RAP and PTS found.
    {
        auto info = moq::mpeg2ts::inspect_object(moq::bytes_view(obj, sizeof(obj)));
        MOQ_CHECK(info.ok());
        MOQ_CHECK(info->has_pat && !info->psi_complete);
        MOQ_CHECK(info->has_random_access && info->random_access_index == 1);
        MOQ_CHECK(info->has_pts && info->pts == 90000);
        MOQ_CHECK(info->timing_pid == 0x0101);
    }

    // Receiver: a gap is located and the payload is never changed.
    {
        auto rx = moq::mpeg2ts::receiver::make();
        MOQ_CHECK(rx.ok());
        auto r0 = rx->push(0, 0, 0, moq::bytes_view(obj, sizeof(obj)));
        MOQ_CHECK(r0.accepted && !r0.gap);
        auto r2 = rx->push(0, 2, 0, moq::bytes_view(obj, sizeof(obj)));
        MOQ_CHECK(r2.accepted && r2.gap && r2.gap_group == 0 && r2.gap_object == 1);
        auto bad = rx->push(0, 3, 0, moq::bytes_view(obj, 10));
        MOQ_CHECK(!bad.accepted);
        MOQ_CHECK(!moq::mpeg2ts::receiver::make(190).ok());
    }

    // Receiver with a reorder window, moved after Objects are held.
    {
        moq::mpeg2ts::receiver_options o;
        o.reorder_slots = 4;
        o.reorder_window_us = 1000;
        auto made = moq::mpeg2ts::receiver::make(o);
        MOQ_CHECK(made.ok());
        moq::mpeg2ts::receiver rx = std::move(made.value());
        const uint32_t eog = moq::mpeg2ts::obj_subgroup_ends_group;
        MOQ_CHECK(rx.offer(0, 1, 0, eog, moq::bytes_view(obj, sizeof(obj))).ok());
        MOQ_CHECK(rx.offer(0, 0, 0, eog, moq::bytes_view(obj, sizeof(obj))).ok());
        moq::mpeg2ts::receiver moved = std::move(rx);
        auto d0 = moved.poll(0);
        MOQ_CHECK(d0 && d0->group == 0 && d0->result.accepted);
        MOQ_CHECK(d0 && d0->struct_size == sizeof(moq::mpeg2ts::receiver::delivery) &&
                  d0->result.struct_size == sizeof(d0->result));
        MOQ_CHECK(!moved.poll(10));                   // (0, 1) could follow
        moved.end_group(0);
        auto d1 = moved.poll(10);
        MOQ_CHECK(d1 && d1->group == 1 && !d1->result.gap);
        MOQ_CHECK(moved.next_deadline_us() == UINT64_MAX);
    }

    // A moved-from receiver no longer refers to the reorder slots it gave
    // away: queries on it stay safe after the destination is destroyed
    // (move construction and move assignment).
    {
        moq::mpeg2ts::receiver_options o;
        o.reorder_slots = 4;
        auto made = moq::mpeg2ts::receiver::make(o);
        MOQ_CHECK(made.ok());
        auto source = std::move(made.value());
        {
            auto destination = std::move(source);
            MOQ_CHECK(destination.next_deadline_us() == UINT64_MAX);
        }
        MOQ_CHECK(source.next_deadline_us() == UINT64_MAX);
        MOQ_CHECK(!source.poll(0));
        MOQ_CHECK(!source.offer(0, 0, 0, 0, moq::bytes_view(obj, sizeof(obj))).ok());

        auto made2 = moq::mpeg2ts::receiver::make(o);
        moq::mpeg2ts::receiver src2 = std::move(made2.value());
        MOQ_CHECK(src2.offer(0, 0, 1, 0, moq::bytes_view(obj, sizeof(obj))).ok());
        {
            moq::mpeg2ts::receiver dst2;
            dst2 = std::move(src2);
            MOQ_CHECK(dst2.next_deadline_us() == 0);
            dst2.finish();
            auto d = dst2.poll(0);
            MOQ_CHECK(d && d->object == 1);
        }
        MOQ_CHECK(src2.next_deadline_us() == UINT64_MAX);
        MOQ_CHECK(!src2.poll(0));
    }

    // The filter's state survives the factory, a move and a copy taken in the
    // middle of a section (ASan checks no access reaches a dead object).
    {
        uint8_t pk[188 * 3];
        long_pat(pk);
        moq::mpeg2ts::program_filter f = filter_after_first_packet(pk);
        std::array<uint8_t, 188> out{};
        moq::mpeg2ts::program_filter copy = f;
        moq::mpeg2ts::program_filter moved = std::move(f);
        for (auto *g : { &copy, &moved }) {
            auto a1 = g->push(moq::bytes_view(pk + 188, 188), out);
            MOQ_CHECK(a1.ok() && *a1 == MOQ_MPEG2TS_FILTER_DROP);
            auto a2 = g->push(moq::bytes_view(pk + 376, 188), out);
            MOQ_CHECK(a2.ok() && *a2 == MOQ_MPEG2TS_FILTER_PAT);
            MOQ_CHECK(ref_crc32(moq::bytes_view(out.data() + 5, 16)) == 0);
        }
    }

    // Program filter: the PAT is rewritten to one program.
    {
        auto f = moq::mpeg2ts::program_filter::make(1);
        MOQ_CHECK(f.ok());
        std::array<uint8_t, 188> out{};
        auto act = f->push(moq::bytes_view(pat, 188), out);
        MOQ_CHECK(act.ok() && *act == MOQ_MPEG2TS_FILTER_PAT);
        MOQ_CHECK(ref_crc32(moq::bytes_view(out.data() + 5, 16)) == 0);
        MOQ_CHECK(!f->program_gone());
        MOQ_CHECK(!moq::mpeg2ts::program_filter::make(0).ok());
    }

    // Switcher: the PAT after a switch gets a different version.
    {
        auto sw = moq::mpeg2ts::switcher::make();
        MOQ_CHECK(sw.ok());
        uint8_t a[188], b[188];
        spec_pat(a);
        spec_pat(b);
        MOQ_CHECK(sw->process(std::span<uint8_t>(a, 188)).ok());
        sw->on_switch();
        MOQ_CHECK(sw->process(std::span<uint8_t>(b, 188)).ok());
        MOQ_CHECK(((b[10] >> 1) & 0x1F) != ((a[10] >> 1) & 0x1F));
        MOQ_CHECK(ref_crc32(moq::bytes_view(b + 5, 16)) == 0);
        MOQ_CHECK(!sw->process(std::span<uint8_t>(b, 10)).ok());
    }

#ifdef MOQ_HAS_MSF
    // Catalog: a parsed mpeg2ts track, checked, and authored back.
    {
        const char json[] =
            "{\"version\":\"1\",\"tracks\":["
            "{\"name\":\"p1\",\"packaging\":\"mpeg2ts\",\"isLive\":true,"
            "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
            "\"mpeg2tsProgramNumber\":1,\"mpeg2tsSiPids\":[17]},"
            "{\"name\":\"bad\",\"packaging\":\"mpeg2ts\",\"isLive\":true,"
            "\"mpeg2tsMode\":\"es-units\"}]}";
        auto cat = moq::msf::parse(moq::bytes_view(json));
        MOQ_CHECK(cat.ok());
        auto t = cat->track(0);
        MOQ_CHECK(t.mpeg2ts != nullptr);
        MOQ_CHECK(cat->track(0).mpeg2ts == t.mpeg2ts);   // borrowed, not a copy
        MOQ_CHECK(t.mpeg2ts->mode == MOQ_MSF_MPEG2TS_MODE_PER_PROGRAM);
        MOQ_CHECK(t.mpeg2ts->si_pid_count == 1 && t.mpeg2ts->si_pids[0] == 17);
        MOQ_CHECK(cat->mpeg2ts_check(0) == MOQ_MSF_MPEG2TS_OK);
        MOQ_CHECK(cat->mpeg2ts_check(1) == MOQ_MSF_MPEG2TS_ERR_MODE_UNRECOGNIZED);

        moq::msf::track tr;
        tr.name = moq::bytes_view("p1");
        tr.packaging = moq::bytes_view("mpeg2ts");
        tr.is_live = true;
        moq_msf_mpeg2ts_t m{};
        m.has_mode = true;
        m.mode = MOQ_MSF_MPEG2TS_MODE_UNMODIFIED_PROGRAM;
        m.has_packet_size = true;
        m.packet_size = 192;
        m.has_timestamp_mode = true;
        m.timestamp_mode = MOQ_MSF_MPEG2TS_TIMESTAMP_ARRIVAL_TIME;
        tr.mpeg2ts = m;
        moq::msf::catalog c;
        c.tracks = &tr;
        c.track_count = 1;
        auto enc = moq::msf::encode(c);
        MOQ_CHECK(enc.ok());
        if (enc.ok()) {
            auto back = moq::msf::parse(moq::bytes_view(enc->data(), enc->size()));
            MOQ_CHECK(back.ok());
            MOQ_CHECK(back->mpeg2ts_check(0) == MOQ_MSF_MPEG2TS_OK);
            MOQ_CHECK(back->track(0).mpeg2ts->packet_size == 192);
        }
        tr.mpeg2ts->packet_size = 188;     // timestamp mode at 188: refused
        MOQ_CHECK(!moq::msf::encode(c).ok());
        tr.mpeg2ts.reset();                // an mpeg2ts track needs its fields
        MOQ_CHECK(!moq::msf::encode(c).ok());
        auto other = cat->track(1);        // parsed, recorded as unrecognized
        MOQ_CHECK(other.mpeg2ts && other.mpeg2ts->mode == MOQ_MSF_MPEG2TS_MODE_UNRECOGNIZED);
    }
#endif

    MOQ_PASS("cpp_mpeg2ts");
    return failures == 0 ? 0 : 1;
}
