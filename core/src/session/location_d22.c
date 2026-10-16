#include "location_d22_internal.h"
#include "session_internal.h"

moq_result_t moq_d22_from_location_cfg(const moq_location_filter_cfg_t *cfg,
                                     moq_d22_location_filter_t *out)
{
    if (!cfg || !out) return MOQ_ERR_INVAL;
    if (cfg->struct_size < MOQ_LOCATION_FILTER_CFG_V0_SIZE)
        return MOQ_ERR_ABI_MISMATCH;
    moq_d22_location_filter_t value = {0};
    switch (cfg->kind) {
    case MOQ_LOCATION_FILTER_NONE:
        value.type = MOQ_D22_FILTER_NONE;
        break;
    case MOQ_LOCATION_FILTER_RELATIVE_GROUP:
        value.type = MOQ_D22_FILTER_RELATIVE;
        value.start_group = cfg->relative_group_offset;
        break;
    case MOQ_LOCATION_FILTER_ABSOLUTE_START:
        value.type = MOQ_D22_FILTER_ABSOLUTE;
        value.start_group = cfg->start_group;
        value.start_object = cfg->start_object;
        break;
    case MOQ_LOCATION_FILTER_GROUP_END:
    case MOQ_LOCATION_FILTER_OBJECT_END:
        if (cfg->end_group < cfg->start_group) return MOQ_ERR_INVAL;
        value.type = cfg->kind == MOQ_LOCATION_FILTER_GROUP_END ?
            MOQ_D22_FILTER_GROUP_END : MOQ_D22_FILTER_RANGE;
        value.start_group = cfg->start_group;
        value.start_object = cfg->start_object;
        value.end_group_delta = cfg->end_group - cfg->start_group;
        if (cfg->kind == MOQ_LOCATION_FILTER_OBJECT_END)
            value.end_object = cfg->end_object;
        break;
    case MOQ_LOCATION_FILTER_NEXT_OBJECT:
        value.type = MOQ_D22_FILTER_NEXT_OBJECT;
        break;
    default:
        return MOQ_ERR_INVAL;
    }
    *out = value;
    return MOQ_OK;
}

moq_result_t moq_d22_from_subscription_filter(moq_subscribe_filter_t filter,
    uint64_t start_group, uint64_t start_object, uint64_t end_group,
    uint64_t end_object, moq_d22_location_filter_t *out)
{
    if (!out) return MOQ_ERR_INVAL;
    moq_d22_location_filter_t value = {0};
    switch (filter) {
    case MOQ_SUBSCRIBE_FILTER_NONE:
        value.type = MOQ_D22_FILTER_NONE;
        break;
    case MOQ_SUBSCRIBE_FILTER_NEXT_GROUP:
        value.type = MOQ_D22_FILTER_RELATIVE;
        value.start_group = 0;
        break;
    case MOQ_SUBSCRIBE_FILTER_RELATIVE_GROUP_INTERNAL:
        value.type = MOQ_D22_FILTER_RELATIVE;
        value.start_group = start_group;
        break;
    case MOQ_SUBSCRIBE_FILTER_OBJECT_END_INTERNAL:
        if (end_group < start_group) return MOQ_ERR_INVAL;
        value.type = MOQ_D22_FILTER_RANGE;
        value.start_group = start_group;
        value.start_object = start_object;
        value.end_group_delta = end_group - start_group;
        value.end_object = end_object;
        break;
    case MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT:
        value.type = MOQ_D22_FILTER_NEXT_OBJECT;
        break;
    case MOQ_SUBSCRIBE_FILTER_ABSOLUTE_START:
        value.type = MOQ_D22_FILTER_ABSOLUTE;
        value.start_group = start_group;
        value.start_object = start_object;
        break;
    case MOQ_SUBSCRIBE_FILTER_ABSOLUTE_RANGE:
        if (end_group < start_group) return MOQ_ERR_INVAL;
        value.type = MOQ_D22_FILTER_GROUP_END;
        value.start_group = start_group;
        value.start_object = start_object;
        value.end_group_delta = end_group - start_group;
        break;
    default:
        return MOQ_ERR_INVAL;
    }
    *out = value;
    return MOQ_OK;
}

moq_result_t moq_d22_from_fetch_range(uint64_t start_group,
    uint64_t start_object, uint64_t end_group, uint64_t end_object,
    moq_d22_location_filter_t *out)
{
    if (!out || end_group < start_group ||
        (end_group == start_group && end_object != 0 &&
         end_object <= start_object)) return MOQ_ERR_INVAL;
    moq_d22_location_filter_t value = {
        .type = end_object == 0 ? MOQ_D22_FILTER_GROUP_END : MOQ_D22_FILTER_RANGE,
        .start_group = start_group,
        .start_object = start_object,
        .end_group_delta = end_group - start_group,
        .end_object = end_object == 0 ? 0 : end_object - 1
    };
    *out = value;
    return MOQ_OK;
}
