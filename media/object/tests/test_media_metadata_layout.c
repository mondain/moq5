#include "media_object_layout_probe.h"

#ifdef __cplusplus
extern "C" {
#endif
extern const uint32_t moq_media_layout_receipt[13];
const uint32_t moq_media_layout_receipt[13] = {
    (uint32_t)sizeof(void *),
    (uint32_t)sizeof(moq_media_packaging_t),
    (uint32_t)sizeof(media_old_track_t),
    (uint32_t)sizeof(moq_media_track_info_t),
    (uint32_t)MEDIA_ALIGN(moq_media_track_info_t),
    (uint32_t)sizeof(media_old_parsed_t),
    (uint32_t)MEDIA_ALIGN(media_old_parsed_t),
    (uint32_t)sizeof(moq_media_parsed_object_t),
    (uint32_t)offsetof(moq_media_parsed_object_t, metadata),
    (uint32_t)sizeof(moq_media_metadata_t),
    (uint32_t)offsetof(moq_media_metadata_t, loc),
    (uint32_t)sizeof(moq_media_companion_profile_t),
    (uint32_t)sizeof(moq_media_time_domain_t)
};
#ifdef __cplusplus
}
#endif

#ifdef MOQ_MEDIA_LAYOUT_EXECUTE
#include <stdio.h>
int main(void)
{
    printf("ptr=%u packaging_enum=%u old_track=%u new_track=%u track_align=%u "
           "old_parsed=%u align=%u new_parsed=%u metadata_offset=%u metadata_size=%u "
           "loc_offset=%u companion=%u domain=%u\n",
        moq_media_layout_receipt[0], moq_media_layout_receipt[1],
        moq_media_layout_receipt[2], moq_media_layout_receipt[3],
        moq_media_layout_receipt[4], moq_media_layout_receipt[5],
        moq_media_layout_receipt[6], moq_media_layout_receipt[7],
        moq_media_layout_receipt[8], moq_media_layout_receipt[9],
        moq_media_layout_receipt[10], moq_media_layout_receipt[11],
        moq_media_layout_receipt[12]);
    moq_media_track_info_t track;
    moq_media_track_info_init_sized(&track, sizeof(track));
    track.transport_version = MOQ_VERSION_DRAFT_22;
    track.media_type = MOQ_MEDIA_TYPE_VIDEO;
    track.packaging = MOQ_MEDIA_PACKAGING_RAW;
    moq_media_object_input_t in;
    moq_media_object_input_init(&in);
    in.status = MOQ_OBJECT_END_OF_TRACK;
    moq_media_parsed_object_t out;
    moq_media_parsed_object_init_sized(&out, sizeof(out));
    moq_media_object_parse_cfg_t cfg = MOQ_MEDIA_OBJECT_PARSE_CFG_INIT;
    cfg.companion_profile = MOQ_MEDIA_COMPANION_D22_MSF01_LOC04_PUBLIC;
    if (moq_media_object_parse_sized(&track, &in, &cfg,
        NULL, 0, &out, sizeof(out), NULL) != MOQ_OK) return 1;
    return out.struct_size != sizeof(out) || out.status != MOQ_OBJECT_END_OF_TRACK ||
           out.metadata.time_domain != MOQ_MEDIA_TIME_DOMAIN_NONE;
}
#endif
