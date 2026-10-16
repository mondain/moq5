#ifndef MOQ_FILL_IDENTITY_INTERNAL_H
#define MOQ_FILL_IDENTITY_INTERNAL_H

#include <stdbool.h>
#include <stdint.h>
#include <stdatomic.h>

/* One private instance per linked core library. Copies of the library,
 * unload/reload and separate processes are distinct identity domains.
 * This protects identity issuance only, not subscriber or session access.
 * The bounded critical section has no calls, allocations or 64-bit atomics.
 * Contention is not wait-free; use from signals or ISRs is not supported. */
typedef struct {
    atomic_flag lock;
    uint64_t last_issued;
} sub_fill_identity_allocator_t;

#define SUB_FILL_IDENTITY_ALLOCATOR_INIT \
    { .lock = ATOMIC_FLAG_INIT, .last_issued = 0 }

/* Both arguments are required. Exhaustion leaves out and the counter intact. */
static inline bool sub_fill_identity_next(sub_fill_identity_allocator_t *ids,
                                         uint64_t *out)
{
    while (atomic_flag_test_and_set_explicit(&ids->lock, memory_order_acquire)) {}
    bool available = ids->last_issued != UINT64_MAX;
    if (available) *out = ++ids->last_issued;
    atomic_flag_clear_explicit(&ids->lock, memory_order_release);
    return available;
}

#endif /* MOQ_FILL_IDENTITY_INTERNAL_H */
