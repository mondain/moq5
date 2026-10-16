#ifndef MOQ_WIRE_KVP_INTERNAL_H
#define MOQ_WIRE_KVP_INTERNAL_H

#include <moq/types.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Copy the delta-typed Key-Value-Pair list `in` (types ascending, the first
 * delta from zero) to `out` with the even-typed integer pair {type, value}
 * added at its place in type order. Integers are QUIC varints, or vi64 when
 * `vi64` is set. A list that already holds `type` at its top level is copied
 * unchanged when that pair's value is `value`, and is MOQ_ERR_INVAL
 * otherwise. A list that does not parse is MOQ_ERR_INVAL; an `out` too small
 * is MOQ_ERR_BUFFER. Nested lists are copied byte for byte. Internal to the
 * core. */
moq_result_t moq_kvp_list_add_int(const uint8_t *in, size_t len, bool vi64,
                                  uint64_t type, uint64_t value,
                                  uint8_t *out, size_t cap, size_t *out_len);

/* The same edit for a property whose instances are counted across both the
 * list and the bodies of its odd-typed `container` pairs (an Immutable
 * Properties block, whose properties are looked up together with the list's
 * own): none adds {type, value} to the list as above; exactly one with the
 * value `value` copies the list unchanged; one with another value, or more
 * than one, is MOQ_ERR_INVAL. The container's bytes are never rewritten.
 * `container` 0 counts the list alone. */
moq_result_t moq_kvp_list_merge_int(const uint8_t *in, size_t len, bool vi64,
                                    uint64_t type, uint64_t value, uint64_t container,
                                    uint8_t *out, size_t cap, size_t *out_len);

#endif
