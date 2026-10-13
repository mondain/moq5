#ifndef MOQ_SESSION_PROFILE_D22_INTERNAL_H
#define MOQ_SESSION_PROFILE_D22_INTERNAL_H

/*
 * Draft-22 profile helpers shared with tests -- NOT part of the public surface
 * (no MOQ_API, so hidden from the shared-library dynamic symbol table).
 *
 * They translate between the session core's filter model (one of four
 * MOQ_SUBSCRIBE_FILTER_* types plus locations, and a fetch range of [start, end)
 * with end_object 0 meaning "the whole end group") and draft 22's Location Filter
 * (9.20.9), FILL_PARAMETERS (9.20.15) and Range Filters (9.20.10-14). See the
 * comments on each definition in profile_d22.c for the exact mapping.
 */

#include "session_internal.h"
#include "moq/control_d22.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Core filter -> Location Filter. Each valid semantic filter is emitted;
 * MOQ_ERR_INVAL for an unknown type or an end group before the start group. */
moq_result_t moq_d22_profile_filter_to_wire(uint32_t filter, uint64_t start_group,
                                            uint64_t start_object, uint64_t end_group,
                                            moq_d22_location_filter_t *out, bool *emit);

/* Location Filter -> core filter; *approximated is set when the four types cannot
 * express it exactly. */
void moq_d22_profile_filter_from_wire(const moq_d22_location_filter_t *f,
                                      uint32_t *type, uint64_t *start_group,
                                      uint64_t *start_object, uint64_t *end_group,
                                      bool *approximated);

void moq_d22_profile_surface_loc(moq_decoded_loc_filter_t *out,
                                 const moq_d22_location_filter_t *f, bool present,
                                 bool approximated);

/* Fill a decoded SUBSCRIBE / REQUEST_UPDATE's filter fields and raw surfaces. */
void moq_d22_profile_surface_subscription_filters(
    const moq_d22_msg_params_t *p, bool update,
    bool *has_filter, uint32_t *filter_type, uint64_t *start_group,
    uint64_t *start_object, uint64_t *end_group,
    moq_decoded_loc_filter_t *loc, moq_decoded_fill_t *fill,
    moq_decoded_range_filters_t *ranges);

/* Core fetch range -> Location Filter, and back. */
moq_result_t moq_d22_profile_fetch_range_to_wire(uint64_t sg, uint64_t so,
                                                 uint64_t eg, uint64_t eo,
                                                 moq_d22_location_filter_t *out);
void moq_d22_profile_fetch_range_from_wire(const moq_d22_msg_params_t *p,
                                           uint64_t *sg, uint64_t *so, uint64_t *eg,
                                           uint64_t *eo, bool *approximated);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_SESSION_PROFILE_D22_INTERNAL_H */
