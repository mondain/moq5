/*
 * Wire-profile registry: which versions this build speaks and the table for
 * each. The public queries let transports and the service tier offer exactly
 * the versions whose profile is complete, without naming any of them.
 */

#include "session_internal.h"
#include "../spi/core_spi.h"

/* Newest first: the client's offer order is what ALPN and the WebTransport
 * protocol field honour, so this order is what peers see. */
static const moq_version_t registry_versions[] = {
    MOQ_VERSION_DRAFT_22,
    MOQ_VERSION_DRAFT_18,
    MOQ_VERSION_DRAFT_16,
};

const moq_profile_ops_t *moq_profile_lookup(moq_version_t version)
{
    switch (version) {
    case MOQ_VERSION_DRAFT_16:
        return moq_d16_profile_ops();
    case MOQ_VERSION_DRAFT_18:
        return moq_d18_profile_ops();
    case MOQ_VERSION_DRAFT_22:
        return moq_d22_profile_ops();
    default:
        return NULL;
    }
}

size_t moq_profile_versions(moq_version_t *out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < sizeof(registry_versions) / sizeof(registry_versions[0]); i++) {
        if (n >= cap) break;
        out[n++] = registry_versions[i];
    }
    return n;
}

bool moq_version_supported(moq_version_t version)
{
    const moq_profile_ops_t *p = moq_profile_lookup(version);
    return p != NULL && p->wire_ready;
}

bool moq_core_internal_track_max_cache_duration(moq_version_t version,
                                                const uint8_t *props, size_t len,
                                                uint64_t *out_ms)
{
    const moq_profile_ops_t *p = moq_profile_lookup(version);
    if (!p || !p->track_properties_max_cache || !out_ms || (!props && len))
        return false;
    uint64_t ms = 0;
    if (!p->track_properties_max_cache(props, len, &ms)) return false;
    *out_ms = ms;
    return true;
}

moq_result_t moq_profile_check_track_properties(const moq_profile_ops_t *p,
                                                const uint8_t *data, size_t len)
{
    if (len == 0) return MOQ_OK;
    if (!data) return MOQ_ERR_INVAL;
    moq_dt_scan_t dtscan;
    uint8_t priority = 128;
    if ((p->validate_track_properties && p->validate_track_properties(data, len) < 0) ||
        p->scan_delivery_timeouts(data, len, true, &dtscan) < 0 ||
        (p->scan_default_publisher_priority &&
         p->scan_default_publisher_priority(data, len, &priority) < 0))
        return MOQ_ERR_INVAL;
    return MOQ_OK;
}

moq_result_t moq_track_properties_validate(moq_version_t version,
                                           const uint8_t *props, size_t len,
                                           bool dynamic_groups)
{
    if (len > 0 && !props) return MOQ_ERR_INVAL;
    if (!moq_version_supported(version)) return MOQ_ERR_UNSUPPORTED;
    const moq_profile_ops_t *p = moq_profile_lookup(version);
    if (!dynamic_groups) return moq_profile_check_track_properties(p, props, len);
    /* As the facade publishes it: DYNAMIC_GROUPS 1 added in this draft's
     * encoding (two integers, at most 16 bytes in either encoding). */
    if (len > SIZE_MAX - 16) return MOQ_ERR_INVAL;
    size_t cap = len + 16;
    uint8_t *buf = (uint8_t *)moq_alloc_default()->alloc(cap, moq_alloc_default()->ctx);
    if (!buf) return MOQ_ERR_NOMEM;
    size_t n = 0;
    moq_result_t rc = p->track_properties_add_dynamic_groups(props, len, buf, cap, &n);
    if (rc == MOQ_OK) rc = moq_profile_check_track_properties(p, buf, n);
    else rc = MOQ_ERR_INVAL;
    moq_alloc_default()->free(buf, cap, moq_alloc_default()->ctx);
    return rc;
}

size_t moq_versions_supported(moq_version_t *out, size_t cap)
{
    moq_version_t all[sizeof(registry_versions) / sizeof(registry_versions[0])];
    size_t total = moq_profile_versions(all, sizeof(all) / sizeof(all[0]));
    size_t n = 0;
    for (size_t i = 0; i < total; i++) {
        if (!moq_version_supported(all[i])) continue;
        if (out && n < cap) out[n] = all[i];
        n++;
    }
    return n;
}
