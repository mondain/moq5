#ifndef MOQ_MSF_INTERNAL_H
#define MOQ_MSF_INTERNAL_H

/*
 * Functions of moq-msf that are not part of its public API, shared between
 * its sources. They are not exported from a shared build.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Whether `len` octets at `data` are whole MPEG-2 TS source packets of
 * `packet_size` octets (188, or 192 with a 4-octet prefix), each with its
 * sync byte, and at least one: the rule draft-gregoire-moq-msfts sets for
 * an mpeg2ts track's initialization data. */
bool moq_msf_ts_whole_packets(const uint8_t *data, size_t len,
                              uint32_t packet_size);

#endif
