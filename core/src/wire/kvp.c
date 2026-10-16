#include "moq/kvp.h"
#include "moq/wire.h"
#include "moq/buf.h"
#include "kvp_internal.h"
#include <string.h>

/* -- Decoder ------------------------------------------------------- */

void moq_kvp_decoder_init(moq_kvp_decoder_t *dec,
                           const uint8_t *buf, size_t len)
{
    dec->buf       = buf;
    dec->remaining = len;
    dec->prev_type = 0;
    dec->first     = true;
}

moq_result_t moq_kvp_decode_next(moq_kvp_decoder_t *dec,
                                  moq_kvp_entry_t *entry)
{
    if (!dec || !entry)
        return MOQ_ERR_INVAL;

    if (dec->remaining == 0)
        return MOQ_DONE;

    /* Remember start position for raw span. */
    const uint8_t *entry_start = dec->buf;

    /* Decode delta type. */
    uint64_t delta = 0;
    size_t n = moq_quic_varint_decode(dec->buf, dec->remaining, &delta);
    if (n == 0)
        return MOQ_ERR_PROTO;
    dec->buf       += n;
    dec->remaining -= n;

    /* Compute absolute type. */
    uint64_t abs_type;
    if (dec->first) {
        abs_type   = delta;
        dec->first = false;
    } else {
        if (delta > UINT64_MAX - dec->prev_type)
            return MOQ_ERR_PROTO;
        abs_type = dec->prev_type + delta;
    }
    dec->prev_type = abs_type;

    entry->type = abs_type;
    entry->is_varint = ((abs_type & 1u) == 0);

    if (entry->is_varint) {
        /* Even type: value is a single varint. The caller decodes it from
         * value/value_len with moq_quic_varint_decode -- the entry exposes the
         * canonical encoded bytes, not a pre-decoded integer. */
        uint64_t val = 0;
        n = moq_quic_varint_decode(dec->buf, dec->remaining, &val);
        if (n == 0)
            return MOQ_ERR_PROTO;
        entry->value     = dec->buf;
        entry->value_len = n;
        dec->buf       += n;
        dec->remaining -= n;
    } else {
        /* Odd type: length-prefixed byte field. */
        uint64_t vlen = 0;
        n = moq_quic_varint_decode(dec->buf, dec->remaining, &vlen);
        if (n == 0)
            return MOQ_ERR_PROTO;
        dec->buf       += n;
        dec->remaining -= n;

        if (vlen > 0xFFFF)
            return MOQ_ERR_PROTO;
        if (vlen > dec->remaining)
            return MOQ_ERR_PROTO;

        entry->value     = dec->buf;
        entry->value_len = (size_t)vlen;
        dec->buf       += (size_t)vlen;
        dec->remaining -= (size_t)vlen;
    }

    /* Raw span covers the entire encoded entry for byte-exact forwarding. */
    entry->raw     = entry_start;
    entry->raw_len = (size_t)(dec->buf - entry_start);

    return MOQ_OK;
}

/* -- Encode validation --------------------------------------------- */

/*
 * Validate entry fields for encoding. Returns false on any inconsistency.
 */
static bool kvp_entry_valid_for_encode(const moq_kvp_entry_t *entry)
{
    /* is_varint must match type parity. */
    bool should_be_varint = ((entry->type & 1u) == 0);
    if (entry->is_varint != should_be_varint)
        return false;

    if (entry->is_varint) {
        /* Even type: value must be exactly one valid QUIC varint. */
        if (entry->value_len == 0)
            return false;
        if (!entry->value)
            return false;
        uint64_t v = 0;
        size_t n = moq_quic_varint_decode(entry->value, entry->value_len, &v);
        if (n != entry->value_len)
            return false;
    } else {
        /* Odd type: value_len <= 2^16-1, and if > 0, value must not be NULL. */
        if (entry->value_len > 0xFFFF)
            return false;
        if (entry->value_len > 0 && !entry->value)
            return false;
    }

    return true;
}

/* -- Encode -------------------------------------------------------- */

size_t moq_kvp_entry_encoded_len(uint64_t prev_type,
                                  const moq_kvp_entry_t *entry)
{
    if (!entry)
        return 0;
    if (entry->type < prev_type)
        return 0;
    if (!kvp_entry_valid_for_encode(entry))
        return 0;

    uint64_t delta = entry->type - prev_type;
    size_t total = moq_quic_varint_len(delta);
    if (total == 0)
        return 0;

    if (entry->is_varint) {
        total += entry->value_len;
    } else {
        size_t len_len = moq_quic_varint_len(entry->value_len);
        if (len_len == 0)
            return 0;
        total += len_len + entry->value_len;
    }

    return total;
}

size_t moq_kvp_encode_entry(uint64_t prev_type,
                             const moq_kvp_entry_t *entry,
                             uint8_t *buf, size_t buf_len)
{
    if (!entry || !buf)
        return 0;

    size_t needed = moq_kvp_entry_encoded_len(prev_type, entry);
    if (needed == 0 || buf_len < needed)
        return 0;

    uint64_t delta = entry->type - prev_type;
    size_t pos = 0;

    pos += moq_quic_varint_encode(delta, buf + pos, buf_len - pos);

    if (entry->is_varint) {
        memcpy(buf + pos, entry->value, entry->value_len);
        pos += entry->value_len;
    } else {
        pos += moq_quic_varint_encode(entry->value_len, buf + pos, buf_len - pos);
        if (entry->value_len > 0) {
            memcpy(buf + pos, entry->value, entry->value_len);
            pos += entry->value_len;
        }
    }

    return pos;
}

/* -- Varint convenience encode ------------------------------------- */

static bool kvp_varint_args_valid(uint64_t prev_type,
                                   uint64_t type, uint64_t value)
{
    if (type & 1u) return false;
    if (type < prev_type) return false;
    if (moq_quic_varint_len(value) == 0) return false;
    return true;
}

size_t moq_kvp_varint_entry_encoded_len(uint64_t prev_type,
                                         uint64_t type,
                                         uint64_t value)
{
    if (!kvp_varint_args_valid(prev_type, type, value))
        return 0;
    uint64_t delta = type - prev_type;
    size_t delta_len = moq_quic_varint_len(delta);
    if (delta_len == 0) return 0;
    return delta_len + moq_quic_varint_len(value);
}

size_t moq_kvp_encode_varint_entry(uint64_t prev_type,
                                    uint64_t type,
                                    uint64_t value,
                                    uint8_t *buf,
                                    size_t buf_len)
{
    if (!buf) return 0;

    size_t needed = moq_kvp_varint_entry_encoded_len(prev_type, type, value);
    if (needed == 0 || buf_len < needed)
        return 0;

    uint64_t delta = type - prev_type;
    size_t pos = 0;
    pos += moq_quic_varint_encode(delta, buf + pos, buf_len - pos);
    pos += moq_quic_varint_encode(value, buf + pos, buf_len - pos);
    return pos;
}

/* -- List edit (internal) ------------------------------------------ */

static moq_result_t kvp_read_int(moq_buf_reader_t *r, bool vi64, uint64_t *v)
{
    moq_result_t rc = vi64 ? moq_buf_read_vi64(r, v) : moq_buf_read_varint(r, v);
    return rc < 0 ? MOQ_ERR_INVAL : MOQ_OK;
}

static moq_result_t kvp_write_int(moq_buf_writer_t *w, bool vi64, uint64_t v)
{
    moq_result_t rc = vi64 ? moq_buf_write_vi64(w, v) : moq_buf_write_varint(w, v);
    return rc == MOQ_ERR_BUFFER ? MOQ_ERR_BUFFER : (rc < 0 ? MOQ_ERR_INVAL : MOQ_OK);
}

/* Count the instances of the even-typed integer `type` in the list and, one
 * level down, inside the bodies of the odd-typed `container` pairs; the
 * first instance's value goes to *first. A list that does not parse is
 * MOQ_ERR_INVAL. */
static moq_result_t kvp_count_int(const uint8_t *in, size_t len, bool vi64,
                                  uint64_t type, uint64_t container, bool top,
                                  size_t *count, uint64_t *first)
{
    moq_buf_reader_t r;
    moq_buf_reader_init(&r, in, len);
    uint64_t prev = 0;
    while (moq_buf_reader_remaining(&r) > 0) {
        uint64_t delta;
        if (kvp_read_int(&r, vi64, &delta) < 0 || delta > UINT64_MAX - prev)
            return MOQ_ERR_INVAL;
        const uint64_t at = prev + delta;
        if (at & 1u) {
            uint64_t n;
            if (kvp_read_int(&r, vi64, &n) < 0 || n > moq_buf_reader_remaining(&r))
                return MOQ_ERR_INVAL;
            if (top && container != 0 && at == container) {
                moq_result_t rc = kvp_count_int(in + moq_buf_reader_offset(&r), (size_t)n,
                                                vi64, type, 0, false, count, first);
                if (rc < 0) return rc;
            }
            r.pos += (size_t)n;
        } else {
            uint64_t v;
            if (kvp_read_int(&r, vi64, &v) < 0) return MOQ_ERR_INVAL;
            if (at == type) {
                if (*count == 0) *first = v;
                (*count)++;
            }
        }
        prev = at;
    }
    return MOQ_OK;
}

moq_result_t moq_kvp_list_merge_int(const uint8_t *in, size_t len, bool vi64,
                                    uint64_t type, uint64_t value, uint64_t container,
                                    uint8_t *out, size_t cap, size_t *out_len)
{
    if ((len > 0 && !in) || (cap > 0 && !out) || !out_len || (type & 1u) ||
        (container != 0 && !(container & 1u)))
        return MOQ_ERR_INVAL;
    *out_len = 0;
    size_t count = 0;
    uint64_t first = 0;
    moq_result_t rc = kvp_count_int(in, len, vi64, type, container, true, &count, &first);
    if (rc < 0) return rc;
    if (count == 0) return moq_kvp_list_add_int(in, len, vi64, type, value, out, cap, out_len);
    if (count > 1 || first != value) return MOQ_ERR_INVAL;
    if (len > cap) return MOQ_ERR_BUFFER;
    if (len) memcpy(out, in, len);
    *out_len = len;
    return MOQ_OK;
}

moq_result_t moq_kvp_list_add_int(const uint8_t *in, size_t len, bool vi64,
                                  uint64_t type, uint64_t value,
                                  uint8_t *out, size_t cap, size_t *out_len)
{
    if ((len > 0 && !in) || (cap > 0 && !out) || !out_len || (type & 1u))
        return MOQ_ERR_INVAL;
    *out_len = 0;
    moq_buf_reader_t r;
    moq_buf_reader_init(&r, in, len);
    moq_buf_writer_t w;
    moq_buf_writer_init(&w, out, cap);
    uint64_t prev = 0;
    bool placed = false;
    moq_result_t rc;
    while (moq_buf_reader_remaining(&r) > 0) {
        uint64_t delta;
        if (kvp_read_int(&r, vi64, &delta) < 0 || delta > UINT64_MAX - prev)
            return MOQ_ERR_INVAL;
        const uint64_t at = prev + delta;
        const size_t body = moq_buf_reader_offset(&r);   /* after the type */
        if (at & 1u) {
            uint64_t n;
            if (kvp_read_int(&r, vi64, &n) < 0 || n > moq_buf_reader_remaining(&r))
                return MOQ_ERR_INVAL;
            r.pos += (size_t)n;
        } else {
            uint64_t v;
            if (kvp_read_int(&r, vi64, &v) < 0) return MOQ_ERR_INVAL;
            if (at == type) {
                if (v != value) return MOQ_ERR_INVAL;
                placed = true;   /* already present: the list stays as it is */
            }
        }
        if (!placed && at > type) {
            rc = kvp_write_int(&w, vi64, type - prev);
            if (rc == MOQ_OK) rc = kvp_write_int(&w, vi64, value);
            if (rc == MOQ_OK) rc = kvp_write_int(&w, vi64, at - type);
            placed = true;
        } else {
            rc = kvp_write_int(&w, vi64, delta);
        }
        if (rc == MOQ_OK)
            rc = moq_buf_write_raw(&w, in + body, moq_buf_reader_offset(&r) - body);
        if (rc < 0) return rc == MOQ_ERR_BUFFER ? MOQ_ERR_BUFFER : MOQ_ERR_INVAL;
        prev = at;
    }
    if (!placed) {
        rc = kvp_write_int(&w, vi64, type - prev);
        if (rc == MOQ_OK) rc = kvp_write_int(&w, vi64, value);
        if (rc < 0) return rc;
    }
    *out_len = moq_buf_writer_offset(&w);
    return MOQ_OK;
}
