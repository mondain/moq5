#ifndef MOQ_FILL_RANGE_INTERNAL_H
#define MOQ_FILL_RANGE_INTERNAL_H

#include "moq/types.h"

typedef enum moq_fill_range_outcome {
    MOQ_FILL_RANGE_STREAM = 1,
    MOQ_FILL_RANGE_NO_STREAM_PAUSED,
    MOQ_FILL_RANGE_NO_STREAM_NO_LARGEST,
    MOQ_FILL_RANGE_NO_STREAM_EMPTY,
    MOQ_FILL_RANGE_NO_STREAM_AFTER_LARGEST
} moq_fill_range_outcome_t;

/* Absolute inclusive endpoints, meaningful only for STREAM. Every NO_STREAM
 * result has zero coordinates. This value retains no input or parent pointers. */
typedef struct moq_fill_range {
    moq_fill_range_outcome_t outcome;
    uint64_t start_group;
    uint64_t start_object;
    uint64_t end_group;
    uint64_t end_object;
} moq_fill_range_t;

/* Resolve one effective fill filter, in the core's form (a subscription filter
 * type with its raw locations, the internal relative-group and object-end
 * forms included; `end_object` is inclusive and read for the object-end form
 * only), against one explicit Largest snapshot. Inheritance, authorization,
 * range filters and fill ownership are external. Largest is a location bound,
 * not evidence that every selected object exists. This is NOT standalone FETCH
 * acceptance or its INVALID_RANGE/empty-stream rule.
 *
 * A null output returns INVAL; an unknown filter type returns PROTO before any
 * paused/no-content shortcut; refusal preserves out. Absent Largest
 * coordinates are ignored. NO_STREAM precedence: PAUSED, NO_LARGEST, explicit
 * EMPTY, AFTER_LARGEST. The next-object form always selects AFTER_LARGEST when
 * a non-paused snapshot exists, including Largest.Object == UINT64_MAX, without
 * computing a wrapped location. Relative arithmetic clamps the whole
 * mathematical (Largest.Group+1-offset). A group end denotes every object of
 * that group before clipping by Largest. A successful call replaces out; it
 * never reads track state or opens a stream. */
moq_result_t moq_fill_range_resolve(uint32_t filter_type, uint64_t start_group,
    uint64_t start_object, uint64_t end_group, uint64_t end_object,
    bool has_largest, uint64_t largest_group, uint64_t largest_object,
    bool paused, moq_fill_range_t *out);

#endif
