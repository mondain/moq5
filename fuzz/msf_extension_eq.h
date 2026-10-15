#ifndef MOQ_FUZZ_MSF_EXTENSION_EQ_H
#define MOQ_FUZZ_MSF_EXTENSION_EQ_H

#include <moq/msf.h>
#include <string.h>

static inline bool fuzz_msf_span_eq(moq_bytes_t a, moq_bytes_t b)
{
    return a.len == b.len &&
           (a.len == 0 || (a.data && b.data && memcmp(a.data, b.data, a.len) == 0));
}

/* Compare semantic fields, never padding, ownership pointers or inactive
 * values. Unknown enum spellings remain meaningful receive-path values. */
static inline bool fuzz_msf_ts_eq(const moq_msf_mpeg2ts_t *a,
                                  const moq_msf_mpeg2ts_t *b)
{
    if (!a || !b || a->struct_size < MOQ_MSF_MPEG2TS_V0_SIZE ||
        b->struct_size < MOQ_MSF_MPEG2TS_V0_SIZE || a->malformed != b->malformed)
        return false;
#define FUZZ_TS_FIELD(field) \
    if (a->has_##field != b->has_##field || \
        (a->has_##field && a->field != b->field)) return false
    FUZZ_TS_FIELD(mode);
    FUZZ_TS_FIELD(packet_size);
    FUZZ_TS_FIELD(es_pid);
    FUZZ_TS_FIELD(program_number);
    FUZZ_TS_FIELD(pcr_pid);
    FUZZ_TS_FIELD(mux_rate);
    FUZZ_TS_FIELD(random_access);
    FUZZ_TS_FIELD(timestamp_mode);
    FUZZ_TS_FIELD(scte35_pid);
#undef FUZZ_TS_FIELD
    if (a->has_mode && a->mode == MOQ_MSF_MPEG2TS_MODE_UNRECOGNIZED &&
        !fuzz_msf_span_eq(a->mode_name, b->mode_name)) return false;
    if (a->has_timestamp_mode &&
        a->timestamp_mode == MOQ_MSF_MPEG2TS_TIMESTAMP_UNRECOGNIZED &&
        !fuzz_msf_span_eq(a->timestamp_mode_name, b->timestamp_mode_name))
        return false;
    if (a->has_si_pids != b->has_si_pids) return false;
    if (a->has_si_pids) {
        if (a->si_pid_count != b->si_pid_count ||
            (a->si_pid_count && (!a->si_pids || !b->si_pids))) return false;
        for (size_t i = 0; i < a->si_pid_count; i++)
            if (a->si_pids[i] != b->si_pids[i]) return false;
    }
    return true;
}

/* Parsed/encoded lists contain at most one record per known kind. Preserve
 * list membership as well as the complete typed record, on every packaging. */
static inline bool fuzz_msf_extensions_eq(const moq_msf_track_t *a,
                                          const moq_msf_track_t *b)
{
    if (a->extension_count != b->extension_count ||
        (a->extension_count && (!a->extensions || !b->extensions))) return false;
    for (size_t i = 0; i < a->extension_count; i++) {
        const moq_msf_track_ext_t *x = &a->extensions[i];
        const moq_msf_track_ext_t *y = &b->extensions[i];
        if (x->kind != y->kind || x->kind != MOQ_MSF_EXT_MPEG2TS ||
            !fuzz_msf_ts_eq(x->u.mpeg2ts, y->u.mpeg2ts)) return false;
    }
    return true;
}

#endif
