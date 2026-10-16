#ifndef MOQ_INTEROP_ENDPOINT_ERROR_H
#define MOQ_INTEROP_ENDPOINT_ERROR_H

#include <moq/types.h>

#include <stddef.h>
#include <stdint.h>

/* The symbolic name of a result code ("MOQ_ERR_UNSUPPORTED"), or NULL for a
 * code this tool does not know. */
const char *interop_result_name(moq_result_t rc);

/* Formats why moq_endpoint_connect refused, for the TAP message of the step
 * that needed the endpoint: "failed to create <what>: <description> (<name>
 * <code>)". When the connect refused with MOQ_ERR_UNSUPPORTED and a draft was
 * pinned (draft_pin != 0), a hint follows: the endpoint refuses an explicit
 * offer the selected transport backend cannot host. The text contains no
 * double quote, so it can sit inside the YAML message string. Always
 * NUL-terminates a non-empty buffer; returns buf. */
char *interop_endpoint_error(char *buf, size_t cap, const char *what,
                             moq_result_t rc, uint32_t draft_pin);

#endif
