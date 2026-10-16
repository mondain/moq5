#include <moq/types.h>
#include <moq/session.h>
#include <moq/rcbuf.h>
#include <moq/cmaf.h>
#include "../../../tests/unit/test_support.h"
#include <stdlib.h>
#include <string.h>

/* The complete media-object declarations an older client was compiled
 * against, before the output tail was added. Deliberately do not include the
 * current media-object header: this client knows no new output tail. */
typedef enum frozen_packaging { FROZEN_RAW = 1, FROZEN_CMAF = 2 } frozen_packaging_t;
typedef enum frozen_media_type { FROZEN_VIDEO = 1, FROZEN_AUDIO = 2 } frozen_media_type_t;
typedef enum frozen_drop_reason {
    FROZEN_MALFORMED_LOC = 1, FROZEN_MALFORMED_CMAF = 2,
    FROZEN_MISSING_TIMESTAMP = 3, FROZEN_NON_MONOTONIC_DTS = 4,
    FROZEN_UNSUPPORTED_MULTI_SAMPLE = 5, FROZEN_STALE = 6, FROZEN_KEYFRAME_WAIT = 7
} frozen_drop_reason_t;
typedef struct frozen_track {
    uint32_t struct_size;
    frozen_media_type_t media_type;
    frozen_packaging_t packaging;
    uint32_t timescale;
    moq_version_t transport_version;
} frozen_track_t;
typedef struct frozen_input {
    uint32_t struct_size;
    uint64_t group_id, object_id;
    moq_object_status_t status;
    bool end_of_group, datagram;
    moq_rcbuf_t *payload, *properties;
} frozen_input_t;
typedef struct frozen_parsed {
    uint32_t struct_size;
    frozen_packaging_t packaging;
    moq_object_status_t status;
    bool end_of_group, datagram, has_capture_time;
    uint64_t capture_time_us, decode_time_us;
    int64_t composition_offset_us;
    uint64_t presentation_time_us;
    bool keyframe;
    moq_bytes_t payload, fragment;
    size_t mdat_offset, mdat_len, sample_count;
    const moq_cmaf_sample_t *samples;
    uint32_t sample_duration_us;
    moq_sap_type_t sap_type;
    uint32_t chunk_count, track_id;
} frozen_parsed_t;

MOQ_API void moq_media_track_info_init(frozen_track_t *info);
MOQ_API void moq_media_track_info_init_sized(frozen_track_t *info, size_t size);
MOQ_API void moq_media_object_input_init(frozen_input_t *in);
MOQ_API void moq_media_parsed_object_init(frozen_parsed_t *out);
MOQ_API moq_result_t moq_media_object_parse(const frozen_track_t *track,
    const frozen_input_t *in, moq_cmaf_sample_t *samples, size_t sample_cap,
    frozen_parsed_t *out, frozen_drop_reason_t *reason);

int main(void)
{
    int failures = 0;
    const uint8_t byte = 0x41, properties[2][3] = {{2, 0x43, 0xe8}, {2, 0x83, 0xe8}};
    moq_rcbuf_t *payload = NULL;
    MOQ_TEST_CHECK_EQ_INT(moq_rcbuf_create(moq_alloc_default(), &byte, 1, &payload), MOQ_OK);
    MOQ_TEST_CHECK(payload != NULL);
    if (!payload) return failures;
    for (size_t v = 0; v < 2; ++v) {
        frozen_track_t *track = malloc(sizeof(*track));
        frozen_input_t *in = malloc(sizeof(*in));
        frozen_parsed_t *out = malloc(sizeof(*out));
        MOQ_TEST_CHECK(track && in && out);
        if (!track || !in || !out) {
            free(track); free(in); free(out);
            continue;
        }
        memset(track, 0xa5, sizeof(*track));
        moq_media_track_info_init(track);
        MOQ_TEST_CHECK(track->struct_size == offsetof(frozen_track_t, timescale) + sizeof(track->timescale));
        uint8_t poison[sizeof(track->transport_version)];
        memset(poison, 0xa5, sizeof(poison));
        MOQ_TEST_CHECK(memcmp(&track->transport_version, poison, sizeof(poison)) == 0);
        moq_media_track_info_init_sized(track, sizeof(*track));
        MOQ_TEST_CHECK(track->struct_size == sizeof(*track) && track->transport_version == MOQ_VERSION_DRAFT_16);
        track->media_type = FROZEN_VIDEO; track->packaging = FROZEN_RAW;
        track->transport_version = v ? MOQ_VERSION_DRAFT_18 : MOQ_VERSION_DRAFT_16;
        moq_rcbuf_t *props = NULL;
        MOQ_TEST_CHECK_EQ_INT(moq_rcbuf_create(moq_alloc_default(), properties[v], sizeof(properties[v]), &props), MOQ_OK);
        MOQ_TEST_CHECK(props != NULL);
        if (!props) {
            free(track); free(in); free(out);
            continue;
        }
        moq_media_object_input_init(in);
        MOQ_TEST_CHECK(in->struct_size == sizeof(*in));
        in->payload = payload; in->properties = props;
        moq_media_parsed_object_init(out);
        MOQ_TEST_CHECK(out->struct_size == sizeof(*out) && out->sap_type == MOQ_SAP_UNKNOWN);
        frozen_drop_reason_t reason = FROZEN_STALE;
        MOQ_TEST_CHECK_EQ_INT(moq_media_object_parse(track, in, NULL, 0, out, &reason), MOQ_OK);
        MOQ_TEST_CHECK(out->struct_size == sizeof(*out) && out->has_capture_time && out->capture_time_us == 1000);
        MOQ_TEST_CHECK(out->decode_time_us == 1000 && out->presentation_time_us == 1000 && reason == 0);
        MOQ_TEST_CHECK(out->payload.data == moq_rcbuf_data(payload) && out->payload.len == 1);
        for (size_t t = 0; t < 2; ++t) {
            in->status = t ? MOQ_OBJECT_END_OF_TRACK : MOQ_OBJECT_END_OF_GROUP;
            in->payload = NULL;
            MOQ_TEST_CHECK_EQ_INT(moq_media_object_parse(track, in, NULL, 0, out, &reason), MOQ_OK);
            MOQ_TEST_CHECK(out->status == in->status && out->struct_size == sizeof(*out) && !out->has_capture_time);
        }
        MOQ_TEST_CHECK(moq_rcbuf_refcount(payload) == 1 && moq_rcbuf_refcount(props) == 1);
        moq_rcbuf_decref(props); free(track); free(in); free(out);
    }
    moq_rcbuf_decref(payload);
    printf("old client: track=%zu parsed=%zu, %d failed checks\n",
        sizeof(frozen_track_t), sizeof(frozen_parsed_t), failures);
    return failures != 0;
}
