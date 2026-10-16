#ifndef MOQ_SERVICE_ENDPOINT_URI_H
#define MOQ_SERVICE_ENDPOINT_URI_H

#include <moq/endpoint.h>
#include <moq/url.h>

/* -- MOQT URI grammar (RFC 3986 syntax; "moqt" and "https" schemes) ---- *
 * The parse validates encoded syntax and separates the components a session
 * binding carries: the authority as spelled and the path-abempty with "?"
 * query when present. Nothing is decoded or normalized. Private, borrowed
 * parse output; presence is not inferred from span length. */
typedef struct moq_endpoint_uri {
    moq_url_t url;
    moq_bytes_t authority;
    moq_bytes_t path_query;
    bool query_present;
    bool https;
} moq_endpoint_uri_t;

moq_result_t moq_endpoint_uri_parse(moq_bytes_t input, moq_endpoint_uri_t *out);

/* Owned encoded target, with NUL-terminated component storage for managed
 * providers. wt_path is NULL for raw QUIC; otherwise it is the HTTP projection.
 * The allocator outlives this object. Start with a zeroed output; cloning over
 * owned storage is WRONG_STATE. A failed clone leaves *out unchanged. */
typedef struct moq_endpoint_uri_owned {
    uint8_t *storage;
    size_t storage_size;
    moq_bytes_t authority;
    moq_bytes_t path_query;
    char *wt_path;
} moq_endpoint_uri_owned_t;

moq_result_t moq_endpoint_uri_clone(const moq_alloc_t *alloc,
    const moq_endpoint_uri_t *uri, bool webtransport,
    moq_endpoint_uri_owned_t *out);
void moq_endpoint_uri_cleanup(const moq_alloc_t *alloc,
    moq_endpoint_uri_owned_t *owned);

#endif
