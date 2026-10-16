#include "endpoint_uri.h"
#include "../../core/src/base/uri_internal.h"

static bool scheme_is(moq_bytes_t scheme, const char *lower)
{
    if (scheme.len != strlen(lower)) return false;
    for (size_t i = 0; i < scheme.len; ++i) {
        uint8_t c = scheme.data[i];
        if (c >= 'A' && c <= 'Z') c = (uint8_t)(c + ('a' - 'A'));
        if (c != (uint8_t)lower[i]) return false;
    }
    return true;
}

static bool fragment_type_valid(moq_bytes_t fragment)
{
    size_t i = 0;
    while (i < fragment.len && fragment.data[i] != ':') {
        uint8_t c = fragment.data[i++];
        if (!((c >= 'a' && c <= 'z') || moq_uri_digit(c) || c == '-')) return false;
    }
    return i != 0 && i < fragment.len;
}

static bool target_size_valid(moq_bytes_t authority, moq_bytes_t target)
{
    return authority.data && authority.len && (!target.len || target.data) &&
        authority.len <= UINT16_MAX && target.len <= UINT16_MAX - authority.len;
}

moq_result_t moq_endpoint_uri_parse(moq_bytes_t input, moq_endpoint_uri_t *out)
{
    if (!out || !input.data || !input.len || !moq_uri_alpha(input.data[0]))
        return MOQ_ERR_INVAL;
    size_t colon = 1;
    while (colon < input.len && input.data[colon] != ':') {
        uint8_t c = input.data[colon++];
        if (!moq_uri_alpha(c) && !moq_uri_digit(c) && c != '+' && c != '-' && c != '.')
            return MOQ_ERR_INVAL;
    }
    if (colon == input.len) return MOQ_ERR_INVAL;
    moq_endpoint_uri_t parsed;
    memset(&parsed, 0, sizeof(parsed));
    parsed.url.struct_size = (uint32_t)sizeof(parsed.url);
    parsed.url.scheme = moq_uri_slice(input, 0, colon);
    parsed.https = scheme_is(parsed.url.scheme, "https");
    bool supported_scheme = parsed.https || scheme_is(parsed.url.scheme, "moqt");
    /* A scheme the endpoint reads in no URI mode is not a connect URL at all
     * (MOQ_ERR_INVAL, as the legacy reading answers); "moq" is admitted by
     * the legacy reading only, so under this one it is a well-formed URI the
     * mode does not support (MOQ_ERR_UNSUPPORTED). Either way the encoded
     * syntax is validated first, so a malformed URI is INVAL regardless. */
    bool legacy_scheme = scheme_is(parsed.url.scheme, "moq");
    moq_result_t unsupported = legacy_scheme ? MOQ_ERR_UNSUPPORTED : MOQ_ERR_INVAL;

    size_t end = colon + 1;
    while (end < input.len && input.data[end] != '#') ++end;
    bool fragment_present = end != input.len;
    if (fragment_present) {
        moq_bytes_t fragment = moq_uri_slice(input, end + 1, input.len - end - 1);
        if (!moq_uri_component_valid(fragment, ":@/?", true) ||
            (supported_scheme && !fragment_type_valid(fragment))) return MOQ_ERR_INVAL;
    }
    size_t start = colon + 1;
    if (end - start < 2 || input.data[start] != '/' || input.data[start + 1] != '/') {
        /* Other URI schemes can use a non-authority hier-part. They are not
         * connectable here, but their encoded path/query is still validated. */
        if (!supported_scheme && moq_uri_component_valid(
                moq_uri_slice(input, start, end - start), ":@/?", true))
            return unsupported;
        return MOQ_ERR_INVAL;
    }
    start += 2;
    size_t path = start;
    while (path < end && input.data[path] != '/' && input.data[path] != '?') ++path;
    parsed.authority = moq_uri_slice(input, start, path - start);
    parsed.path_query = moq_uri_slice(input, path, end - path);
    moq_uri_authority_parts_t authority;
    if (!moq_uri_path_query_valid(parsed.path_query)) return MOQ_ERR_INVAL;
    if (!moq_uri_authority_parse_generic(parsed.authority, &authority)) return MOQ_ERR_INVAL;
    /* Generic authorities may contain userinfo/port with an empty host. The
     * supported MOQT/HTTPS schemes separately require that host to be nonempty. */
    if (supported_scheme && !authority.host.len) return MOQ_ERR_INVAL;

    size_t query = path;
    while (query < end && input.data[query] != '?') ++query;
    parsed.url.path = moq_uri_slice(input, path, query - path);
    parsed.query_present = query != end;
    if (parsed.query_present) parsed.url.query = moq_uri_slice(input, query + 1, end - query - 1);
    parsed.url.host = authority.host;

    /* All encoded syntax is checked before selecting the managed-client
     * subset. These are endpoint policy refusals, not malformed wire URIs. */
    if (!supported_scheme) return unsupported;
    if (fragment_present || authority.userinfo_present ||
        authority.ipvfuture || memchr(authority.host.data, '%', authority.host.len))
        return MOQ_ERR_UNSUPPORTED;
    unsigned port = 443;
    if (authority.port_present) {
        if (!authority.port.len) return MOQ_ERR_UNSUPPORTED;
        port = 0;
        for (size_t i = 0; i < authority.port.len; ++i) {
            unsigned digit = (unsigned)(authority.port.data[i] - '0');
            if (port > (65535u - digit) / 10u) return MOQ_ERR_UNSUPPORTED;
            port = port * 10u + digit;
        }
        if (!port) return MOQ_ERR_UNSUPPORTED;
    }
    parsed.url.port = (uint16_t)port;
    /* Resource bound from the binding API, separate from syntax validity. */
    if (!target_size_valid(parsed.authority, parsed.path_query)) return MOQ_ERR_INVAL;
    *out = parsed;
    return MOQ_OK;
}

moq_result_t moq_endpoint_uri_clone(const moq_alloc_t *alloc,
    const moq_endpoint_uri_t *uri, bool webtransport,
    moq_endpoint_uri_owned_t *out)
{
    if (!alloc || !alloc->alloc || !alloc->free || !uri || !out ||
        !target_size_valid(uri->authority, uri->path_query)) return MOQ_ERR_INVAL;
    if (out->storage) return MOQ_ERR_WRONG_STATE;
    bool slash = !uri->path_query.len || uri->path_query.data[0] == '?';
    size_t raw = uri->authority.len + 1 + uri->path_query.len + 1;
    size_t http = webtransport ? (size_t)slash + uri->path_query.len + 1 : 0;
    /* The binding bound proves these small additions, including on ILP32. */
    if (http > SIZE_MAX - raw) return MOQ_ERR_INVAL;
    moq_endpoint_uri_owned_t owned;
    memset(&owned, 0, sizeof(owned));
    owned.storage_size = raw + http;
    owned.storage = alloc->alloc(owned.storage_size, alloc->ctx);
    if (!owned.storage) return MOQ_ERR_NOMEM;
    uint8_t *p = owned.storage;
    owned.authority = (moq_bytes_t){p, uri->authority.len};
    memcpy(p, uri->authority.data, uri->authority.len);
    p += uri->authority.len; *p++ = 0;
    owned.path_query = (moq_bytes_t){p, uri->path_query.len};
    if (uri->path_query.len) memcpy(p, uri->path_query.data, uri->path_query.len);
    p += uri->path_query.len; *p++ = 0;
    if (webtransport) {
        owned.wt_path = (char *)p;
        if (slash) *p++ = '/';
        if (uri->path_query.len) memcpy(p, uri->path_query.data, uri->path_query.len);
        p[uri->path_query.len] = 0;
    }
    *out = owned;
    return MOQ_OK;
}

void moq_endpoint_uri_cleanup(const moq_alloc_t *alloc,
    moq_endpoint_uri_owned_t *owned)
{
    if (!owned) return;
    if (owned->storage) alloc->free(owned->storage, owned->storage_size, alloc->ctx);
    memset(owned, 0, sizeof(*owned));
}
