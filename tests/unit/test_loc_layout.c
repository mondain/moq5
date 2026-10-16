#include <moq/loc.h>

#ifdef __cplusplus
#define LOC_ASSERT static_assert
#define LOC_ALIGN(T) alignof(T)
#else
#define LOC_ASSERT _Static_assert
#define LOC_ALIGN(T) _Alignof(T)
#endif

typedef struct loc_old_layout {
    uint32_t struct_size;
    bool has_timestamp;
    uint64_t timestamp;
    bool has_timescale;
    uint64_t timescale;
    bool has_video_frame_marking;
    moq_loc_video_frame_marking_t video_frame_marking;
    bool has_audio_level;
    moq_loc_audio_level_t audio_level;
    bool has_video_config;
    moq_bytes_t video_config;
} loc_old_layout_t;

#define LOC_PIN_OLD(field) \
    LOC_ASSERT(offsetof(moq_loc_headers_t, field) == \
               offsetof(loc_old_layout_t, field), "old LOC offset " #field)
LOC_PIN_OLD(struct_size);
LOC_PIN_OLD(has_timestamp);
LOC_PIN_OLD(timestamp);
LOC_PIN_OLD(has_timescale);
LOC_PIN_OLD(timescale);
LOC_PIN_OLD(has_video_frame_marking);
LOC_PIN_OLD(video_frame_marking);
LOC_PIN_OLD(has_audio_level);
LOC_PIN_OLD(audio_level);
LOC_PIN_OLD(has_video_config);
LOC_PIN_OLD(video_config);
LOC_ASSERT(sizeof(moq_loc_headers_t) == sizeof(loc_old_layout_t), "old LOC sizeof");
LOC_ASSERT(LOC_ALIGN(moq_loc_headers_t) == LOC_ALIGN(loc_old_layout_t), "old LOC alignment");
LOC_ASSERT(MOQ_LOC_HEADERS_V0_SIZE == sizeof(moq_loc_headers_t), "complete old LOC floor");
LOC_ASSERT(MOQ_LOC_METADATA_V0_SIZE == sizeof(moq_loc_metadata_t), "complete metadata floor");
LOC_ASSERT(offsetof(moq_loc_metadata_t, marking) == 25, "fixed marking offset");
LOC_ASSERT(offsetof(moq_loc_metadata_t, audio_level) == 29, "fixed raw audio offset");

/* Cross-compiled object/assembly constants pin the layout, not ARM runtime. */
#ifdef __cplusplus
extern "C" {
#endif
extern const uint32_t moq_loc_layout_receipt[10];
const uint32_t moq_loc_layout_receipt[10] = {
    (uint32_t)sizeof(void *),
    (uint32_t)sizeof(moq_loc_profile_t),
    (uint32_t)sizeof(moq_loc_headers_t),
    (uint32_t)LOC_ALIGN(moq_loc_headers_t),
    (uint32_t)offsetof(moq_loc_headers_t, video_config),
    (uint32_t)sizeof(moq_loc_metadata_t),
    (uint32_t)LOC_ALIGN(moq_loc_metadata_t),
    (uint32_t)offsetof(moq_loc_metadata_t, video_config),
    (uint32_t)offsetof(moq_loc_metadata_t, audio_config),
    (uint32_t)MOQ_LOC_METADATA_V0_SIZE
};
#ifdef __cplusplus
}
#endif

#ifdef MOQ_LOC_LAYOUT_EXECUTE
#include <stdio.h>
#ifdef MOQ_LOC_CPP_LEGACY_CONTROL
#include <moq/loc.hpp>
#include <cstring>
#endif
int main(void)
{
    printf("pointer=%u profile_enum=%u old_size=%u old_align=%u old_video=%u "
           "metadata_size=%u metadata_align=%u video=%u audio=%u floor=%u\n",
           moq_loc_layout_receipt[0], moq_loc_layout_receipt[1],
           moq_loc_layout_receipt[2], moq_loc_layout_receipt[3],
           moq_loc_layout_receipt[4], moq_loc_layout_receipt[5],
           moq_loc_layout_receipt[6], moq_loc_layout_receipt[7],
           moq_loc_layout_receipt[8], moq_loc_layout_receipt[9]);
    moq_loc_metadata_t m;
    moq_loc_metadata_init_sized(&m, sizeof(m));
    moq_bytes_t empty = {NULL, 0};
    if (moq_loc_parse_metadata(MOQ_VERSION_DRAFT_22, MOQ_LOC_PROFILE_04,
                              empty, &m, sizeof(m)) != MOQ_OK) return 1;
    moq_rcbuf_t *out = NULL;
    if (moq_loc_encode_metadata(moq_alloc_default(), MOQ_VERSION_DRAFT_22,
                               MOQ_LOC_PROFILE_04, &m, &out) != MOQ_OK) return 1;
    if (m.struct_size != sizeof(m) || m.present != 0 || out) return 1;
    moq_loc_headers_t old;
    moq_loc_headers_init(&old);
    if (moq_loc_parse(MOQ_VERSION_DRAFT_18, MOQ_LOC_PROFILE_01,
                      empty, &old) != MOQ_OK) return 1;
#ifdef MOQ_LOC_CPP_LEGACY_CONTROL
    for (int i = 0; i < 2; ++i) {
        const uint8_t literal[] = {2, (uint8_t)(i ? 0x83 : 0x43), 0xe8};
        moq_version_t version = i ? MOQ_VERSION_DRAFT_18 : MOQ_VERSION_DRAFT_16;
        auto parsed = moq::loc::parse(version, moq::loc::profile::loc01,
                                     moq::bytes_view(literal, sizeof(literal)));
        if (!parsed || !parsed.value().has_timestamp || parsed.value().timestamp != 1000)
            return 1;
        moq::loc::headers in;
        in.has_timestamp = true;
        in.timestamp = 1000;
        auto encoded = moq::loc::encode(version, moq::loc::profile::loc01, in);
        if (!encoded || encoded.value().size() != sizeof(literal) ||
            std::memcmp(encoded.value().data(), literal, sizeof(literal)) != 0)
            return 1;
    }
#endif
    return old.struct_size != sizeof(old);
}
#endif
