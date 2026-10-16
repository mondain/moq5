#ifndef MOQ_CORE_SPI_H
#define MOQ_CORE_SPI_H

/*
 * Same-build interface between libmoq-core and the relay built with it. Each
 * symbol here is exported from the core library so a shared relay library
 * can call it, which makes it a private same-build ABI: a binary dependency
 * between a core and a relay built together, not a stable or public ABI. No
 * installed header declares it, and it changes with the core it ships beside.
 * tests/cmake/check_core_spi_symbols.cmake pins the exact exported set. Wire
 * decoding stays in the core; the relay receives values, never wire forms.
 */

#include <moq/export.h>
#include <moq/session.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The MAX_CACHE_DURATION Track Property (0x04: draft-22 §10.3, draft-18
 * §12.3, draft-16 §11.1.1) in a Track Property set encoded for `version`.
 * Draft-18 and draft-22 search Immutable Properties as well, as their text
 * requires. Draft-16 reads the top-level set only: its Immutable Extensions
 * (§11.2) hold Track Extensions a relay "MAY decode and view", so one found
 * only inside them is not applied. True with its milliseconds in *out_ms when
 * present; false when absent, for an unsupported version, or past a point
 * where the set does not parse. A relay "MUST NOT start forwarding any
 * individual Object received through this subscription or fetch after the
 * specified number of milliseconds has elapsed since the beginning of the
 * Object was received". */
MOQ_API bool moq_core_internal_track_max_cache_duration(moq_version_t version,
                                                        const uint8_t *props,
                                                        size_t len,
                                                        uint64_t *out_ms);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_CORE_SPI_H */
