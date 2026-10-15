#ifndef MOQ_TS_LAYOUT_PROBE_H
#define MOQ_TS_LAYOUT_PROBE_H
#include <moq/media_object.h>

/* Baseline fields, independently frozen. Packaging uses the
 * fixed-width type; this does not claim old short-enum binary compatibility. */
typedef struct ts_old_track {
    uint32_t             struct_size;
    moq_media_type_t     media_type;
    uint32_t             packaging;
    uint32_t             timescale;
    moq_version_t        transport_version;
} ts_old_track_t;
typedef struct ts_old_parsed {
    uint32_t              struct_size;
    uint32_t              packaging;
    moq_object_status_t   status;
    bool                  end_of_group;
    bool                  datagram;
    bool                  has_capture_time;
    uint64_t              capture_time_us;
    uint64_t              decode_time_us;
    int64_t               composition_offset_us;
    uint64_t              presentation_time_us;
    bool                  keyframe;
    moq_bytes_t           payload;
    moq_bytes_t           fragment;
    size_t                mdat_offset;
    size_t                mdat_len;
    size_t                      sample_count;
    const moq_cmaf_sample_t    *samples;
    uint32_t                    sample_duration_us;
    moq_sap_type_t              sap_type;
    uint32_t                    chunk_count;
    uint32_t                    track_id;
} ts_old_parsed_t;
_Static_assert(sizeof(ts_old_track_t) == sizeof(moq_media_track_info_t),
               "embedded track info must not grow");
_Static_assert(_Alignof(ts_old_track_t) == _Alignof(moq_media_track_info_t),
               "embedded track alignment must not change");
_Static_assert(MOQ_MEDIA_PARSED_OBJECT_V0_SIZE == sizeof(ts_old_parsed_t),
               "parsed output floor must equal frozen padded sizeof");
_Static_assert(offsetof(ts_old_parsed_t, track_id) ==
               offsetof(moq_media_parsed_object_t, track_id), "parsed tail moved");
_Static_assert(MOQ_MEDIA_OBJECT_PARSE_CFG_V0_SIZE == 8,
               "config floor through companion_profile");
_Static_assert(offsetof(moq_media_object_parse_cfg_t, mpeg2ts_packet_size) >=
               offsetof(moq_media_object_parse_cfg_t, track_loc_metadata) +
               sizeof(void *), "packet size follows the metadata pointer");
#endif
