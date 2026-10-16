#ifndef MOQ_WIRE_CONTROL_D18_INTERNAL_H
#define MOQ_WIRE_CONTROL_D18_INTERNAL_H

/*
 * Internal draft-18 control-codec helpers -- NOT part of the public wire codec
 * surface (no MOQ_API, so hidden from the shared-library dynamic symbol table).
 * Shared between the wire codec (control_d18.c) and the session profile
 * (profile_d18.c) without widening the public ABI.
 */

#include "moq/control_d18.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Pure per-profile timeout scanner (§9.8, internal): extracts
 * OBJECT/SUBGROUP delivery-timeout Track Properties (raw wire ms),
 * searching the mutable list and IMMUTABLE_PROPERTIES contents; duplicates
 * and nested immutable blocks are MOQ_ERR_PROTO; unknown properties pass
 * through. */
moq_result_t moq_d18_scan_delivery_timeouts(const uint8_t *props, size_t len,
                                            bool *out_has_object,
                                            uint64_t *out_object_ms,
                                            bool *out_has_subgroup,
                                            uint64_t *out_subgroup_ms);

/* Track priority extraction shared by D18 and D22. Unknown properties pass
 * through; absence resolves to 128, and values above 255 are invalid. */
/* Strict check of a Track Property set this side sends: structure, no
 * unknown Mandatory Property, DYNAMIC_GROUPS 0 or 1 (also inside Immutable
 * Properties), a readable default publisher priority. MOQ_ERR_PROTO when it
 * fails. */
moq_result_t moq_d18_validate_track_properties(const uint8_t *data, size_t len);
moq_result_t moq_d18_scan_default_publisher_priority(const uint8_t *props,
                                                     size_t len,
                                                     uint8_t *out);

/*
 * Object Property violations that make the Track malformed rather than the
 * session (draft-22 §12.1). The session-level failures stay MOQ_ERR_PROTO:
 * a Delta Type past 2^64-1 and a value Length above 2^16-1 (§8.3), and a
 * DYNAMIC_GROUPS value above 1 (§10.6).
 */
#define MOQ_OBJ_PROPS_OK                   0u
#define MOQ_OBJ_PROPS_UNPARSEABLE          1u  /* a pair overruns the block (§10.7) */
#define MOQ_OBJ_PROPS_NESTED_IMMUTABLE     2u  /* 0x0B inside 0x0B (§10.7) */
#define MOQ_OBJ_PROPS_DUPLICATE_IMMUTABLE  3u  /* two 0x0B on one object (§10.7) */
#define MOQ_OBJ_PROPS_MANDATORY            4u  /* 0x4000-0x7FFF on an object (§3.7) */
#define MOQ_OBJ_PROPS_DUPLICATE_GAP        5u  /* two 0x3C or two 0x3E (§10.8, §10.9) */

typedef struct moq_obj_props_scan {
    uint8_t  malformed;               /* MOQ_OBJ_PROPS_*; the first one found */
    bool     has_prior_group_gap;     /* Prior Group ID Gap (0x3C) */
    bool     has_prior_object_gap;    /* Prior Object ID Gap (0x3E) */
    uint64_t prior_group_gap;
    uint64_t prior_object_gap;
} moq_obj_props_scan_t;

/* Scan an Object Property block, searching the mutable list and the contents
 * of Immutable Properties (§10.7: "processors MUST search both"). Returns
 * MOQ_ERR_PROTO only for the session-level failures named above; every
 * Track-level violation is reported in out->malformed with MOQ_OK. The gap
 * values are reported only when the block is not malformed. */
moq_result_t moq_d18_scan_object_properties(const uint8_t *props, size_t len,
                                            moq_obj_props_scan_t *out);

/* moq_d18_decode_object_datagram with the Object Property outcome split as
 * above: with `track_scoped` set, a Track-level property violation returns
 * MOQ_OK and is reported in *scan; without it the block is validated as the
 * public decoder does (any violation is MOQ_ERR_PROTO) and *scan carries only
 * the gap values. */
moq_result_t moq_d18_decode_object_datagram_scan(const uint8_t *data, size_t len,
                                                 bool track_scoped,
                                                 moq_d18_object_datagram_t *out,
                                                 moq_obj_props_scan_t *scan);

/*
 * REQUEST_UPDATE_OK codec (§10.5): the REQUEST_OK form that carries
 * LARGEST_OBJECT / EXPIRES response parameters with empty Track Properties.
 * The zero-parameter moq_d18_{encode,decode}_request_ok in the public header
 * stay for the other REQUEST_OK responses.
 */
moq_result_t moq_d18_encode_request_update_ok(moq_buf_writer_t *w,
                                              const moq_d18_msg_params_t *p);
moq_result_t moq_d18_decode_request_update_ok(const uint8_t *payload,
                                              size_t payload_len,
                                              moq_d18_msg_params_t *out);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_WIRE_CONTROL_D18_INTERNAL_H */
