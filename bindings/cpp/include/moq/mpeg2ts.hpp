#ifndef MOQ_MPEG2TS_HPP
#define MOQ_MPEG2TS_HPP

// Header-only C++ view of <moq/mpeg2ts.h>: MPEG-2 TS packaging helpers for
// the MSF "mpeg2ts" packaging (draft-gregoire-moq-msfts). Every bytes_view
// result borrows from the caller's input. The only allocation is a
// receiver's reorder window, when receiver_options asks for one.

#include <moq/mpeg2ts.h>
#include <moq/result.hpp>
#include <moq/types.hpp>

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

namespace moq::mpeg2ts {

inline constexpr uint32_t ts_packet_size   = MOQ_MPEG2TS_TS_PACKET_SIZE;
inline constexpr uint32_t m2ts_packet_size = MOQ_MPEG2TS_M2TS_PACKET_SIZE;
inline constexpr uint16_t pid_pat          = MOQ_MPEG2TS_PID_PAT;
inline constexpr uint16_t pid_null         = MOQ_MPEG2TS_PID_NULL;
inline constexpr uint16_t pid_none         = MOQ_MPEG2TS_PID_NONE;
inline constexpr uint16_t pid_auto         = MOQ_MPEG2TS_PID_AUTO;

/// What the subscription says about an Object's place in its Group.
inline constexpr uint32_t obj_subgroup_ends_group = MOQ_MPEG2TS_OBJ_SUBGROUP_ENDS_GROUP;
inline constexpr uint32_t obj_last_in_group       = MOQ_MPEG2TS_OBJ_LAST_IN_GROUP;

enum class validity {
    ok             = MOQ_MPEG2TS_OK,
    empty          = MOQ_MPEG2TS_ERR_EMPTY,
    partial_packet = MOQ_MPEG2TS_ERR_PARTIAL_PACKET,
    sync_byte      = MOQ_MPEG2TS_ERR_SYNC_BYTE,
    null_packet    = MOQ_MPEG2TS_ERR_NULL_PACKET,
    foreign_pid    = MOQ_MPEG2TS_ERR_FOREIGN_PID,
};

using packet      = moq_mpeg2ts_packet_t;
using pes_header  = moq_mpeg2ts_pes_header_t;
using program     = moq_mpeg2ts_program_t;
using stream      = moq_mpeg2ts_stream_t;
using object_info = moq_mpeg2ts_object_info_t;

inline uint64_t pcr_delta(uint64_t from, uint64_t to) noexcept
{
    return moq_mpeg2ts_pcr_delta(from, to);
}

inline uint64_t pts_delta(uint64_t from, uint64_t to) noexcept
{
    return moq_mpeg2ts_pts_delta(from, to);
}

/// Parse one source packet (188 or 192 octets) from the start of `src`.
inline result<packet> parse_packet(bytes_view src,
                                   uint32_t packet_size = ts_packet_size)
{
    packet p;
    moq_mpeg2ts_packet_init(&p);
    moq_result_t rc = moq_mpeg2ts_parse_packet(src.raw(), packet_size, &p);
    if (rc < 0) return errc_from_result(rc);
    return p;
}

inline result<pes_header> parse_pes_header(bytes_view pes_start)
{
    pes_header h{};
    moq_result_t rc = moq_mpeg2ts_parse_pes_header(pes_start.raw(), &h);
    if (rc < 0) return errc_from_result(rc);
    return h;
}

struct pat_info {
    uint16_t transport_stream_id = 0;
    uint8_t  version = 0;
    std::optional<uint16_t> network_pid;
    std::span<const program> programs;
};

/// Parse a PAT section into caller storage. errc::buffer_too_small when the
/// storage is short; *required (if given) is then the program count needed.
inline result<pat_info> parse_pat(bytes_view section, std::span<program> storage,
                                  size_t *required = nullptr)
{
    moq_mpeg2ts_pat_t pat;
    moq_mpeg2ts_pat_init(&pat, storage.data(), storage.size());
    moq_result_t rc = moq_mpeg2ts_parse_pat(section.raw(), &pat);
    if (required) *required = pat.program_count;
    if (rc < 0) return errc_from_result(rc);
    pat_info out;
    out.transport_stream_id = pat.transport_stream_id;
    out.version = pat.version;
    if (pat.has_network_pid) out.network_pid = pat.network_pid;
    out.programs = std::span<const program>(storage.data(), pat.program_count);
    return out;
}

struct pmt_info {
    uint16_t   program_number = 0;
    uint8_t    version = 0;
    uint16_t   pcr_pid = 0;
    bytes_view program_descriptors;
    std::span<const stream> streams;
};

inline result<pmt_info> parse_pmt(bytes_view section, std::span<stream> storage,
                                  size_t *required = nullptr)
{
    moq_mpeg2ts_pmt_t pmt;
    moq_mpeg2ts_pmt_init(&pmt, storage.data(), storage.size());
    moq_result_t rc = moq_mpeg2ts_parse_pmt(section.raw(), &pmt);
    if (required) *required = pmt.stream_count;
    if (rc < 0) return errc_from_result(rc);
    pmt_info out;
    out.program_number = pmt.program_number;
    out.version = pmt.version;
    out.pcr_pid = pmt.pcr_pid;
    out.program_descriptors = bytes_view(pmt.program_descriptors);
    out.streams = std::span<const stream>(storage.data(), pmt.stream_count);
    return out;
}

struct object_report {
    validity reason = validity::ok;
    size_t   packet_count = 0;
    size_t   bad_packet = 0;
};

/// Check one Object payload against the Object Payload Format rules. An
/// invalid payload is a successful result whose reason is not
/// validity::ok; only bad arguments are errors.
inline result<object_report> validate_object(
    bytes_view payload, uint32_t packet_size = ts_packet_size,
    std::optional<uint16_t> es_pid = std::nullopt)
{
    moq_mpeg2ts_object_rules_t rules;
    moq_mpeg2ts_object_rules_init(&rules);
    rules.packet_size = packet_size;
    if (es_pid) {
        rules.es_packets = true;
        rules.es_pid = *es_pid;
    }
    moq_mpeg2ts_object_report_t rep;
    moq_mpeg2ts_object_report_init(&rep);
    moq_result_t rc = moq_mpeg2ts_validate_object(&rules, payload.raw(), &rep, sizeof(rep));
    if (rc == MOQ_ERR_INVAL) return errc_from_result(rc);
    object_report out;
    out.reason = static_cast<validity>(rep.reason);
    out.packet_count = rep.packet_count;
    out.bad_packet = rep.bad_packet;
    return out;
}

/// PID selectors take a PID, pid_none, or (timing and random access)
/// pid_auto for the program's video PID.
struct inspect_options {
    uint32_t packet_size = ts_packet_size;
    uint16_t pmt_pid = pid_none;
    uint16_t program_number = 0;
    uint16_t timing_pid = pid_none;
    uint16_t pcr_pid = pid_none;
    uint16_t random_access_pid = pid_none;
};

inline result<object_info> inspect_object(bytes_view payload,
                                          const inspect_options &opt = {})
{
    moq_mpeg2ts_inspect_cfg_t ip;
    moq_mpeg2ts_inspect_cfg_init(&ip);
    ip.packet_size = opt.packet_size;
    ip.pmt_pid = opt.pmt_pid;
    ip.program_number = opt.program_number;
    ip.timing_pid = opt.timing_pid;
    ip.pcr_pid = opt.pcr_pid;
    ip.random_access_pid = opt.random_access_pid;
    object_info info;
    moq_mpeg2ts_object_info_init(&info);
    moq_result_t rc = moq_mpeg2ts_inspect_object(&ip, payload.raw(), &info, sizeof(info));
    if (rc < 0) return errc_from_result(rc);
    return info;
}

struct receiver_options {
    uint32_t packet_size = ts_packet_size;
    std::optional<uint16_t> es_pid;          ///< es-packets mode
    uint16_t random_access_pid = pid_auto;
    uint16_t program_number = 0;             ///< 0: the PAT's first program
    size_t   reorder_slots = 0;              ///< 0: in-order push only
    uint64_t reorder_window_us = 0;
};

/// Subscriber-side reconstruction state (Object Payload Format, gaps), with
/// an optional reorder window. Movable, not copyable: the C state refers to
/// the window's slots, which the object owns. A move hands both to the
/// destination and leaves the source uninitialized, so calls on a moved-from
/// receiver are refused (next_deadline_us() is UINT64_MAX, poll() finds
/// nothing).
class receiver {
    moq_mpeg2ts_receiver_t rx_{};
    std::unique_ptr<moq_mpeg2ts_reorder_slot_t[]> slots_;

    void take(receiver &o) noexcept
    {
        rx_ = o.rx_;
        slots_ = std::move(o.slots_);
        o.rx_ = moq_mpeg2ts_receiver_t{};    // struct_size 0: not usable
    }

public:
    using push_result = moq_mpeg2ts_receive_result_t;
    using delivery = moq_mpeg2ts_delivery_t;

    receiver() = default;
    receiver(receiver &&o) noexcept { take(o); }
    receiver &operator=(receiver &&o) noexcept
    {
        if (this != &o) take(o);
        return *this;
    }
    receiver(const receiver &) = delete;
    receiver &operator=(const receiver &) = delete;

    static result<receiver> make(const receiver_options &o)
    {
        receiver r;
        moq_mpeg2ts_receiver_cfg_t cfg;
        moq_mpeg2ts_receiver_cfg_init(&cfg);
        cfg.packet_size = o.packet_size;
        if (o.es_pid) {
            cfg.es_packets = true;
            cfg.es_pid = *o.es_pid;
        }
        cfg.random_access_pid = o.random_access_pid;
        cfg.program_number = o.program_number;
        if (o.reorder_slots) {
            r.slots_.reset(new moq_mpeg2ts_reorder_slot_t[o.reorder_slots]());
            cfg.reorder_slots = r.slots_.get();
            cfg.reorder_slot_count = o.reorder_slots;
            cfg.reorder_window_us = o.reorder_window_us;
        }
        moq_result_t rc = moq_mpeg2ts_receiver_init(&r.rx_, sizeof(r.rx_), &cfg);
        if (rc < 0) return errc_from_result(rc);
        return r;
    }

    static result<receiver> make(uint32_t packet_size = ts_packet_size,
                                 std::optional<uint16_t> es_pid = std::nullopt)
    {
        receiver_options o;
        o.packet_size = packet_size;
        o.es_pid = es_pid;
        return make(o);
    }

    /// In-order accounting. Never fails on a bad Object: `accepted` is false
    /// and the reason set. flags: obj_subgroup_ends_group, obj_last_in_group.
    push_result push(uint64_t group, uint64_t object, uint32_t flags,
                     bytes_view payload)
    {
        push_result res;
        moq_mpeg2ts_receive_result_init(&res);
        (void)moq_mpeg2ts_receiver_push(&rx_, group, object, flags,
                                        payload.raw(), &res, sizeof(res));
        return res;
    }

    /// The Group's final Object is the largest one seen (subgroup FIN).
    void end_group(uint64_t group) noexcept
    {
        moq_mpeg2ts_receiver_end_group(&rx_, group);
    }

    /// Hold an Object in the reorder window; the payload must stay valid
    /// until poll() hands it back.
    result<void> offer(uint64_t now_us, uint64_t group, uint64_t object,
                       uint32_t flags, bytes_view payload, void *ctx = nullptr)
    {
        moq_result_t rc = moq_mpeg2ts_receiver_offer(&rx_, now_us, group, object,
                                                     flags, payload.raw(), ctx);
        if (rc < 0) return errc_from_result(rc);
        return {};
    }

    std::optional<delivery> poll(uint64_t now_us)
    {
        delivery d{};
        if (moq_mpeg2ts_receiver_poll(&rx_, now_us, &d, sizeof(d)) != MOQ_OK)
            return std::nullopt;
        return d;
    }

    uint64_t next_deadline_us() const noexcept
    {
        return moq_mpeg2ts_receiver_next_deadline_us(&rx_);
    }
    void finish() noexcept { moq_mpeg2ts_receiver_finish(&rx_); }
};

/// Per-program filter for the per-program mode. The C state holds no pointer
/// into itself, so copies and moves are independent filters.
class program_filter {
    moq_mpeg2ts_program_filter_t f_{};

public:
    static result<program_filter> make(uint16_t program_number,
                                       uint32_t packet_size = ts_packet_size,
                                       std::span<const uint16_t> si_pids = {},
                                       bool keep_null = false)
    {
        program_filter pf;
        moq_mpeg2ts_program_filter_cfg_t cfg;
        moq_mpeg2ts_program_filter_cfg_init(&cfg);
        cfg.packet_size = packet_size;
        cfg.program_number = program_number;
        cfg.keep_null = keep_null;
        cfg.si_pids = si_pids.data();
        cfg.si_pid_count = si_pids.size();
        moq_result_t rc = moq_mpeg2ts_program_filter_init(&pf.f_, sizeof(pf.f_), &cfg);
        if (rc < 0) return errc_from_result(rc);
        return pf;
    }

    /// Filter one packet into `out` (one packet of room). Returns the action.
    result<moq_mpeg2ts_filter_action_t> push(bytes_view in, std::span<uint8_t> out)
    {
        moq_mpeg2ts_filter_action_t act = MOQ_MPEG2TS_FILTER_DROP;
        moq_result_t rc = moq_mpeg2ts_program_filter_push(&f_, in.raw(), out.data(),
                                                          out.size(), &act);
        if (rc < 0) return errc_from_result(rc);
        return act;
    }

    bool program_gone() const noexcept
    {
        return moq_mpeg2ts_program_filter_program_gone(&f_);
    }
};

/// Output rewriting across switches between alternate tracks.
class switcher {
    moq_mpeg2ts_switcher_t sw_{};

public:
    static result<switcher> make(uint32_t packet_size = ts_packet_size)
    {
        switcher s;
        moq_result_t rc = moq_mpeg2ts_switcher_init(&s.sw_, sizeof(s.sw_), packet_size);
        if (rc < 0) return errc_from_result(rc);
        return s;
    }

    /// The output moves to another track; pmt_pids names its PMT PIDs when
    /// they are known (from the catalog initialization data, say).
    void on_switch(std::span<const uint16_t> pmt_pids = {}) noexcept
    {
        moq_mpeg2ts_switcher_switch(&sw_, pmt_pids.data(), pmt_pids.size());
    }

    /// Rewrite one packet in place.
    result<void> process(std::span<uint8_t> pkt)
    {
        moq_result_t rc = MOQ_ERR_INVAL;
        if (pkt.size() >= sw_._packet_size)
            rc = moq_mpeg2ts_switcher_process(&sw_, pkt.data());
        if (rc < 0) return errc_from_result(rc);
        return {};
    }

    moq_mpeg2ts_switcher_stats_t stats() const noexcept
    {
        moq_mpeg2ts_switcher_stats_t st{};
        (void)moq_mpeg2ts_switcher_get_stats(&sw_, &st, sizeof(st));
        return st;
    }
};

} // namespace moq::mpeg2ts

#endif // MOQ_MPEG2TS_HPP
