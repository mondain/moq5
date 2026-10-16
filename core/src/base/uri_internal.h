#ifndef MOQ_URI_INTERNAL_H
#define MOQ_URI_INTERNAL_H

/* Source-only RFC3986 grammar shared by session binding and managed clients.
 * Internal linkage: no installed header, exported SPI or cross-DSO dependency.
 * Encoded bytes are validated, never decoded, resolved or normalized here. */
#include <moq/types.h>
#include <string.h>

static inline bool moq_uri_digit(uint8_t c) { return c >= '0' && c <= '9'; }
static inline bool moq_uri_alpha(uint8_t c)
{ return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static inline bool moq_uri_hex(uint8_t c)
{ return moq_uri_digit(c) || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); }
static inline bool moq_uri_unreserved(uint8_t c)
{ return moq_uri_digit(c) || moq_uri_alpha(c) || c == '-' || c == '.' || c == '_' || c == '~'; }
static inline bool moq_uri_subdelim(uint8_t c)
{ return c != 0 && strchr("!$&'()*+,;=", c) != NULL; }

/* Caller has already checked the slice bounds and supplied a non-NULL span. */
static inline moq_bytes_t moq_uri_slice(moq_bytes_t v, size_t start, size_t len)
{
    moq_bytes_t result = {v.data + start, len};
    return result;
}

static inline bool moq_uri_component_valid(moq_bytes_t v, const char *extra,
    bool percent)
{
    if (v.len && !v.data) return false;
    for (size_t i = 0; i < v.len; ++i) {
        uint8_t c = v.data[i];
        if (moq_uri_unreserved(c) || moq_uri_subdelim(c) ||
            (c && strchr(extra, c))) continue;
        if (percent && c == '%' && v.len - i >= 3 &&
            moq_uri_hex(v.data[i+1]) && moq_uri_hex(v.data[i+2])) { i += 2; continue; }
        return false;
    }
    return true;
}

static inline bool moq_uri_ipv4_valid(moq_bytes_t v)
{
    size_t p = 0;
    for (unsigned part = 0; part < 4; ++part) {
        size_t start = p;
        unsigned value = 0;
        while (p < v.len && moq_uri_digit(v.data[p])) {
            if (p - start == 3) return false;
            value = value * 10 + (unsigned)(v.data[p++] - '0');
        }
        if (p == start || value > 255 ||
            (p - start > 1 && v.data[start] == '0')) return false;
        if (part == 3) return p == v.len;
        if (p == v.len || v.data[p++] != '.') return false;
    }
    return false;
}

static inline bool moq_uri_ip_literal_valid(moq_bytes_t v)
{
    if (!v.len) return false;
    if (v.data[0] == 'v' || v.data[0] == 'V') {
        size_t p = 1;
        while (p < v.len && moq_uri_hex(v.data[p])) ++p;
        if (p == 1 || p == v.len || v.data[p++] != '.' || p == v.len) return false;
        return moq_uri_component_valid(moq_uri_slice(v, p, v.len - p), ":", false);
    }
    size_t p = 0;
    unsigned groups = 0;
    bool compressed = false;
    if (v.data[0] == ':') {
        if (v.len < 2 || v.data[1] != ':') return false;
        compressed = true; p = 2;
    }
    while (p < v.len) {
        size_t start = p;
        bool dotted = false;
        while (p < v.len && v.data[p] != ':') {
            if (v.data[p] == '.') dotted = true;
            ++p;
        }
        if (dotted) {
            if (p != v.len || !moq_uri_ipv4_valid(moq_uri_slice(v, start, p - start))) return false;
            groups += 2;
        } else {
            if (p == start || p - start > 4) return false;
            for (size_t i = start; i < p; ++i) if (!moq_uri_hex(v.data[i])) return false;
            ++groups;
        }
        if (groups > 8) return false;
        if (p == v.len) break;
        if (++p == v.len) return false;
        if (v.data[p] == ':') {
            if (compressed) return false;
            compressed = true; ++p;
        }
    }
    return compressed ? groups < 8 : groups == 8;
}

typedef struct moq_uri_authority_parts {
    moq_bytes_t userinfo;
    moq_bytes_t host; /* brackets removed, encoded reg-name unchanged */
    moq_bytes_t port;
    bool userinfo_present;
    bool port_present;
    bool ip_literal;
    bool ipvfuture;
} moq_uri_authority_parts_t;

/* Generic RFC3986 authority allows an empty reg-name, even with userinfo or
 * port. Optional decomposition commits only on success. Scheme host policy
 * belongs in the strict wrapper or endpoint, not this grammar. */
static inline bool moq_uri_authority_parse_generic(moq_bytes_t v,
    moq_uri_authority_parts_t *out)
{
    if (v.len && !v.data) return false;
    moq_uri_authority_parts_t parts;
    memset(&parts, 0, sizeof(parts));
    if (!v.len) {
        parts.host = v;
        if (out) *out = parts;
        return true;
    }
    size_t host = 0;
    for (size_t i = 0; i < v.len; ++i) {
        if (v.data[i] == '@') {
            if (host || !moq_uri_component_valid(moq_uri_slice(v, 0, i), ":", true)) return false;
            parts.userinfo_present = true;
            parts.userinfo = moq_uri_slice(v, 0, i);
            host = i + 1;
        }
    }
    size_t end = host;
    if (host < v.len && v.data[host] == '[') {
        ++end;
        while (end < v.len && v.data[end] != ']') ++end;
        if (end == v.len) return false;
        parts.host = moq_uri_slice(v, host + 1, end - host - 1);
        if (!moq_uri_ip_literal_valid(parts.host)) return false;
        parts.ip_literal = true;
        parts.ipvfuture = parts.host.data[0] == 'v' || parts.host.data[0] == 'V';
        ++end;
    } else {
        while (end < v.len && v.data[end] != ':') ++end;
        parts.host = moq_uri_slice(v, host, end - host);
        if (!moq_uri_component_valid(parts.host, "", true)) return false;
    }
    if (end != v.len) {
        if (v.data[end++] != ':') return false;
        parts.port_present = true;
        parts.port = moq_uri_slice(v, end, v.len - end);
        for (; end < v.len; ++end) if (!moq_uri_digit(v.data[end])) return false;
    }
    if (out) *out = parts;
    return true;
}

/* The MOQT form of an authority: generic syntax plus a nonempty host. The
 * output is written only on acceptance; a syntax or host refusal leaves it
 * untouched. */
static inline bool moq_uri_authority_parse(moq_bytes_t v,
    moq_uri_authority_parts_t *out)
{
    moq_uri_authority_parts_t parts;
    if (!moq_uri_authority_parse_generic(v, &parts) || !parts.host.len) return false;
    if (out) *out = parts;
    return true;
}

static inline bool moq_uri_authority_valid(moq_bytes_t v)
{ return moq_uri_authority_parse(v, NULL); }

static inline bool moq_uri_path_query_valid(moq_bytes_t v)
{
    if (!v.len) return true;
    if (!v.data || (v.data[0] != '/' && v.data[0] != '?')) return false;
    /* '?' terminates path-abempty; subsequent '?' are legal query pchars. */
    return moq_uri_component_valid(v, ":@/?", true);
}

#endif
