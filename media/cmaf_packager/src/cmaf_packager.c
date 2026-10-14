#include <moq/cmaf_packager.h>
#include <moq/codec_signaling.h>
#include "codec_asc_internal.h"
#include <limits.h>

#define PACKAGER_MAX_CONFIG (1024u * 1024u)
#define PACKAGER_MAX_PAYLOAD (64u * 1024u * 1024u)
#define PACKAGER_MAX_BUFFER (PACKAGER_MAX_PAYLOAD + 2u * 1024u * 1024u)
#define PACKAGER_MAX_SAMPLES 65536u

_Static_assert(MOQ_CMAF_PACKAGER_CFG_V0_SIZE <= sizeof(moq_cmaf_packager_cfg_t),
               "config v0 floor must fit");

#include <stddef.h>
#include <string.h>

/* -- FOURCC ---------------------------------------------------------- */

#define FOURCC(a,b,c,d) \
    (((uint32_t)(a)<<24)|((uint32_t)(b)<<16)|((uint32_t)(c)<<8)|(uint32_t)(d))

/* -- Growable output buffer ------------------------------------------ */

/*
 * Every put_* is a no-op once err is set, so a box-writing sequence needs one
 * check at the end instead of one per field. The buffer moves on a grow, so
 * box sizes are patched by offset rather than through a saved pointer.
 */
typedef struct {
    const moq_alloc_t *alloc;
    uint8_t           *p;
    size_t             len;
    size_t             cap;
    moq_result_t       err;
} buf_t;

static void buf_free(buf_t *b)
{
    if (b->p) {
        b->alloc->free(b->p, b->cap, b->alloc->ctx);
        b->p = NULL;
    }
    b->len = 0;
    b->cap = 0;
}

/* Grown by alloc+copy+free: moq_alloc_t only guarantees realloc for session
 * allocators, and a packager has no reason to demand one. */
static bool buf_reserve(buf_t *b, size_t extra)
{
    if (b->err) return false;
    if (extra > (size_t)-1 - b->len) {
        b->err = MOQ_ERR_UNSUPPORTED;
        return false;
    }
    size_t need = b->len + extra;
    if (need > PACKAGER_MAX_BUFFER) {
        b->err = MOQ_ERR_UNSUPPORTED;
        return false;
    }
    if (need <= b->cap) return true;

    size_t cap = b->cap ? b->cap : 1024;
    while (cap < need) {
        if (cap > (size_t)-1 / 2) {
            cap = need;
            break;
        }
        cap *= 2;
    }
    if (cap > PACKAGER_MAX_BUFFER) cap = PACKAGER_MAX_BUFFER;

    uint8_t *next = b->alloc->alloc(cap, b->alloc->ctx);
    if (!next) {
        b->err = MOQ_ERR_NOMEM;
        return false;
    }
    if (b->len) memcpy(next, b->p, b->len);
    if (b->p) b->alloc->free(b->p, b->cap, b->alloc->ctx);
    b->p = next;
    b->cap = cap;
    return true;
}

static uint8_t *buf_extend(buf_t *b, size_t n)
{
    if (!buf_reserve(b, n)) return NULL;
    uint8_t *at = b->p + b->len;
    b->len += n;
    return at;
}

static void put8(buf_t *b, uint8_t v)
{
    uint8_t *p = buf_extend(b, 1);
    if (p) p[0] = v;
}

static void put16(buf_t *b, uint16_t v)
{
    uint8_t *p = buf_extend(b, 2);
    if (p) { p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
}

static void put24(buf_t *b, uint32_t v)
{
    uint8_t *p = buf_extend(b, 3);
    if (p) {
        p[0] = (uint8_t)(v >> 16); p[1] = (uint8_t)(v >> 8); p[2] = (uint8_t)v;
    }
}

/* Big-endian store into an already-reserved slot, for the fields patched in
 * place once their value is known. */
static void poke32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t)(v >> 24); p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);  p[3] = (uint8_t)v;
}

static void put32(buf_t *b, uint32_t v)
{
    uint8_t *p = buf_extend(b, 4);
    if (p) poke32(p, v);
}

static void put64(buf_t *b, uint64_t v)
{
    put32(b, (uint32_t)(v >> 32));
    put32(b, (uint32_t)v);
}

static void put_bytes(buf_t *b, const void *src, size_t n)
{
    if (n == 0) return;
    uint8_t *p = buf_extend(b, n);
    if (p) memcpy(p, src, n);
}

static void put_zero(buf_t *b, size_t n)
{
    uint8_t *p = buf_extend(b, n);
    if (p) memset(p, 0, n);
}

/* Version byte followed by 24 flag bits: the FullBox preamble. */
static void put_fullbox(buf_t *b, uint8_t version, uint32_t flags)
{
    put8(b, version);
    put24(b, flags);
}

/* Length prefix of a NAL, at the avcC/hvcC-declared width. */
static void put_nal_length(buf_t *b, size_t len, uint8_t width)
{
    for (int shift = (width - 1) * 8; shift >= 0; shift -= 8)
        put8(b, (uint8_t)((len >> shift) & 0xff));
}

/* -- Box open/close -------------------------------------------------- */

/* Writes a placeholder size and the type; box_close() patches the size once
 * the children are in, so no box length is computed up front. */
static size_t box_open(buf_t *b, uint32_t type)
{
    size_t at = b->len;
    put32(b, 0);
    put32(b, type);
    return at;
}

static void box_close(buf_t *b, size_t at)
{
    if (b->err) return;
    size_t size = b->len - at;
    /* A 32-bit size covers everything here: the only box that can approach
     * the limit is mdat, and a 4 GiB access unit is not a real input. */
    if (size > 0xFFFFFFFFu) {
        b->err = MOQ_ERR_UNSUPPORTED;
        return;
    }
    poke32(b->p + at, (uint32_t)size);
}

/* -- MPEG-4 descriptor lengths --------------------------------------- */

/* Descriptor sizes use the 4-byte expandable form (0x80 continuation on the
 * first three bytes). Minimal encoding is legal too, but the 4-byte form is
 * what reference mp4 muxers emit. */
#define DESCR_LEN_BYTES 4

static void put_descr(buf_t *b, uint8_t tag, size_t payload_len)
{
    if (payload_len > 0x0fffffffu) { b->err = MOQ_ERR_UNSUPPORTED; return; }
    put8(b, tag);
    put8(b, (uint8_t)(0x80 | ((payload_len >> 21) & 0x7f)));
    put8(b, (uint8_t)(0x80 | ((payload_len >> 14) & 0x7f)));
    put8(b, (uint8_t)(0x80 | ((payload_len >> 7) & 0x7f)));
    put8(b, (uint8_t)(payload_len & 0x7f));
}

#define DESCR_HDR (1 + DESCR_LEN_BYTES)

/* -- Codec config inspection ----------------------------------------- */

/*
 * lengthSizeMinusOne sits in the low two bits of avcC[4] and hvcC[21]. Taken
 * from the caller's record, so the width used to prefix samples is by
 * construction the width the init segment declares.
 */
static moq_result_t nal_length_size_from_record(moq_cmaf_sample_entry_t kind,
                                                moq_bytes_t rec,
                                                uint8_t *out)
{
    size_t offset;
    size_t min_len;

    if (kind == MOQ_CMAF_ENTRY_AVC1) {
        offset = 4;
        min_len = 7;
    } else {
        offset = 21;
        min_len = 23;
    }
    if (rec.len < min_len) return MOQ_ERR_PROTO;

    *out = (uint8_t)((rec.data[offset] & 0x03) + 1);
    return MOQ_OK;
}

/* -- Annex B --------------------------------------------------------- */

/*
 * Offset of the next start code at or after `from`, or n when there is none.
 * *sc_len gets 3 or 4: a zero byte in front of 00 00 01 belongs to the start
 * code, not to the NAL, but only if it lies inside the region being scanned.
 */
static size_t next_start_code(const uint8_t *d, size_t n, size_t from,
                              size_t *sc_len)
{
    if (from > n || n - from < 3) return n;
    for (size_t i = from; i <= n - 3; i++) {
        if (d[i] == 0 && d[i + 1] == 0 && d[i + 2] == 1) {
            if (i > from && d[i - 1] == 0) {
                *sc_len = 4;
                return i - 1;
            }
            *sc_len = 3;
            return i;
        }
    }
    return n;
}

/* Sample framing, shared by the allocation-free preflight and append pass.
 * AVC1/HVC1 require out-of-band parameter sets; never silently change carriage. */
static moq_result_t nal_payload(const uint8_t *data, size_t len, uint8_t width,
                                 moq_cmaf_sample_entry_t kind)
{
    if (!len || (kind == MOQ_CMAF_ENTRY_HVC1 && len < 2)) return MOQ_ERR_PROTO;
    uint64_t max_len = width == 4 ? UINT32_MAX : ((uint64_t)1 << (width * 8)) - 1;
    if (len > max_len) return MOQ_ERR_UNSUPPORTED;
    uint8_t type = kind == MOQ_CMAF_ENTRY_AVC1 ? data[0] & 0x1f
                                               : (data[0] >> 1) & 0x3f;
    if (kind == MOQ_CMAF_ENTRY_AVC1 ? type == 7 || type == 8 || type == 13
                                   : type >= 32 && type <= 34)
        return MOQ_ERR_PROTO;
    return MOQ_OK;
}

static moq_result_t convert_nals(buf_t *out, moq_bytes_t s, uint8_t width,
                                 moq_cmaf_sample_entry_t kind, bool annexb,
                                 size_t *written)
{
    size_t pos = 0, total = 0, count = 0;
    if (annexb) {
        size_t sc = 0;
        pos = next_start_code(s.data, s.len, 0, &sc);
        if (pos == s.len) return MOQ_ERR_PROTO;
        for (size_t i = 0; i < pos; i++)
            if (s.data[i] != 0) return MOQ_ERR_PROTO;
        pos += sc;
    }
    while (pos < s.len) {
        size_t next = s.len, next_sc = 0, len;
        if (annexb) {
            next = next_start_code(s.data, s.len, pos, &next_sc);
            size_t end = next;
            while (end > pos && s.data[end - 1] == 0) end--;
            len = end - pos;
        } else {
            if (s.len - pos < width) return MOQ_ERR_PROTO;
            len = 0;
            for (uint8_t i = 0; i < width; i++) len = (len << 8) | s.data[pos + i];
            pos += width;
            if (len > s.len - pos) return MOQ_ERR_PROTO;
        }
        moq_result_t rc = nal_payload(s.data + pos, len, width, kind);
        if (rc < 0) return rc;
        if (len > PACKAGER_MAX_PAYLOAD - width ||
            total > PACKAGER_MAX_PAYLOAD - width - len) return MOQ_ERR_UNSUPPORTED;
        total += width + len;
        if (out) {
            put_nal_length(out, len, width);
            put_bytes(out, s.data + pos, len);
            if (out->err) return out->err;
        }
        count++;
        if (annexb) {
            if (next != s.len && next + next_sc == s.len) return MOQ_ERR_PROTO;
            pos = next == s.len ? s.len : next + next_sc;
        } else pos += len;
    }
    if (!count) return MOQ_ERR_PROTO;
    *written = total;
    return MOQ_OK;
}

/* -- Timestamp rescaling --------------------------------------------- */

/* Every num/den pair reaching rescale() is bounded to this at create, which
 * keeps the remainder term below 2^62. */
#define RESCALE_MAX 0x7fffffffLL

/*
 * v * num / den, exact and without a 128-bit intermediate: split v into
 * quotient and remainder first. Truncation matches C division for negative v
 * because quotient and remainder carry the same sign. False when the result
 * would not fit, rather than overflowing signed arithmetic.
 */
static bool rescale(int64_t v, int64_t num, int64_t den, int64_t *out)
{
    if (den <= 0 || num <= 0 || num == den) {
        *out = v;
        return true;
    }

    const int64_t q = v / den;
    const int64_t r = v % den;
    if (q > INT64_MAX / num || q < INT64_MIN / num)
        return false;
    int64_t whole = q * num, fraction = (r * num) / den;
    if (fraction > 0 ? whole > INT64_MAX - fraction
                     : whole < INT64_MIN - fraction)
        return false;
    *out = whole + fraction;
    return true;
}

/* a - b, false when the difference does not fit. */
static bool sub_checked(int64_t a, int64_t b, int64_t *out)
{
    if (b < 0 ? a > INT64_MAX + b : a < INT64_MIN + b)
        return false;
    *out = a - b;
    return true;
}

/* -- Packager -------------------------------------------------------- */

struct moq_cmaf_packager {
    moq_alloc_t              alloc;

    moq_cmaf_sample_entry_t    codec_kind;
    moq_cmaf_sample_format_t sample_format;
    bool                     is_video;

    uint32_t                 timescale;
    uint32_t                 track_id;
    int64_t                  next_dts;
    uint32_t                 avg_bitrate;
    uint8_t                  nal_length_size;

    int64_t                  src_num;  /* source timebase -> timescale */
    int64_t                  src_den;

    bool                     rebase;
    bool                     have_origin;
    int64_t                  origin;   /* in source timebase units */

    uint32_t                 sequence_number;

    uint8_t                 *codec_config;
    size_t                   codec_config_len;

    buf_t                    init;     /* ftyp+moov, built once */
    buf_t                    frag;     /* moof+mdat, rebuilt per write */
    buf_t                    mdat;     /* converted samples for this write */
};

/* ISO-BMFF sample_flags: sample_depends_on in bits 24-25,
 * sample_is_non_sync_sample in bit 16. */
#define SAMPLE_FLAGS_SYNC     0x02000000u /* depends_on = 2 (independent) */
#define SAMPLE_FLAGS_NON_SYNC 0x01010000u /* depends_on = 1, non_sync = 1 */

static uint32_t sample_entry_type(moq_cmaf_sample_entry_t kind)
{
    switch (kind) {
    case MOQ_CMAF_ENTRY_AVC1:  return FOURCC('a','v','c','1');
    case MOQ_CMAF_ENTRY_HVC1: return FOURCC('h','v','c','1');
    case MOQ_CMAF_ENTRY_AV01:  return FOURCC('a','v','0','1');
    case MOQ_CMAF_ENTRY_MP4A:  return FOURCC('m','p','4','a');
    case MOQ_CMAF_ENTRY_OPUS: return FOURCC('O','p','u','s');
    default:                  return 0;
    }
}

static bool codec_is_video(moq_cmaf_sample_entry_t kind)
{
    return kind == MOQ_CMAF_ENTRY_AVC1 || kind == MOQ_CMAF_ENTRY_HVC1 ||
           kind == MOQ_CMAF_ENTRY_AV01;
}

static bool codec_is_nal(moq_cmaf_sample_entry_t kind)
{
    return kind == MOQ_CMAF_ENTRY_AVC1 || kind == MOQ_CMAF_ENTRY_HVC1;
}

/* -- Init segment ---------------------------------------------------- */

static void write_ftyp(buf_t *b)
{
    size_t at = box_open(b, FOURCC('f','t','y','p'));
    put32(b, FOURCC('c','m','f','c'));  /* major_brand: CMAF */
    put32(b, 0);                        /* minor_version */
    put32(b, FOURCC('c','m','f','c'));
    put32(b, FOURCC('i','s','o','6'));
    put32(b, FOURCC('i','s','o','m'));
    box_close(b, at);
}

static void write_codec_config_box(buf_t *b, const moq_cmaf_packager_t *p)
{
    moq_bytes_t rec = { p->codec_config, p->codec_config_len };

    switch (p->codec_kind) {
    case MOQ_CMAF_ENTRY_AVC1: {
        size_t at = box_open(b, FOURCC('a','v','c','C'));
        put_bytes(b, rec.data, rec.len);
        box_close(b, at);
        break;
    }
    case MOQ_CMAF_ENTRY_HVC1: {
        size_t at = box_open(b, FOURCC('h','v','c','C'));
        put_bytes(b, rec.data, rec.len);
        box_close(b, at);
        break;
    }
    case MOQ_CMAF_ENTRY_AV01: {
        size_t at = box_open(b, FOURCC('a','v','1','C'));
        put_bytes(b, rec.data, rec.len);
        box_close(b, at);
        break;
    }
    case MOQ_CMAF_ENTRY_OPUS: {
        size_t at = box_open(b, FOURCC('d','O','p','s'));
        put_bytes(b, rec.data, rec.len);
        box_close(b, at);
        break;
    }
    case MOQ_CMAF_ENTRY_MP4A: {
        /*
         * esds: ES_Descriptor( DecoderConfigDescriptor( DecoderSpecificInfo )
         * + SLConfigDescriptor ). Sizes are computed bottom-up because a
         * descriptor length precedes its payload.
         */
        const size_t dsi_payload = rec.len;
        const size_t dcd_payload = 13 + DESCR_HDR + dsi_payload;
        const size_t sl_payload  = 1;
        const size_t esd_payload = 3 + DESCR_HDR + dcd_payload
                                     + DESCR_HDR + sl_payload;

        size_t at = box_open(b, FOURCC('e','s','d','s'));
        put_fullbox(b, 0, 0);

        put_descr(b, 0x03, esd_payload);
        put16(b, (uint16_t)p->track_id);  /* ES_ID */
        put8(b, 0);                       /* no dependency, no URL, no OCR */

        put_descr(b, 0x04, dcd_payload);
        put8(b, 0x40);                    /* objectTypeIndication: AAC */
        /* streamType=AudioStream(0x05)<<2 | upStream=0 | reserved=1 */
        put8(b, 0x15);
        put24(b, 0);                      /* bufferSizeDB: not tracked */
        put32(b, p->avg_bitrate);         /* maxBitrate */
        put32(b, p->avg_bitrate);         /* avgBitrate */

        put_descr(b, 0x05, dsi_payload);
        put_bytes(b, rec.data, rec.len);

        put_descr(b, 0x06, sl_payload);
        put8(b, 0x02);                    /* predefined: MP4 */

        box_close(b, at);
        break;
    }
    default:
        b->err = MOQ_ERR_UNSUPPORTED;
        break;
    }
}

static void write_sample_entry(buf_t *b, const moq_cmaf_packager_t *p,
                               const moq_cmaf_packager_cfg_t *cfg)
{
    size_t at = box_open(b, sample_entry_type(p->codec_kind));

    put_zero(b, 6);   /* SampleEntry reserved */
    put16(b, 1);      /* data_reference_index */

    if (p->is_video) {
        put_zero(b, 2);                    /* pre_defined */
        put_zero(b, 2);                    /* reserved */
        put_zero(b, 12);                   /* pre_defined[3] */
        put16(b, (uint16_t)cfg->width);
        put16(b, (uint16_t)cfg->height);
        put32(b, 0x00480000);              /* horizresolution: 72 dpi */
        put32(b, 0x00480000);              /* vertresolution */
        put32(b, 0);                       /* reserved */
        put16(b, 1);                       /* frame_count */
        put_zero(b, 32);                   /* compressorname */
        put16(b, 0x0018);                  /* depth: colour, no alpha */
        put16(b, 0xffff);                  /* pre_defined = -1 */
    } else {
        put_zero(b, 8);                    /* reserved[2] */
        put16(b, (uint16_t)cfg->channel_count);
        put16(b, 16);                      /* samplesize */
        put_zero(b, 2);                    /* pre_defined */
        put_zero(b, 2);                    /* reserved */
        /*
         * samplerate is 16.16 fixed point, so only the integer part is
         * expressible. RFC 7845 4.1 pins the Opus sample entry at 48000
         * whatever the mdhd timescale says.
         */
        uint32_t entry_rate = (p->codec_kind == MOQ_CMAF_ENTRY_OPUS)
                                  ? 48000u : cfg->samplerate;
        put32(b, entry_rate << 16);
    }

    write_codec_config_box(b, p);
    box_close(b, at);
}

static void write_stbl(buf_t *b, const moq_cmaf_packager_t *p,
                       const moq_cmaf_packager_cfg_t *cfg)
{
    size_t at = box_open(b, FOURCC('s','t','b','l'));

    size_t stsd = box_open(b, FOURCC('s','t','s','d'));
    put_fullbox(b, 0, 0);
    put32(b, 1);                     /* entry_count */
    write_sample_entry(b, p, cfg);
    box_close(b, stsd);

    /* A fragmented track keeps its sample tables empty: every sample is
     * described by a trun instead. */
    size_t stts = box_open(b, FOURCC('s','t','t','s'));
    put_fullbox(b, 0, 0);
    put32(b, 0);
    box_close(b, stts);

    size_t stsc = box_open(b, FOURCC('s','t','s','c'));
    put_fullbox(b, 0, 0);
    put32(b, 0);
    box_close(b, stsc);

    size_t stsz = box_open(b, FOURCC('s','t','s','z'));
    put_fullbox(b, 0, 0);
    put32(b, 0);                     /* sample_size */
    put32(b, 0);                     /* sample_count */
    box_close(b, stsz);

    size_t stco = box_open(b, FOURCC('s','t','c','o'));
    put_fullbox(b, 0, 0);
    put32(b, 0);
    box_close(b, stco);

    box_close(b, at);
}

static void write_minf(buf_t *b, const moq_cmaf_packager_t *p,
                       const moq_cmaf_packager_cfg_t *cfg)
{
    size_t at = box_open(b, FOURCC('m','i','n','f'));

    if (p->is_video) {
        size_t vmhd = box_open(b, FOURCC('v','m','h','d'));
        put_fullbox(b, 0, 1);        /* flags = 1, as the spec requires */
        put16(b, 0);                 /* graphicsmode */
        put_zero(b, 6);              /* opcolor */
        box_close(b, vmhd);
    } else {
        size_t smhd = box_open(b, FOURCC('s','m','h','d'));
        put_fullbox(b, 0, 0);
        put16(b, 0);                 /* balance */
        put16(b, 0);                 /* reserved */
        box_close(b, smhd);
    }

    size_t dinf = box_open(b, FOURCC('d','i','n','f'));
    size_t dref = box_open(b, FOURCC('d','r','e','f'));
    put_fullbox(b, 0, 0);
    put32(b, 1);                     /* entry_count */
    size_t url = box_open(b, FOURCC('u','r','l',' '));
    put_fullbox(b, 0, 1);            /* flags = 1: media is self-contained */
    box_close(b, url);
    box_close(b, dref);
    box_close(b, dinf);

    write_stbl(b, p, cfg);
    box_close(b, at);
}

static void write_trak(buf_t *b, const moq_cmaf_packager_t *p,
                       const moq_cmaf_packager_cfg_t *cfg)
{
    size_t at = box_open(b, FOURCC('t','r','a','k'));

    size_t tkhd = box_open(b, FOURCC('t','k','h','d'));
    put_fullbox(b, 0, 0x000003);     /* enabled | in_movie */
    put32(b, 0);                     /* creation_time */
    put32(b, 0);                     /* modification_time */
    put32(b, p->track_id);
    put32(b, 0);                     /* reserved */
    put32(b, 0);                     /* duration: unknown, this is live */
    put_zero(b, 8);                  /* reserved[2] */
    put16(b, 0);                     /* layer */
    put16(b, 0);                     /* alternate_group */
    put16(b, p->is_video ? 0 : 0x0100); /* volume: 1.0 for audio */
    put16(b, 0);                     /* reserved */
    /* Unity matrix. */
    put32(b, 0x00010000); put32(b, 0); put32(b, 0);
    put32(b, 0); put32(b, 0x00010000); put32(b, 0);
    put32(b, 0); put32(b, 0); put32(b, 0x40000000);
    put32(b, p->is_video ? (cfg->width << 16) : 0);   /* 16.16 width */
    put32(b, p->is_video ? (cfg->height << 16) : 0);  /* 16.16 height */
    box_close(b, tkhd);

    size_t mdia = box_open(b, FOURCC('m','d','i','a'));

    size_t mdhd = box_open(b, FOURCC('m','d','h','d'));
    put_fullbox(b, 0, 0);
    put32(b, 0);                     /* creation_time */
    put32(b, 0);                     /* modification_time */
    put32(b, p->timescale);
    put32(b, 0);                     /* duration */
    put16(b, 0x55c4);                /* language: "und", packed ISO-639-2/T */
    put16(b, 0);                     /* pre_defined */
    box_close(b, mdhd);

    size_t hdlr = box_open(b, FOURCC('h','d','l','r'));
    put_fullbox(b, 0, 0);
    put32(b, 0);                     /* pre_defined */
    put32(b, p->is_video ? FOURCC('v','i','d','e') : FOURCC('s','o','u','n'));
    put_zero(b, 12);                 /* reserved */
    if (p->is_video)
        put_bytes(b, "VideoHandler", 13);   /* NUL included */
    else
        put_bytes(b, "SoundHandler", 13);
    box_close(b, hdlr);

    write_minf(b, p, cfg);
    box_close(b, mdia);
    box_close(b, at);
}

static void write_moov(buf_t *b, const moq_cmaf_packager_t *p,
                       const moq_cmaf_packager_cfg_t *cfg)
{
    size_t at = box_open(b, FOURCC('m','o','o','v'));

    size_t mvhd = box_open(b, FOURCC('m','v','h','d'));
    put_fullbox(b, 0, 0);
    put32(b, 0);                     /* creation_time */
    put32(b, 0);                     /* modification_time */
    put32(b, 1000);                  /* timescale: movie-level, milliseconds */
    put32(b, 0);                     /* duration: unknown */
    put32(b, 0x00010000);            /* rate: 1.0 */
    put16(b, 0x0100);                /* volume: 1.0 */
    put16(b, 0);                     /* reserved */
    put_zero(b, 8);                  /* reserved[2] */
    put32(b, 0x00010000); put32(b, 0); put32(b, 0);
    put32(b, 0); put32(b, 0x00010000); put32(b, 0);
    put32(b, 0); put32(b, 0); put32(b, 0x40000000);
    put_zero(b, 24);                 /* pre_defined */
    put32(b, p->track_id + 1);       /* next_track_ID */
    box_close(b, mvhd);

    write_trak(b, p, cfg);

    /*
     * mvex marks the track as fragmented. No mehd: the total duration of a
     * live stream is not known when the init segment is built.
     */
    size_t mvex = box_open(b, FOURCC('m','v','e','x'));
    size_t trex = box_open(b, FOURCC('t','r','e','x'));
    put_fullbox(b, 0, 0);
    put32(b, p->track_id);
    put32(b, 1);                     /* default_sample_description_index */
    /* No trex defaults: each chunk supplies duration, size and flags through
     * tfhd/trun, so it does not inherit these from the init segment. */
    put32(b, 0);                     /* default_sample_duration */
    put32(b, 0);                     /* default_sample_size */
    put32(b, 0);                     /* default_sample_flags */
    box_close(b, trex);
    box_close(b, mvex);

    box_close(b, at);
}

/* -- Config validation ----------------------------------------------- */

static moq_result_t config_codec(const moq_cmaf_packager_cfg_t *cfg,
                                 moq_cmaf_sample_entry_t *kind,
                                 moq_codec_source_format_t *source,
                                 moq_codec_config_format_t *format,
                                 moq_bytes_t *entry)
{
    switch (cfg->sample_entry) {
    case MOQ_CMAF_ENTRY_AVC3:
    case MOQ_CMAF_ENTRY_HEV1: return MOQ_ERR_UNSUPPORTED;
    case MOQ_CMAF_ENTRY_AVC1:
        *kind = MOQ_CMAF_ENTRY_AVC1; *source = MOQ_CODEC_SOURCE_AVC_AVCC;
        *format = MOQ_CODEC_CONFIG_AVCC; *entry = MOQ_BYTES_LITERAL("avc1"); break;
    case MOQ_CMAF_ENTRY_HVC1:
        *kind = MOQ_CMAF_ENTRY_HVC1; *source = MOQ_CODEC_SOURCE_HEVC_HVCC;
        *format = MOQ_CODEC_CONFIG_HVCC; *entry = MOQ_BYTES_LITERAL("hvc1"); break;
    case MOQ_CMAF_ENTRY_MP4A:
        *kind = MOQ_CMAF_ENTRY_MP4A; *source = MOQ_CODEC_SOURCE_AAC_ASC;
        *format = MOQ_CODEC_CONFIG_AAC_ASC; *entry = MOQ_BYTES_LITERAL("mp4a"); break;
    case MOQ_CMAF_ENTRY_AV01:
        *kind = MOQ_CMAF_ENTRY_AV01; *source = MOQ_CODEC_SOURCE_AV1_AV1C;
        *format = MOQ_CODEC_CONFIG_AV1C; *entry = MOQ_BYTES_LITERAL("av01"); break;
    case MOQ_CMAF_ENTRY_OPUS:
        *kind = MOQ_CMAF_ENTRY_OPUS; *source = MOQ_CODEC_SOURCE_OPUS_DOPS;
        *format = MOQ_CODEC_CONFIG_OPUS; *entry = MOQ_BYTES_LITERAL("opus"); break;
    default: return MOQ_ERR_INVAL;
    }
    return MOQ_OK;
}

static moq_result_t validate_cfg(moq_cmaf_packager_cfg_t *cfg,
                                 moq_cmaf_sample_entry_t kind)
{
    if (!cfg->codec_config.data || cfg->codec_config.len == 0) return MOQ_ERR_INVAL;
    if (cfg->codec_config.len > PACKAGER_MAX_CONFIG) return MOQ_ERR_UNSUPPORTED;
    if (cfg->track_id == UINT32_MAX) return MOQ_ERR_INVAL;
    if (cfg->track_id == 0) cfg->track_id = 1;
    if (codec_is_video(kind)) {
        if (!cfg->width || !cfg->height || cfg->width > UINT16_MAX ||
            cfg->height > UINT16_MAX) return MOQ_ERR_INVAL;
    } else {
        if (!cfg->samplerate || cfg->samplerate > UINT16_MAX ||
            !cfg->channel_count || cfg->channel_count > UINT16_MAX)
            return MOQ_ERR_INVAL;
        if (kind == MOQ_CMAF_ENTRY_OPUS && cfg->samplerate != 48000)
            return MOQ_ERR_INVAL;
        if (cfg->timescale && cfg->timescale != cfg->samplerate) return MOQ_ERR_INVAL;
        cfg->timescale = cfg->samplerate;
    }
    bool format_ok = codec_is_nal(kind)
        ? cfg->sample_format == MOQ_CMAF_SAMPLE_NAL_ANNEXB ||
          cfg->sample_format == MOQ_CMAF_SAMPLE_NAL_LENGTH_PREFIXED
        : kind == MOQ_CMAF_ENTRY_MP4A
        ? cfg->sample_format == MOQ_CMAF_SAMPLE_AAC_RAW ||
          cfg->sample_format == MOQ_CMAF_SAMPLE_AAC_ADTS
        : kind == MOQ_CMAF_ENTRY_AV01
        ? cfg->sample_format == MOQ_CMAF_SAMPLE_AV1_OBU
        : cfg->sample_format == MOQ_CMAF_SAMPLE_OPUS_PACKET;
    if (!format_ok || cfg->timescale == 0 || cfg->timescale > RESCALE_MAX ||
        cfg->source_timebase_den > RESCALE_MAX ||
        ((cfg->source_timebase_num == 0) != (cfg->source_timebase_den == 0)) ||
        (int64_t)cfg->timescale * cfg->source_timebase_num > RESCALE_MAX)
        return MOQ_ERR_INVAL;
    return MOQ_OK;
}

/* ADTS cannot represent arbitrary ASC extensions. Keep this narrow framing
 * check separate from shared record validation and refuse other valid forms. */
static moq_result_t adts_config(const moq_cmaf_packager_cfg_t *cfg)
{
    codec_asc_info_t info;
    moq_result_t rc = codec_asc_inspect(cfg->codec_config.data, cfg->codec_config.len, &info);
    if (rc < 0) return rc;
    /* Only the ADTS representation restrictions belong here: indexed rate,
     * two-byte GA record, and no frame-length/core-coder/extension flags. */
    if (cfg->codec_config.len != 2 || info.object_type < 1 || info.object_type > 4 ||
        info.frequency_index > 12 || !info.declared_frequency_known ||
        !info.declared_channels_known || (cfg->codec_config.data[1] & 7))
        return MOQ_ERR_UNSUPPORTED;
    if (info.frequency != cfg->samplerate || info.declared_channels != cfg->channel_count)
        return MOQ_ERR_INVAL;
    return MOQ_OK;
}

/* -- Public API ------------------------------------------------------ */

void moq_cmaf_packager_cfg_init_sized(moq_cmaf_packager_cfg_t *cfg, size_t cfg_size)
{
    if (!cfg || cfg_size < sizeof(cfg->struct_size)) return;
    size_t n = cfg_size < sizeof(*cfg) ? cfg_size : sizeof(*cfg);
    memset(cfg, 0, n);
    cfg->struct_size = (uint32_t)n;
}

moq_result_t moq_cmaf_packager_create(const moq_alloc_t *alloc,
                                      const moq_cmaf_packager_cfg_t *cfg,
                                      moq_cmaf_packager_t **out)
{
    if (out) *out = NULL;
    if (!cfg || !out) return MOQ_ERR_INVAL;

    if (cfg->struct_size < MOQ_CMAF_PACKAGER_CFG_V0_SIZE)
        return MOQ_ERR_INVAL;

    if (!alloc) alloc = moq_alloc_default();
    if (!alloc->alloc || !alloc->free) return MOQ_ERR_INVAL;

    /* A local copy: validation fills in the derived fields without touching
     * the caller's struct. */
    moq_cmaf_packager_cfg_t c;
    memset(&c, 0, sizeof(c));
    memcpy(&c, cfg, MOQ_CMAF_PACKAGER_CFG_V0_SIZE);
    moq_cmaf_sample_entry_t kind = MOQ_CMAF_ENTRY_UNSET;
    moq_codec_source_format_t source;
    moq_codec_config_format_t format;
    moq_bytes_t entry;
    moq_result_t rc = config_codec(&c, &kind, &source, &format, &entry);
    if (rc < 0) return rc;
    rc = validate_cfg(&c, kind);
    if (rc < 0) return rc;
    moq_codec_init_data_cfg_t record;
    moq_codec_init_data_cfg_init(&record);
    record.source_format = source;
    record.source = c.codec_config;
    size_t record_size = 0;
    rc = moq_codec_init_data_build(&record, NULL, 0, &record_size);
    if (rc != MOQ_ERR_BUFFER) return rc < 0 ? rc : MOQ_ERR_INTERNAL;
    moq_codec_string_cfg_t signaling;
    moq_codec_string_cfg_init(&signaling);
    signaling.config_format = format;
    signaling.sample_entry = entry;
    signaling.decoder_config = c.codec_config;
    signaling.has_mp4_object_type_indication = kind == MOQ_CMAF_ENTRY_MP4A;
    signaling.mp4_object_type_indication = 0x40;
    size_t string_size = 0;
    rc = moq_codec_string_format(&signaling, NULL, 0, &string_size);
    if (rc != MOQ_ERR_BUFFER) return rc < 0 ? rc : MOQ_ERR_INTERNAL;

    if (kind == MOQ_CMAF_ENTRY_MP4A) {
        codec_asc_info_t info;
        rc = codec_asc_inspect(c.codec_config.data, c.codec_config.len, &info);
        if (rc < 0) return rc;
        if ((info.declared_frequency_known && info.frequency != c.samplerate) ||
            (info.declared_channels_known && info.declared_channels != c.channel_count))
            return MOQ_ERR_INVAL;
    } else if (kind == MOQ_CMAF_ENTRY_OPUS) {
        /* Shared dOps validation above guarantees the channel byte exists.
         * This comparison adds no second record parser. */
        uint8_t channels = c.codec_config.data[1];
        if (channels == 0) return MOQ_ERR_PROTO;
        if (channels != c.channel_count) return MOQ_ERR_INVAL;
    }

    if (c.sample_format == MOQ_CMAF_SAMPLE_AAC_ADTS) {
        rc = adts_config(&c);
        if (rc < 0) return rc;
    }
    uint8_t nal_length_size = 4;
    if (codec_is_nal(kind)) {
        rc = nal_length_size_from_record(kind, c.codec_config,
                                         &nal_length_size);
        if (rc < 0) return rc;
        if (nal_length_size == 3) return MOQ_ERR_UNSUPPORTED;
    }

    moq_cmaf_packager_t *p = alloc->alloc(sizeof(*p), alloc->ctx);
    if (!p) return MOQ_ERR_NOMEM;
    memset(p, 0, sizeof(*p));

    p->alloc = *alloc;
    p->codec_kind = kind;
    p->sample_format = c.sample_format;
    p->is_video = codec_is_video(kind);
    p->timescale = c.timescale;
    p->track_id = c.track_id;
    p->avg_bitrate = c.avg_bitrate;
    p->nal_length_size = nal_length_size;
    p->rebase = c.rebase_timestamps;
    p->src_num = (int64_t)c.timescale * (int64_t)c.source_timebase_num;
    p->src_den = (int64_t)c.source_timebase_den;
    p->init.alloc = &p->alloc;
    p->frag.alloc = &p->alloc;
    p->mdat.alloc = &p->alloc;

    p->codec_config = alloc->alloc(c.codec_config.len, alloc->ctx);
    if (!p->codec_config) {
        moq_cmaf_packager_destroy(p);
        return MOQ_ERR_NOMEM;
    }
    memcpy(p->codec_config, c.codec_config.data, c.codec_config.len);
    p->codec_config_len = c.codec_config.len;

    write_ftyp(&p->init);
    write_moov(&p->init, p, &c);
    if (p->init.err) {
        rc = p->init.err;
        moq_cmaf_packager_destroy(p);
        return rc;
    }

    *out = p;
    return MOQ_OK;
}

void moq_cmaf_packager_destroy(moq_cmaf_packager_t *p)
{
    if (!p) return;
    moq_alloc_t alloc = p->alloc;

    buf_free(&p->init);
    buf_free(&p->frag);
    buf_free(&p->mdat);
    if (p->codec_config)
        alloc.free(p->codec_config, p->codec_config_len, alloc.ctx);
    alloc.free(p, sizeof(*p), alloc.ctx);
}

moq_bytes_t moq_cmaf_packager_init_segment(const moq_cmaf_packager_t *p)
{
    moq_bytes_t empty = { NULL, 0 };
    if (!p) return empty;

    moq_bytes_t out = { p->init.p, p->init.len };
    return out;
}

uint32_t moq_cmaf_packager_timescale(const moq_cmaf_packager_t *p)
{
    return p ? p->timescale : 0;
}

/* ADTS framing only. Compare a header-derived ASC against the already
 * shared-validator-checked record; do not add a second ASC bitstream parser. */
static moq_result_t adts_skip(const moq_cmaf_packager_t *p, moq_bytes_t data,
                              size_t *skip)
{
    if (data.len < 7 || data.data[0] != 0xff ||
        (data.data[1] & 0xf6) != 0xf0) return MOQ_ERR_PROTO;
    size_t header = (data.data[1] & 1) ? 7 : 9;
    size_t length = ((size_t)(data.data[3] & 3) << 11) |
                    ((size_t)data.data[4] << 3) | (data.data[5] >> 5);
    if (length != data.len || length <= header) return MOQ_ERR_PROTO;
    if ((data.data[6] & 3) != 0) return MOQ_ERR_UNSUPPORTED;
    unsigned aot = (data.data[2] >> 6) + 1;
    unsigned frequency = (data.data[2] >> 2) & 15;
    unsigned channels = ((data.data[2] & 1) << 2) | (data.data[3] >> 6);
    if (frequency > 12) return MOQ_ERR_PROTO;
    if (channels == 0) return MOQ_ERR_UNSUPPORTED;
    uint8_t asc[2] = {
        (uint8_t)((aot << 3) | (frequency >> 1)),
        (uint8_t)((frequency << 7) | (channels << 3))
    };
    if (p->codec_config_len != 2 ||
        memcmp(asc, p->codec_config, sizeof(asc)) != 0) return MOQ_ERR_PROTO;
    *skip = header;
    return MOQ_OK;
}

static moq_result_t convert_sample(const moq_cmaf_packager_t *p, moq_bytes_t data,
                                   buf_t *out, uint32_t *out_size)
{
    size_t written = 0, skip = 0;
    if (codec_is_nal(p->codec_kind)) {
        moq_result_t rc = convert_nals(out, data, p->nal_length_size, p->codec_kind,
            p->sample_format == MOQ_CMAF_SAMPLE_NAL_ANNEXB, &written);
        if (rc < 0) return rc;
    } else {
        if (p->sample_format == MOQ_CMAF_SAMPLE_AAC_ADTS) {
            moq_result_t rc = adts_skip(p, data, &skip);
            if (rc < 0) return rc;
        }
        written = data.len - skip;
        if (out) {
            put_bytes(out, data.data + skip, written);
            if (out->err) return out->err;
        }
    }
    if (written == 0) return MOQ_ERR_PROTO;
    if (written > PACKAGER_MAX_PAYLOAD) return MOQ_ERR_UNSUPPORTED;
    *out_size = (uint32_t)written;
    return MOQ_OK;
}

/* -- Fragment writing ------------------------------------------------ */

/* Per-sample trun record, resolved before the moof is written: which optional
 * fields the trun carries depends on how these compare to each other. */
typedef struct {
    uint32_t size;
    uint32_t duration;
    uint32_t flags;
    int32_t  composition_offset;
} sample_rec_t;

/* tfhd optional fields. */
#define TFHD_SAMPLE_DESCRIPTION_INDEX 0x000002u
#define TFHD_DEFAULT_SAMPLE_DURATION  0x000008u
#define TFHD_DEFAULT_SAMPLE_SIZE      0x000010u
#define TFHD_DEFAULT_SAMPLE_FLAGS     0x000020u
#define TFHD_DEFAULT_BASE_IS_MOOF     0x020000u

/* trun optional fields. */
#define TRUN_DATA_OFFSET              0x000001u
#define TRUN_FIRST_SAMPLE_FLAGS       0x000004u
#define TRUN_SAMPLE_DURATION          0x000100u
#define TRUN_SAMPLE_SIZE              0x000200u
#define TRUN_SAMPLE_FLAGS             0x000400u
#define TRUN_SAMPLE_COMPOSITION_OFFSET 0x000800u

/*
 * Which optional fields the trun has to carry: only what differs from the tfhd
 * defaults, which is what the reference mp4 muxers do.
 */
static uint32_t resolve_trun_flags(const sample_rec_t *recs, size_t count,
                                   uint32_t default_duration,
                                   uint32_t default_size,
                                   uint32_t default_flags)
{
    uint32_t flags = TRUN_DATA_OFFSET;

    for (size_t i = 0; i < count; i++) {
        if (recs[i].duration != default_duration)
            flags |= TRUN_SAMPLE_DURATION;
        if (recs[i].size != default_size)
            flags |= TRUN_SAMPLE_SIZE;
        if (i > 0 && recs[i].flags != default_flags)
            flags |= TRUN_SAMPLE_FLAGS;
        if (recs[i].composition_offset != 0)
            flags |= TRUN_SAMPLE_COMPOSITION_OFFSET;
    }

    /* Only the opening sample differs: one override beats a per-sample field
     * on every sample. */
    if (!(flags & TRUN_SAMPLE_FLAGS) && recs[0].flags != default_flags)
        flags |= TRUN_FIRST_SAMPLE_FLAGS;

    return flags;
}

/*
 * moof+mdat for one chunk. The tfhd carries the defaults, so the chunk still
 * stands alone; default_sample_flags comes from the SECOND sample when there
 * is one, because a chunk normally opens on a sync sample and taking its flags
 * as the default would force every following sample to restate its own.
 */
static void write_chunk(moq_cmaf_packager_t *p, const sample_rec_t *recs,
                        size_t count, uint64_t base_dts)
{
    const uint32_t default_duration = recs[0].duration;
    const uint32_t default_size = recs[0].size;
    const uint32_t default_flags = (count > 1) ? recs[1].flags : recs[0].flags;
    const uint32_t trun_flags = resolve_trun_flags(recs, count,
                                                   default_duration,
                                                   default_size, default_flags);

    /* Version 1 makes the composition offset signed, which reordered frames
     * need; with no offsets at all the field is absent anyway. */
    uint8_t trun_version = 0;
    for (size_t i = 0; i < count; i++) {
        if (recs[i].composition_offset < 0) trun_version = 1;
    }

    buf_t *b = &p->frag;
    size_t moof = box_open(b, FOURCC('m','o','o','f'));

    size_t mfhd = box_open(b, FOURCC('m','f','h','d'));
    put_fullbox(b, 0, 0);
    put32(b, p->sequence_number + 1);
    box_close(b, mfhd);

    size_t traf = box_open(b, FOURCC('t','r','a','f'));

    size_t tfhd = box_open(b, FOURCC('t','f','h','d'));
    /*
     * default-base-is-moof with no base_data_offset: the trun data_offset is
     * relative to this moof, so the chunk carries no file-absolute reference
     * and stands alone as a MoQ object payload. CMAF wants the sample
     * description index stated rather than inherited from trex.
     */
    put_fullbox(b, 0, TFHD_DEFAULT_BASE_IS_MOOF |
                      TFHD_SAMPLE_DESCRIPTION_INDEX |
                      TFHD_DEFAULT_SAMPLE_DURATION |
                      TFHD_DEFAULT_SAMPLE_SIZE |
                      TFHD_DEFAULT_SAMPLE_FLAGS);
    put32(b, p->track_id);
    put32(b, 1);                     /* sample_description_index */
    put32(b, default_duration);
    put32(b, default_size);
    put32(b, default_flags);
    box_close(b, tfhd);

    size_t tfdt = box_open(b, FOURCC('t','f','d','t'));
    put_fullbox(b, 1, 0);            /* version 1: 64-bit */
    put64(b, base_dts);
    box_close(b, tfdt);

    size_t trun = box_open(b, FOURCC('t','r','u','n'));
    put_fullbox(b, trun_version, trun_flags);
    put32(b, (uint32_t)count);
    const size_t data_offset_at = b->len;
    put32(b, 0);                     /* data_offset, patched below */
    if (trun_flags & TRUN_FIRST_SAMPLE_FLAGS)
        put32(b, recs[0].flags);

    for (size_t i = 0; i < count; i++) {
        if (trun_flags & TRUN_SAMPLE_DURATION)
            put32(b, recs[i].duration);
        if (trun_flags & TRUN_SAMPLE_SIZE)
            put32(b, recs[i].size);
        if (trun_flags & TRUN_SAMPLE_FLAGS)
            put32(b, recs[i].flags);
        if (trun_flags & TRUN_SAMPLE_COMPOSITION_OFFSET)
            put32(b, (uint32_t)recs[i].composition_offset);
    }

    box_close(b, trun);
    box_close(b, traf);
    box_close(b, moof);

    /* data_offset counts from the start of the moof, so it is the moof's own
     * size plus the mdat header, known only now. */
    if (!b->err) {
        size_t offset = b->len - moof + 8;
        if (offset > INT32_MAX) { b->err = MOQ_ERR_UNSUPPORTED; return; }
        poke32(b->p + data_offset_at, (uint32_t)offset);
    }

    size_t mdat = box_open(b, FOURCC('m','d','a','t'));
    put_bytes(b, p->mdat.p, p->mdat.len);
    box_close(b, mdat);
}

static bool ranges_overlap(const void *a, size_t an, const void *b, size_t bn)
{
    if (!an || !bn || !a || !b) return false;
    uintptr_t x = (uintptr_t)a, y = (uintptr_t)b;
    return x <= y ? y - x < an : x - y < bn;
}

static bool aliases(const moq_cmaf_packager_t *p, const void *data, size_t size)
{
    return ranges_overlap(data, size, p, sizeof(*p)) ||
           ranges_overlap(data, size, p->init.p, p->init.cap) ||
           ranges_overlap(data, size, p->frag.p, p->frag.cap) ||
           ranges_overlap(data, size, p->mdat.p, p->mdat.cap) ||
           ranges_overlap(data, size, p->codec_config, p->codec_config_len);
}

moq_result_t moq_cmaf_packager_write(moq_cmaf_packager_t *p,
                                     const moq_cmaf_packager_sample_t *samples,
                                     size_t count, moq_bytes_t *out_fragment)
{
    if (out_fragment) *out_fragment = (moq_bytes_t){ NULL, 0 };
    if (!p || !samples || !out_fragment || count == 0) return MOQ_ERR_INVAL;
    if (count > PACKAGER_MAX_SAMPLES || count > SIZE_MAX / sizeof(*samples))
        return MOQ_ERR_UNSUPPORTED;
    if (p->sequence_number == UINT32_MAX) return MOQ_ERR_UNSUPPORTED;
    if (aliases(p, samples, count * sizeof(*samples))) return MOQ_ERR_INVAL;
    for (size_t i = 0; i < count; i++) {
        if (!samples[i].data.data || !samples[i].data.len) return MOQ_ERR_INVAL;
        if (aliases(p, samples[i].data.data, samples[i].data.len)) return MOQ_ERR_INVAL;
        if (samples[i].data.len > PACKAGER_MAX_PAYLOAD) return MOQ_ERR_UNSUPPORTED;
    }

    const int64_t origin = !p->rebase ? 0
                         : p->have_origin ? p->origin : samples[0].dts;
    int64_t next_dts = 0, base_dts = 0;
    size_t payload_bytes = 0;
    /* Preflight all timing, framing and representability before allocation or
     * buffer mutation. A failure cannot move the timeline or sequence. */
    for (size_t i = 0; i < count; i++) {
        const moq_cmaf_packager_sample_t *sample = &samples[i];
        int64_t rel_dts, rel_pts, dts, pts, cts;
        if (!sample->duration ||
            !sub_checked(sample->dts, origin, &rel_dts) ||
            !sub_checked(sample->pts, origin, &rel_pts) ||
            !rescale(rel_dts, p->src_num, p->src_den, &dts) ||
            !rescale(rel_pts, p->src_num, p->src_den, &pts) ||
            !sub_checked(pts, dts, &cts) || cts < INT32_MIN || cts > INT32_MAX ||
            dts < 0 || dts > INT64_MAX - sample->duration ||
            (i ? dts != next_dts : p->have_origin && dts < p->next_dts))
            return MOQ_ERR_INVAL;
        if (!i) base_dts = dts;
        next_dts = dts + sample->duration;
        uint32_t size;
        moq_result_t rc = convert_sample(p, sample->data, NULL, &size);
        if (rc < 0) return rc;
        if (size > PACKAGER_MAX_PAYLOAD - payload_bytes) return MOQ_ERR_UNSUPPORTED;
        payload_bytes += size;
    }

    sample_rec_t inline_recs[16];
    sample_rec_t *recs = inline_recs;
    if (count > sizeof(inline_recs) / sizeof(inline_recs[0])) {
        if (count > SIZE_MAX / sizeof(*recs)) return MOQ_ERR_UNSUPPORTED;
        recs = p->alloc.alloc(count * sizeof(*recs), p->alloc.ctx);
        if (!recs) return MOQ_ERR_NOMEM;
    }
    p->mdat.len = p->frag.len = 0;
    p->mdat.err = p->frag.err = MOQ_OK;
    moq_result_t rc = MOQ_OK;
    for (size_t i = 0; i < count; i++) {
        int64_t rel_dts, rel_pts, dts, pts;
        /* Identical calculations to the checked preflight; no inputs retained. */
        if (!sub_checked(samples[i].dts, origin, &rel_dts) ||
            !sub_checked(samples[i].pts, origin, &rel_pts) ||
            !rescale(rel_dts, p->src_num, p->src_den, &dts) ||
            !rescale(rel_pts, p->src_num, p->src_den, &pts)) {
            rc = MOQ_ERR_INTERNAL;
            break;
        }
        recs[i].composition_offset = (int32_t)(pts - dts);
        recs[i].flags = samples[i].keyframe ? SAMPLE_FLAGS_SYNC : SAMPLE_FLAGS_NON_SYNC;
        recs[i].duration = samples[i].duration;
        rc = convert_sample(p, samples[i].data, &p->mdat, &recs[i].size);
        if (rc < 0) break;
    }
    if (rc == MOQ_OK) {
        write_chunk(p, recs, count, (uint64_t)base_dts);
        rc = p->frag.err;
    }
    if (recs != inline_recs)
        p->alloc.free(recs, count * sizeof(*recs), p->alloc.ctx);
    if (rc < 0) { p->frag.len = 0; return rc; }
    p->origin = origin;
    p->have_origin = true;
    p->next_dts = next_dts;
    p->sequence_number++;
    *out_fragment = (moq_bytes_t){ p->frag.p, p->frag.len };
    return MOQ_OK;
}
