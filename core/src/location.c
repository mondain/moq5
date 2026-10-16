#include "moq/location.h"
#include "moq/fill.h"
#include <string.h>

_Static_assert(offsetof(moq_location_filter_cfg_t, end_object) +
                   sizeof(uint64_t) == MOQ_LOCATION_FILTER_CFG_V0_SIZE,
               "location filter v0 layout must remain frozen");

void moq_location_filter_cfg_init_sized(moq_location_filter_cfg_t *cfg,
                                       size_t cfg_size)
{
    if (!cfg || cfg_size < sizeof(cfg->struct_size)) return;
    size_t n = cfg_size < sizeof(*cfg) ? cfg_size : sizeof(*cfg);
    memset(cfg, 0, n);
    cfg->struct_size = (uint32_t)n;
}

_Static_assert(offsetof(moq_fill_cfg_t, location) + sizeof(void *) ==
                   MOQ_FILL_CFG_V0_SIZE,
               "fill cfg v0 layout must remain frozen");

void moq_fill_cfg_init_sized(moq_fill_cfg_t *cfg, size_t cfg_size)
{
    if (!cfg || cfg_size < sizeof(cfg->struct_size)) return;
    size_t n = cfg_size < sizeof(*cfg) ? cfg_size : sizeof(*cfg);
    memset(cfg, 0, n);
    cfg->struct_size = (uint32_t)n;
}
