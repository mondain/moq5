#ifndef MOQ_FILL_H
#define MOQ_FILL_H

#include "location.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Per-request fill overrides, not sticky subscription state. A NULL option
 * requests no fill; a non-NULL option requests one even with no overrides.
 * Omitted priority/order/location inherit the enclosing subscription's
 * effective state at request processing, not at subsequent packet arrival.
 * group_order uses the MOQ_GROUP_ORDER_ASCENDING (1) / DESCENDING (2)
 * constants from session.h. A present DEFAULT (0) is invalid.
 *
 * An absent timeout selects the implementation's upstream wait policy;
 * explicit zero permits only immediately available objects. This is never
 * a child-lifetime/late-header expiry timer. Inactive scalar values are ignored.
 * A NULL location inherits; explicit NONE removes the location constraint.
 * The location pointer borrows for the consuming call only; retained selected
 * values must be copied. Positive range filters are not represented here.
 * This value alone does not enable a protocol profile or issue a request. */
typedef struct moq_fill_cfg {
    uint32_t struct_size;
    bool has_subscriber_priority;
    uint8_t subscriber_priority;
    bool has_group_order;
    bool has_fill_timeout;
    uint64_t fill_timeout_ms;
    uint32_t group_order;
    const moq_location_filter_cfg_t *location;
} moq_fill_cfg_t;

#if UINTPTR_MAX == UINT64_MAX
#define MOQ_FILL_CFG_V0_SIZE 32u
#elif UINTPTR_MAX == UINT32_MAX
#define MOQ_FILL_CFG_V0_SIZE 24u
#else
#error "Unsupported pointer width for moq_fill_cfg_t"
#endif

/* Zeroes/stamps min(cfg_size, library sizeof). NULL or fewer than four bytes
 * is untouched. Consuming calls require at least the frozen v0 prefix;
 * initializers accept partial storage without claiming it is consumable. */
MOQ_API void moq_fill_cfg_init_sized(moq_fill_cfg_t *cfg, size_t cfg_size);

static inline void moq_fill_cfg_init(moq_fill_cfg_t *cfg)
{
    moq_fill_cfg_init_sized(cfg, sizeof(*cfg));
}

#ifdef __cplusplus
}
#endif

#endif
