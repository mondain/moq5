#ifndef MOQ_SESSION_LOCATION_D22_INTERNAL_H
#define MOQ_SESSION_LOCATION_D22_INTERNAL_H

#include "moq/session.h"
#include "moq/location.h"
#include "../wire/control_d22_internal.h"

/* Translate the existing semantic API, not its draft-16/18 wire codes.
 * Inactive input fields are ignored; refusal leaves the output unchanged.
 * These helpers do not enable a profile or turn Joining FETCH into a fill. */
moq_result_t moq_d22_from_subscription_filter(moq_subscribe_filter_t filter,
    uint64_t start_group, uint64_t start_object, uint64_t end_group,
    uint64_t end_object, moq_d22_location_filter_t *out);

/* Existing FETCH cfg: exclusive EndObject, with zero meaning the whole end
 * group. Draft-22: inclusive EndObject, with a distinct whole-group form. */
moq_result_t moq_d22_from_fetch_range(uint64_t start_group,
    uint64_t start_object, uint64_t end_group, uint64_t end_object,
    moq_d22_location_filter_t *out);

/* Sized semantic option to draft-22 wire value. End group is absolute in the
 * cfg and delta-encoded only here. No resolution against mutable track state. */
moq_result_t moq_d22_from_location_cfg(const moq_location_filter_cfg_t *cfg,
                                     moq_d22_location_filter_t *out);

#endif
