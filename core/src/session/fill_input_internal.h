#ifndef MOQ_FILL_INPUT_INTERNAL_H
#define MOQ_FILL_INPUT_INTERNAL_H

#include "moq/fill.h"

/* Selected input values only. No pointers, parent-state resolution, range
 * evaluation, allocation, slot selection, or session/table mutation. The
 * location is in the core's filter form (type plus raw locations; the
 * object-end form's end object is inclusive). */
typedef struct moq_fill_input {
    bool requested;
    bool has_subscriber_priority;
    uint8_t subscriber_priority;
    bool has_group_order;
    uint32_t group_order;
    bool has_fill_timeout;
    uint64_t fill_timeout_ms;
    bool has_location;
    uint32_t filter_type;
    uint64_t start_group;
    uint64_t start_object;
    uint64_t end_group;
    uint64_t end_object;
} moq_fill_input_t;

/* NULL cfg means no request; a non-NULL cfg must provide a readable size
 * stamp and that many allocated bytes. Future tails are ignored. Refusal
 * preserves out, including when nested location validation fails. */
moq_result_t moq_fill_input_normalize(const moq_fill_cfg_t *cfg,
                                      moq_fill_input_t *out);

#define MOQ_FILL_PUBLIC_GENERATION_MAX UINT64_C(0x08000000)
#define MOQ_FILL_PUBLIC_SLOT_MAX UINT64_C(0xffff)

typedef struct moq_fill_identity {
    size_t slot;
    uint64_t generation; /* private, not the packed odd generation */
} moq_fill_identity_t;

/* Map private generation g to public 2*g-1 after wide-domain validation.
 * Invalid arguments/unrepresentable fields return INVAL without changing out.
 * Integration must reserve this identity before any ID/owner/queue commit;
 * exhausted public slots cannot be reused even if private g could advance. */
moq_result_t moq_fill_handle_encode(uint16_t session_tag, size_t slot,
    uint64_t generation, moq_fill_t *out);

/* Decode a structurally valid fill for the expected nonzero session tag.
 * Wrong kind/invalid structure returns STALE_HANDLE; a different session is
 * WRONG_SESSION. No liveness/parent/generation lookup is performed: integration
 * must still resolve this identity against its retained owner. On refusal out
 * is unchanged. NULL out or zero expected tag is INVAL. */
moq_result_t moq_fill_handle_decode(uint16_t session_tag, moq_fill_t handle,
    moq_fill_identity_t *out);

#endif
