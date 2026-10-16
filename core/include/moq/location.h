#ifndef MOQ_LOCATION_H
#define MOQ_LOCATION_H

#include "types.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef uint32_t moq_location_filter_kind_t;

#define MOQ_LOCATION_FILTER_NONE           0u
#define MOQ_LOCATION_FILTER_RELATIVE_GROUP 1u
#define MOQ_LOCATION_FILTER_ABSOLUTE_START 2u
#define MOQ_LOCATION_FILTER_GROUP_END      3u
#define MOQ_LOCATION_FILTER_OBJECT_END     4u
#define MOQ_LOCATION_FILTER_NEXT_OBJECT    5u

/* Semantic location filter, independent of the legacy subscription-filter
 * codes. An absent option means "unspecified"; an explicit NONE removes the
 * location constraint. The enclosing operation defines omission/inheritance.
 *
 * NONE and NEXT_OBJECT use no fields. RELATIVE_GROUP uses only
 * relative_group_offset: zero selects the next group, one the current group,
 * two the preceding group. The computed group is clamped to [0, UINT64_MAX].
 * ABSOLUTE_START uses start_group/start_object and is open-ended.
 * GROUP_END additionally includes every object through end_group.
 * OBJECT_END additionally uses end_object, INCLUSIVE, including UINT64_MAX.
 * With equal start/end groups, end_object < start_object represents an empty
 * filter; it is not an invalid encoding. An end_group below start_group is
 * invalid because the wire requires a nonnegative end-group delta.
 * End groups are absolute, never wire deltas. Inactive fields are ignored.
 * Fetch operations bound an open end by their Largest Object snapshot;
 * subscription operations leave it open. Fetch no-content/start-after-Largest
 * refusal and empty-result rules apply independently of representability.
 * A filter end does not itself end a subscription. Resolution and validation
 * occur in the consuming operation.
 *
 * This value owns no resources. A configuration pointer borrows for the call;
 * an operation that retains it must copy the selected values before returning.
 */
typedef struct moq_location_filter_cfg {
    uint32_t struct_size;
    moq_location_filter_kind_t kind;
    uint64_t relative_group_offset;
    uint64_t start_group;
    uint64_t start_object;
    uint64_t end_group;
    uint64_t end_object;
} moq_location_filter_cfg_t;

#define MOQ_LOCATION_FILTER_CFG_V0_SIZE 48u

/* Zeroes/stamps min(cfg_size, library sizeof), with NONE as the default.
 * NULL, or storage smaller than struct_size, is untouched. A consuming call
 * rejects a stamped prefix smaller than the frozen v0 size. */
MOQ_API void moq_location_filter_cfg_init_sized(moq_location_filter_cfg_t *cfg,
                                               size_t cfg_size);

/* Header-side size preserves the caller's ABI when the library grows. */
static inline void moq_location_filter_cfg_init(moq_location_filter_cfg_t *cfg)
{
    moq_location_filter_cfg_init_sized(cfg, sizeof(*cfg));
}

#ifdef __cplusplus
}
#endif

#endif
