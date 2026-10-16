#include "fill_input_internal.h"
#include "session_internal.h"

moq_result_t moq_fill_input_normalize(const moq_fill_cfg_t *cfg,
                                      moq_fill_input_t *out)
{
    if (!out) return MOQ_ERR_INVAL;
    moq_fill_input_t value = {0};
    if (cfg) {
        if (cfg->struct_size < MOQ_FILL_CFG_V0_SIZE)
            return MOQ_ERR_ABI_MISMATCH;
        value.requested = true;
        if (cfg->has_subscriber_priority) {
            value.has_subscriber_priority = true;
            value.subscriber_priority = cfg->subscriber_priority;
        }
        if (cfg->has_group_order) {
            if (cfg->group_order != MOQ_GROUP_ORDER_ASCENDING &&
                cfg->group_order != MOQ_GROUP_ORDER_DESCENDING)
                return MOQ_ERR_INVAL;
            value.has_group_order = true;
            value.group_order = cfg->group_order;
        }
        if (cfg->has_fill_timeout) {
            value.has_fill_timeout = true;
            value.fill_timeout_ms = cfg->fill_timeout_ms;
        }
        if (cfg->location) {
            moq_result_t rc = moq_location_filter_to_core(cfg->location,
                &value.filter_type, &value.start_group, &value.start_object,
                &value.end_group, &value.end_object);
            if (rc != MOQ_OK) return rc;
            value.has_location = true;
        }
    }
    *out = value;
    return MOQ_OK;
}

moq_result_t moq_fill_handle_encode(uint16_t tag, size_t slot,
    uint64_t generation, moq_fill_t *out)
{
    if (!out || !tag || slot > MOQ_FILL_PUBLIC_SLOT_MAX ||
        !generation || generation > MOQ_FILL_PUBLIC_GENERATION_MAX)
        return MOQ_ERR_INVAL;
    uint64_t odd_generation = generation * 2 - 1;
    moq_fill_t value = {moq_handle_pack(MOQ_HANDLE_POOL_FILL, tag,
        (uint32_t)odd_generation, (uint32_t)slot)};
    *out = value;
    return MOQ_OK;
}

moq_result_t moq_fill_handle_decode(uint16_t tag, moq_fill_t handle,
    moq_fill_identity_t *out)
{
    if (!out || !tag) return MOQ_ERR_INVAL;
    if (!moq_fill_is_valid(handle)) return MOQ_ERR_STALE_HANDLE;
    if (moq_handle_session_tag(handle._opaque) != tag) return MOQ_ERR_WRONG_SESSION;
    moq_fill_identity_t value = {
        .slot = moq_handle_slot(handle._opaque),
        .generation = ((uint64_t)moq_handle_generation(handle._opaque) + 1) / 2
    };
    *out = value;
    return MOQ_OK;
}
