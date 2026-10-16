#include "fill_range_internal.h"
#include "session_internal.h"

static bool after(uint64_t group, uint64_t object, uint64_t other_group, uint64_t other_object)
{
    return group > other_group || (group == other_group && object > other_object);
}

static uint64_t relative_start_group(uint64_t largest, uint64_t offset)
{
    if (offset == 0) return largest == UINT64_MAX ? UINT64_MAX : largest + 1;
    /* Rearrange the mathematical sum before evaluation; saturating largest+1
     * first would lose a group at UINT64_MAX for every nonzero offset. */
    uint64_t delta = offset - 1;
    return delta > largest ? 0 : largest - delta;
}

moq_result_t moq_fill_range_resolve(uint32_t filter_type, uint64_t f_start_group,
    uint64_t f_start_object, uint64_t f_end_group, uint64_t f_end_object,
    bool has_largest, uint64_t largest_group, uint64_t largest_object,
    bool paused, moq_fill_range_t *out)
{
    if (!out) return MOQ_ERR_INVAL;
    bool bounded_end = false;
    bool whole_end_group = false;
    bool relative = false, next_object = false, absolute = false;
    switch (filter_type) {
    case MOQ_SUBSCRIBE_FILTER_NONE:
        break;
    case MOQ_SUBSCRIBE_FILTER_NEXT_GROUP:
    case MOQ_SUBSCRIBE_FILTER_RELATIVE_GROUP_INTERNAL:
        relative = true; break;
    case MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT:
        next_object = true; break;
    case MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START:
        absolute = true; break;
    case MOQ_SUBSCRIBE_FILTER_ABSOLUTE_RANGE:
        absolute = true; bounded_end = true; whole_end_group = true; break;
    case MOQ_SUBSCRIBE_FILTER_OBJECT_END_INTERNAL:
        absolute = true; bounded_end = true; break;
    default:
        return MOQ_ERR_PROTO;
    }
    if (bounded_end && f_end_group < f_start_group) return MOQ_ERR_PROTO;

    moq_fill_range_t value = { .outcome = MOQ_FILL_RANGE_STREAM };
    if (paused) {
        value.outcome = MOQ_FILL_RANGE_NO_STREAM_PAUSED;
    } else if (!has_largest) {
        value.outcome = MOQ_FILL_RANGE_NO_STREAM_NO_LARGEST;
    } else if (next_object) {
        value.outcome = MOQ_FILL_RANGE_NO_STREAM_AFTER_LARGEST;
    } else {
        uint64_t start_group = 0, start_object = 0;
        if (relative) {
            /* The next-group form is the relative form at offset zero. */
            uint64_t offset = filter_type == MOQ_SUBSCRIBE_FILTER_NEXT_GROUP ? 0 : f_start_group;
            start_group = relative_start_group(largest_group, offset);
        } else if (absolute) {
            start_group = f_start_group;
            start_object = f_start_object;
        }
        uint64_t end_group, end_object;
        if (!bounded_end) {
            end_group = largest_group;
            end_object = largest_object;
        } else {
            end_group = f_end_group;
            end_object = whole_end_group ? UINT64_MAX : f_end_object;
        }
        if (bounded_end && after(start_group, start_object, end_group, end_object)) {
            value.outcome = MOQ_FILL_RANGE_NO_STREAM_EMPTY;
        } else if (after(start_group, start_object, largest_group, largest_object)) {
            value.outcome = MOQ_FILL_RANGE_NO_STREAM_AFTER_LARGEST;
        } else {
            if (after(end_group, end_object, largest_group, largest_object)) {
                end_group = largest_group;
                end_object = largest_object;
            }
            value.start_group = start_group;
            value.start_object = start_object;
            value.end_group = end_group;
            value.end_object = end_object;
        }
    }
    *out = value;
    return MOQ_OK;
}
