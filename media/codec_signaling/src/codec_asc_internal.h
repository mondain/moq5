#ifndef MOQ_CODEC_ASC_INTERNAL_H
#define MOQ_CODEC_ASC_INTERNAL_H

/* Private, noninstalled inspection shared by signaling and the writer. Static
 * inline definitions avoid a hidden cross-DSO dependency or new public API. */
#include <moq/types.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    const uint8_t *d;
    size_t len;
    size_t bitpos;
    bool overrun;
} codec_bitr_t;

static inline uint32_t codec_br_u(codec_bitr_t *b, int n)
{
    uint32_t v = 0;
    for (int i = 0; i < n; i++) {
        size_t byte = b->bitpos >> 3;
        int bit = 7 - (int)(b->bitpos & 7);
        uint32_t x = 0;
        if (byte < b->len) x = (uint32_t)((b->d[byte] >> bit) & 1);
        else b->overrun = true;
        v = (v << 1) | x;
        b->bitpos++;
    }
    return v;
}

typedef struct {
    uint32_t object_type;
    uint32_t frequency_index;
    uint32_t frequency; /* core leading value, NOT necessarily output rate */
    uint32_t channel_configuration;
    bool declared_frequency_known;
    bool declared_channels_known;
    uint32_t declared_channels;
} codec_asc_info_t;

/* The original signaling acceptance rule is unchanged: only truncated common
 * leading fields fail. No AOT-specific payload or decoder semantics validation.
 * Configuration-established facts are deliberately narrower: ordinary GA AOT1..4 with
 * only the three GA flags and zero padding, no core-coder/extension flag, and a
 * known non-PCE channel configuration. Any tail may contain a sync extension:
 * do not infer SBR output rate or PS channel count from the core fields. These
 * facts are not decoder-verified and do not exclude unsignaled implicit SBR. */
static inline moq_result_t codec_asc_inspect(const uint8_t *d, size_t len,
                                            codec_asc_info_t *out)
{
    static const uint32_t rates[] = {
        96000, 88200, 64000, 48000, 44100, 32000, 24000,
        22050, 16000, 12000, 11025, 8000, 7350
    };
    codec_bitr_t br = {d, len, 0, false};
    *out = (codec_asc_info_t){0};
    out->object_type = codec_br_u(&br, 5);
    if (out->object_type == 31) out->object_type = 32u + codec_br_u(&br, 6);
    out->frequency_index = codec_br_u(&br, 4);
    if (out->frequency_index == 15) out->frequency = codec_br_u(&br, 24);
    else if (out->frequency_index < sizeof(rates) / sizeof(rates[0]))
        out->frequency = rates[out->frequency_index];
    out->channel_configuration = codec_br_u(&br, 4);
    if (br.overrun) return MOQ_ERR_PROTO;

    bool ordinary = out->object_type >= 1 && out->object_type <= 4;
    bool known_channels = out->channel_configuration >= 1 &&
                          out->channel_configuration <= 7;
    if (ordinary && known_channels && len == (br.bitpos + 3 + 7) / 8) {
        (void)codec_br_u(&br, 1); /* frameLengthFlag does not change these facts */
        uint32_t depends = codec_br_u(&br, 1);
        uint32_t extension = codec_br_u(&br, 1);
        bool padding_zero = true;
        while (!br.overrun && (br.bitpos & 7))
            if (codec_br_u(&br, 1)) padding_zero = false;
        if (!br.overrun && !depends && !extension && padding_zero) {
            out->declared_frequency_known = out->frequency != 0;
            out->declared_channels_known = true;
            out->declared_channels = out->channel_configuration == 7
                                    ? 8 : out->channel_configuration;
        }
    }
    return MOQ_OK;
}
#endif /* MOQ_CODEC_ASC_INTERNAL_H */
