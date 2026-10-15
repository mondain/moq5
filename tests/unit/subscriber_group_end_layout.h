#ifndef MOQ_SUB_GROUP_END_LAYOUT_H
#define MOQ_SUB_GROUP_END_LAYOUT_H
#include <moq/subscriber.h>
/* Frozen pre-TS layout, including ABI-specific tail padding. */
typedef struct {
    uint32_t struct_size, max_tracks, max_objects;
    moq_sub_callbacks_t callbacks;
    uint32_t max_fetches, max_fetch_items;
    bool streaming_objects;
    uint32_t max_chunks;
    void (*on_goaway)(void *, moq_bytes_t);
    void (*on_subscribe_done)(void *, moq_sub_track_t *, uint64_t);
    uint64_t max_queued_object_bytes;
    void (*on_update_ok)(void *, moq_sub_track_t *, const moq_sub_update_result_t *);
} sub_cfg_before_ts_t;
_Static_assert(offsetof(moq_sub_cfg_t, on_update_ok) ==
               offsetof(sub_cfg_before_ts_t, on_update_ok),
               "the pre-TS callback must not move");
_Static_assert(offsetof(moq_sub_cfg_t, surface_group_ends) >=
               sizeof(sub_cfg_before_ts_t),
               "group ends must not read old caller padding");
#endif
