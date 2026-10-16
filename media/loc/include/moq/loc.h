#ifndef MOQ_LOC_H
#define MOQ_LOC_H

/*
 * LOC (Low Overhead Container) metadata parse/encode helpers.
 *
 * Parses and encodes LOC metadata carried in raw object property
 * bytes for the selected LOC profile. Each profile maps typed C
 * fields to the property IDs defined by that LOC draft version.
 *
 * Pure helper library: no sessions, no I/O, no threads. A
 * moq_version_t is taken by value to select the wire codec; no
 * session is touched.
 * Link against moq::loc (depends only on moq::core).
 *
 * Unknown well-formed properties are silently ignored.
 *
 * -- Transport version, LOC revision, and the KVP integer codec --------
 *
 * The caller explicitly selects the companion LOC profile, separately from
 * the transport version. Transport selects only the KVP integer encoding:
 *     MOQ_VERSION_DRAFT_16  section 1.4/1.4.2: fields marked (i) use the
 *       RFC 9000 QUIC variable-length integer (1/2/4/8 bytes, max 2^62-1);
 *     MOQ_VERSION_DRAFT_18  section 1.4.1/1.4.3 and
 *     MOQ_VERSION_DRAFT_22  section 8.3: fields marked (vi64) use MOQT's
 *       own encoding (1..9 bytes, max 2^64-1).
 *
 * The Delta Type, the odd-type Length, and the even-type value are all
 * encoded with that draft's integer. Values 0..63 encode identically in
 * both, so a codec confusion is invisible below 64 and produces wrong
 * bytes at or above it.
 *
 * Supported explicit pairs are LOC-01 with D16/18 and LOC-04 with D22.
 * There is no default: an unsupported or unknown
 * version -- including 0 -- FAILS CLOSED with MOQ_ERR_INVAL rather than
 * guessing a codec, and so does a revision paired with a version it is not
 * registered for (LOC-01 ids on draft 22, LOC-04 ids on draft 16 or 18).
 * A future draft becomes usable here only when its row is deliberately
 * registered in the table.
 */

#include <stddef.h>
#include <moq/types.h>
#include <moq/rcbuf.h>
#include <moq/session.h>

#ifdef __cplusplus
extern "C" {
#endif

/* -- LOC profile selection -------------------------------------------- */

typedef enum moq_loc_profile {
    MOQ_LOC_PROFILE_01 = 1,  /* draft-ietf-moq-loc-01: drafts 16 and 18 */
    MOQ_LOC_PROFILE_02 = 2,  /* draft-ietf-moq-loc-02; reserved, never selected */
    MOQ_LOC_PROFILE_04 = 4,  /* draft-ietf-moq-loc-04: draft 22 */
} moq_loc_profile_t;

/*
 * Compatibility selector retained pending migration of existing callers:
 * MOQ_LOC_PROFILE_01 for
 * drafts 16 and 18, MOQ_LOC_PROFILE_04 for draft 22, and 0 for a version
 * the table does not register (including 0). This is the single place
 * the compatibility mapping lives. This is not companion opt-in or proof that
 * an unconverted media/service caller supports D22. Pure function.
 */
MOQ_API moq_loc_profile_t moq_loc_profile_for_version(
    moq_version_t transport_version);

/*
 * The largest integer property value (timestamp, timescale) that the
 * integer encoding of transport_version can carry: UINT64_MAX on drafts
 * 18 and 22 (vi64), 2^62-1 on draft 16, and 0 for a version the table
 * does not register (including 0). Read from the same table
 * moq_loc_encode_metadata uses for a supported explicit pair. The legacy
 * encoder remains LOC01+D16/18 only. Pure function, no session touched.
 */
MOQ_API uint64_t moq_loc_value_max_for_version(
    moq_version_t transport_version);

/* -- Video Frame Marking (RFC 9626 §3.1) ------------------------------ */

typedef struct moq_loc_video_frame_marking {
    bool    start_of_frame;
    bool    end_of_frame;
    bool    independent;
    bool    discardable;
    bool    base_layer_sync;
    uint8_t temporal_id;  /* 0-7 */
    bool    has_layer_id;
    uint8_t layer_id;     /* 0-255; valid when has_layer_id */
} moq_loc_video_frame_marking_t;

/* -- Audio Level (RFC 6464 §3) ---------------------------------------- */
/*
 * LOC-01 property ID 0x06; LOC-04 property ID 0x0C. Both carry the RFC
 * 6464 byte (V bit + 7-bit level) in the low 8 bits of an even-type
 * integer value.
 */

typedef struct moq_loc_audio_level {
    bool    voice_activity;
    uint8_t level;  /* 0-127; 0 = loudest, 127 = silence */
} moq_loc_audio_level_t;

/* -- Parsed LOC headers ----------------------------------------------- */

typedef struct moq_loc_headers {
    uint32_t struct_size;

    bool     has_timestamp;
    uint64_t timestamp;  /* LOC-01 ID 0x02: microseconds since the Unix epoch. */ /* api-boundary-exempt */

    bool     has_timescale;
    uint64_t timescale;  /* Reserved legacy field; LOC-01 encode refuses it. */

    bool     has_video_frame_marking;
    moq_loc_video_frame_marking_t video_frame_marking; /* LOC-01 0x04 integer */

    bool     has_audio_level;
    moq_loc_audio_level_t audio_level;  /* LOC-01 0x06 */

    bool        has_video_config;
    moq_bytes_t video_config;  /* LOC-01 ID 0x0D; BORROWED from
                                * the input property bytes */

} moq_loc_headers_t;

/* Complete frozen legacy layout. New metadata never grows this record. */
#define MOQ_LOC_HEADERS_V0_SIZE \
    (offsetof(moq_loc_headers_t, video_config) + sizeof(moq_bytes_t))

/* Clears/stamps the complete frozen legacy record. */
MOQ_API void moq_loc_headers_init(moq_loc_headers_t *h);

/*
 * Parse LOC metadata from raw object property bytes.
 *
 * Empty input parses successfully with no fields set.
 * Unknown well-formed properties are ignored.
 * Duplicate known properties: last value wins.
 *
 * video_config.data is BORROWED from the input property bytes.
 *
 * Legacy LOC-01 on D16/18 only. New D22 callers use parse_metadata below.
 * The Key-Value-Pair integer codec is selected by transport_version.
 *
 * Returns MOQ_OK on success.
 * Returns MOQ_ERR_INVAL if out is NULL, data is NULL with len > 0,
 *   transport_version is not D16/18, or profile is not LOC-01. The version is
 *   checked even for an EMPTY property block: a caller that cannot name
 *   its codec is refused whether or not there is anything to decode.
 * Returns MOQ_ERR_PROTO on malformed data.
 */
MOQ_API moq_result_t moq_loc_parse(moq_version_t transport_version,
                                    moq_loc_profile_t profile,
                                    moq_bytes_t properties,
                                    moq_loc_headers_t *out);

/*
 * Encode LOC metadata into raw object property bytes.
 *
 * Fields are emitted in ascending property ID order. Allocates one
 * rcbuf via alloc.
 *
 * Legacy LOC-01 on D16/18 only. New D22 callers use encode_metadata below.
 * The Key-Value-Pair integer codec is selected by transport_version.
 * A value the selected codec cannot represent -- a timestamp beyond
 * what the draft-16 integer encoding reaches, say -- is MOQ_ERR_INVAL,
 * not a truncation.
 *
 * If no fields are set, returns MOQ_OK with *out_properties = NULL.
 * Returns MOQ_ERR_INVAL on invalid input (audio level > 127,
 *   temporal_id > 7, NULL alloc or out_properties, an unregistered
 *   transport_version, a profile other than the one the table selects
 *   for it, a value the selected codec cannot represent, or a field the
 *   selected revision has no id for: timescale on LOC-01).
 *   The version is checked BEFORE the field set, so a caller
 *   with nothing to encode is still refused an unknown codec.
 * Returns MOQ_ERR_NOMEM on allocation failure.
 */
MOQ_API moq_result_t moq_loc_encode(const moq_alloc_t *alloc,
                                     moq_version_t transport_version,
                                     moq_loc_profile_t profile,
                                     const moq_loc_headers_t *headers,
                                     moq_rcbuf_t **out_properties);

/* -- Lossless, explicitly selected companion metadata ---------------- */

#define MOQ_LOC_METADATA_HAS_TIMESTAMP    UINT32_C(0x01)
#define MOQ_LOC_METADATA_HAS_TIMESCALE    UINT32_C(0x02)
#define MOQ_LOC_METADATA_HAS_MARKING      UINT32_C(0x04)
#define MOQ_LOC_METADATA_HAS_AUDIO_LEVEL  UINT32_C(0x08)
#define MOQ_LOC_METADATA_HAS_VIDEO_CONFIG UINT32_C(0x10)
#define MOQ_LOC_METADATA_HAS_AUDIO_CONFIG UINT32_C(0x20)
#define MOQ_LOC_METADATA_PRESENT_MASK     UINT32_C(0x3f)

typedef struct moq_loc_metadata {
    uint32_t struct_size;
    uint32_t present;
    uint64_t timestamp;       /* Raw ticks; no clock conversion. */
    uint64_t timescale;       /* Raw units/second, including zero. */
    uint8_t marking_len;
    uint8_t marking[4];       /* Exact marking payload; no normalization. */
    uint8_t audio_level;      /* Raw V bit and seven-bit level. */
    uint8_t reserved[2];      /* Must be zero on encode. */
    moq_bytes_t video_config;
    moq_bytes_t audio_config;
} moq_loc_metadata_t;

/* Full frozen v0 extent; includes BOTH complete borrowed spans. */
#define MOQ_LOC_METADATA_V0_SIZE \
    (offsetof(moq_loc_metadata_t, audio_config) + sizeof(moq_bytes_t))

/* Clears/stamps min(size, library size); size < 4 writes nothing. */
MOQ_API void moq_loc_metadata_init_sized(moq_loc_metadata_t *out, size_t size);

/* Explicit LOC01+D16/18 or LOC04+D22; unknown/mismatched pairs are INVAL.
 * No input output-stamp is required. Size below the full v0 floor or invalid
 * arguments => INVAL. Malformed KVP/known values => PROTO. A known field
 * repeated in one block (outer or inner immutable lists) => UNSUPPORTED,
 * even for equal values. Unknown well-formed fields are skipped.
 * Success alone copies/stamps min(out_size, library size); any failure leaves
 * the entire caller output untouched. Configs borrow ORIGINAL property bytes,
 * including immutable inner offsets; their presence may have length zero.
 * The caller retains that backing for as long as either span is used.
 * LOC01 integer marking is represented as minimal big-endian payload bytes;
 * LOC04 preserves all 1..4 bytes verbatim. No owned resource or cleanup. */
MOQ_API moq_result_t moq_loc_parse_metadata(moq_version_t transport_version,
    moq_loc_profile_t profile, moq_bytes_t properties,
    moq_loc_metadata_t *out, size_t out_size);

/* Requires input struct_size >= full v0; reads only the covered library prefix.
 * Unknown present bits, reserved bytes, invalid spans/marking lengths or an
 * unrepresentable integer => INVAL, before allocation. LOC01 timescale/audio
 * config, or multi-byte marking with an unrepresentable leading zero, =>
 * UNSUPPORTED (no lossy fallback). Raw timestamp/scale zero/MAX remain metadata,
 * not a usable service-clock promise. Emits sorted mutable keys into one owned
 * rcbuf; does not concatenate/rewrite raw immutable blocks. On success only,
 * publishes *out_properties (NULL for no fields); NOMEM or any other refusal
 * leaves it untouched. Configs are borrowed for this call only. */
MOQ_API moq_result_t moq_loc_encode_metadata(const moq_alloc_t *alloc,
    moq_version_t transport_version, moq_loc_profile_t profile,
    const moq_loc_metadata_t *in, moq_rcbuf_t **out_properties);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_LOC_H */
