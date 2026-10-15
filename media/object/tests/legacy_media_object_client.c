/* Ordinary-enum binary compatibility client. Header is a frozen baseline,
 * never regenerated from current declarations. Old short-enum binaries are
 * outside the enum-to-fixed-width binary compatibility contract. */
#include "frozen_media_object_client.h"
#include <string.h>

int media_object_legacy_client(void);
int media_object_legacy_client(void)
{
    /* Report the old-header representation mismatch without calling across it.
     * Current-header tests still exercise a consistent short-enum build. */
    if (sizeof(moq_media_packaging_t) != sizeof(uint32_t)) return 5;
    struct { moq_media_track_info_t value; unsigned char guard[16]; } track;
    struct { moq_media_parsed_object_t value; unsigned char guard[16]; } out;
    memset(&track, 0xA5, sizeof(track));
    memset(&out, 0xA5, sizeof(out));
    moq_media_track_info_init_sized(&track.value, sizeof(track.value));
    if (track.value.struct_size != sizeof(track.value)) return 1;
    track.value.media_type = MOQ_MEDIA_TYPE_VIDEO;
    track.value.packaging = MOQ_MEDIA_PACKAGING_RAW;
    moq_media_object_input_t in;
    moq_media_object_input_init(&in);
    in.status = MOQ_OBJECT_END_OF_GROUP;
    moq_media_drop_reason_t why = MOQ_MEDIA_DROP_MALFORMED_LOC;
    if (moq_media_object_parse(&track.value, &in, NULL, 0, &out.value, &why)
        != MOQ_OK) return 2;
    if (out.value.struct_size != sizeof(out.value) || why != 0 ||
        out.value.status != MOQ_OBJECT_END_OF_GROUP) return 3;
    for (size_t i = 0; i < 16; i++)
        if (track.guard[i] != 0xA5 || out.guard[i] != 0xA5) return 4;
    return 0;
}
