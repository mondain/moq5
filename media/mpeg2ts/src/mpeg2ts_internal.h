#ifndef MOQ_MPEG2TS_INTERNAL_H
#define MOQ_MPEG2TS_INTERNAL_H

/*
 * Functions of moq-mpeg2ts that are not part of its public API: the library
 * uses them to author and read PSI sections, and its own tests call them
 * directly. They are not exported from a shared build.
 */

#include <moq/mpeg2ts.h>

#ifdef __cplusplus
extern "C" {
#endif

/* CRC_32 as used by PSI sections (ISO/IEC 13818-1 Annex A: polynomial
 * 0x04C11DB7, initial value 0xFFFFFFFF, no reflection, no final XOR). A whole
 * section including its CRC_32 field yields 0. Reads only constant data. */
uint32_t moq_mpeg2ts_crc32(const uint8_t *data, size_t len);

/*
 * Collect the CA_PID of every CA_descriptor (tag 0x09, 2.6.16) in a
 * descriptor loop. *count is the number found; at most cap are written.
 * Returns MOQ_ERR_BUFFER when more than cap were found, MOQ_ERR_PROTO when a
 * descriptor overruns the loop or a CA_descriptor is shorter than four
 * octets.
 */
moq_result_t moq_mpeg2ts_ca_pids(moq_bytes_t descriptors, uint16_t *pids,
                                 size_t cap, size_t *count);

/*
 * Build a single-section PAT listing `programs` into `out`, with CRC_32.
 * Returns MOQ_ERR_BUFFER (with *out_len the size needed) when out is too
 * small, MOQ_ERR_INVAL for a program_number of 0, a PID above 0x1FFF, a
 * version above 31, or a section that would exceed 1024 octets.
 */
moq_result_t moq_mpeg2ts_build_pat(uint16_t transport_stream_id,
                                   uint8_t version,
                                   const moq_mpeg2ts_program_t *programs,
                                   size_t program_count,
                                   uint8_t *out, size_t cap, size_t *out_len);

#ifdef __cplusplus
}
#endif

#endif /* MOQ_MPEG2TS_INTERNAL_H */
