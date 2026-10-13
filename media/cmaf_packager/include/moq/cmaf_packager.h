#ifndef MOQ_CMAF_PACKAGER_H
#define MOQ_CMAF_PACKAGER_H

#include <moq/types.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Clear, single-track CMAF writer. One instance belongs to one calling thread.
 * Caller owns catalog/initRef, MOQT group placement, switching alignment and
 * exact SAP1/2 declarations. A sync flag cannot distinguish those SAP types.
 * No sockets, clocks, group policy, codec inference or encryption. */
typedef enum moq_cmaf_sample_entry {
    MOQ_CMAF_ENTRY_UNSET = 0,
    MOQ_CMAF_ENTRY_AVC1 = 1,
    MOQ_CMAF_ENTRY_AVC3 = 2, /* recognized, currently UNSUPPORTED */
    MOQ_CMAF_ENTRY_HVC1 = 3,
    MOQ_CMAF_ENTRY_HEV1 = 4, /* recognized, currently UNSUPPORTED */
    MOQ_CMAF_ENTRY_MP4A = 5,
    MOQ_CMAF_ENTRY_AV01 = 6,
    MOQ_CMAF_ENTRY_OPUS = 7  /* writes the case-sensitive "Opus" box entry */
} moq_cmaf_sample_entry_t;

typedef enum moq_cmaf_sample_format {
    MOQ_CMAF_SAMPLE_UNSET = 0,
    MOQ_CMAF_SAMPLE_NAL_ANNEXB = 1,
    MOQ_CMAF_SAMPLE_NAL_LENGTH_PREFIXED = 2,
    MOQ_CMAF_SAMPLE_AAC_RAW = 3,
    MOQ_CMAF_SAMPLE_AAC_ADTS = 4,
    MOQ_CMAF_SAMPLE_AV1_OBU = 5,
    MOQ_CMAF_SAMPLE_OPUS_PACKET = 6
} moq_cmaf_sample_format_t;

typedef struct moq_cmaf_packager_cfg {
    uint32_t struct_size;
    moq_cmaf_sample_entry_t sample_entry;
    /* avcC/hvcC/ASC/av1C/dOps record payload, without box header. Copied.
     * Uses codec_signaling's structural validator, not decoder semantics.
     * AV1/Opus sample bytes are caller-asserted encoded access units/packets;
     * this writer does not validate their full codec syntax. */
    moq_bytes_t codec_config;
    moq_cmaf_sample_format_t sample_format;
    /* Video requires a nonzero timescale. AAC: 0 selects samplerate; another
     * value must equal samplerate. Opus: 0 or 48000 only. */
    uint32_t timescale;
    uint32_t track_id; /* 0 selects 1; UINT32_MAX is unrepresentable here */
    /* PTS/DTS timebase in seconds. 0/0 means track ticks. Conversion truncates
     * toward zero after optional rebasing. Both values must be present or zero;
     * den and timescale*num must fit INT32_MAX. Durations are already ticks. */
    uint32_t source_timebase_num;
    uint32_t source_timebase_den;
    bool rebase_timestamps; /* first successful DTS becomes origin */
    uint32_t width;  /* video: 1..65535 */
    uint32_t height; /* video: 1..65535 */
    uint32_t samplerate; /* AAC: 1..65535; Opus: exactly 48000 */
    uint32_t channel_count; /* audio: 1..65535 */
    uint32_t avg_bitrate; /* AAC esds average/max bitrate in bits/second */
} moq_cmaf_packager_cfg_t;

/* Frozen v0 meaningful prefix. Future fields must start beyond the complete
 * original sizeof, including trailing padding, on every supported ABI. */
#define MOQ_CMAF_PACKAGER_CFG_V0_SIZE \
    (offsetof(moq_cmaf_packager_cfg_t, avg_bitrate) + \
     sizeof(((moq_cmaf_packager_cfg_t *)0)->avg_bitrate))

/* Clear/stamp min(cfg_size,library size). NULL or fewer than four bytes is a
 * no-op; future storage is untouched. Entry and format default to UNSET. */
MOQ_API void moq_cmaf_packager_cfg_init_sized(moq_cmaf_packager_cfg_t *cfg,
                                            size_t cfg_size);
#define moq_cmaf_packager_cfg_init(cfg) \
    moq_cmaf_packager_cfg_init_sized((cfg), sizeof(*(cfg)))

/* Frozen leaf input: layout/array stride will NEVER grow. A richer sample
 * requires a distinct type and operation, not appended fields or struct_size. */
typedef struct moq_cmaf_packager_sample {
    moq_bytes_t data; /* one nonempty encoded unit; borrowed during write */
    int64_t pts;
    int64_t dts;
    uint32_t duration; /* REQUIRED nonzero duration in track ticks */
    bool keyframe; /* sync flag; caller asserts decoder independence */
} moq_cmaf_packager_sample_t;

typedef struct moq_cmaf_packager moq_cmaf_packager_t;

/* Copies allocator vtable and config bytes. NULL allocator selects default;
 * alloc/free required, realloc unused. Allocator ctx remains borrowed until
 * destroy. Clears non-NULL *out on EVERY failure, including NULL cfg.
 * Entry/format must be coherent. AVC3/HEV1 refuse UNSUPPORTED, never remap.
 * ADTS supports exactly one frame with a 7/9-byte header and one raw data block,
 * matching a canonical two-byte ASC (AOT1..4, indexed rate, channel config1..7,
 * zero GA flags). Other valid ASC/ADTS features return UNSUPPORTED. RAW AAC is
 * never sniffed or stripped. Codec records are structurally validated through
 * codec_signaling; unsupported validator capabilities propagate UNSUPPORTED.
 * AAC rate/channels must match configuration-established facts from private
 * inspection, not decoder-verified properties. Unsignaled implicit SBR is not
 * excluded. PCE, signaled SBR/PS, escaped AOT semantics and uninspected
 * extension tails remain unknown and are accepted for RAW, not guessed. Opus
 * channel_count must match dOps. This is not full decoder-config validation.
 * Limits: config <=1 MiB; no extended-size boxes or >28-bit descriptor lengths.
 * INVAL: args, cfg prefix, choices or scalar ranges. PROTO: malformed record.
 * UNSUPPORTED: recognized carriage/implementation limit. NOMEM: allocator only. */
MOQ_API moq_result_t moq_cmaf_packager_create(const moq_alloc_t *alloc,
                                            const moq_cmaf_packager_cfg_t *cfg,
                                            moq_cmaf_packager_t **out);
MOQ_API void moq_cmaf_packager_destroy(moq_cmaf_packager_t *p); /* NULL no-op */

/* Immutable borrowed ftyp+moov, valid until destroy; NULL p gives {NULL,0}. */
MOQ_API moq_bytes_t moq_cmaf_packager_init_segment(const moq_cmaf_packager_t *p);
MOQ_API uint32_t moq_cmaf_packager_timescale(const moq_cmaf_packager_t *p);

/* One moof+mdat chunk. Within a chunk converted DTS must equal previous DTS
 * plus previous duration. Across writes gaps are allowed via tfdt; overlap or
 * decrease refuses. CTS must fit int32; decode timeline must fit int64 and
 * start nonnegative. No timestamp/duration is silently rewritten or clamped.
 * Origin, next DTS and sequence commit only on success, including after NOMEM.
 * Samples/array must NOT alias ANY packager-owned buffer (including a previous
 * fragment or init segment); rejects INVAL before buffer mutation/allocation.
 * Success borrows internal output until the NEXT ATTEMPTED write, including a
 * failed write, or destroy. A failed attempt invalidates the previous borrow
 * even when bytes happen to remain. Clears non-NULL *out_fragment on failure.
 * Limits: <=65536 samples, converted payload <=64 MiB, signed trun data_offset,
 * uint32 boxes/count/sequence. Reject limits before narrowing/large allocation.
 * INVAL: args, zero duration, aliasing or unrepresentable/discontinuous timing.
 * PROTO: malformed NAL/ADTS or inline parameter sets in AVC1/HVC1 samples.
 * UNSUPPORTED: NAL width/size limit or unimplemented ADTS feature.
 * NOMEM: allocator failure only. No ownership transfer or partial output. */
MOQ_API moq_result_t moq_cmaf_packager_write(moq_cmaf_packager_t *p,
                                           const moq_cmaf_packager_sample_t *samples,
                                           size_t count, moq_bytes_t *out_fragment);

#ifdef __cplusplus
}
#endif
#endif /* MOQ_CMAF_PACKAGER_H */
