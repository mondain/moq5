#include <moq/publisher.h>

#ifdef __cplusplus
#define PIN static_assert
#define ALIGNOF alignof
#else
#define PIN _Static_assert
#define ALIGNOF _Alignof
#endif

/* Measured separately in C/C++, normal/short-enum modes on all three ABIs. */
#if UINTPTR_MAX == UINT64_MAX
#define CFG_TAIL (sizeof(moq_pub_accept_mode_t) == 1 ? 144 : 152)
#define CFG_SIZE (sizeof(moq_pub_accept_mode_t) == 1 ? 184 : 192)
#define CFG_ALIGN 8
#define RECORD_START 8
#define REQUEST_SIZE 96
#define TERMINAL_SIZE 56
#define RECORD_ALIGN 8
#elif defined(__i386__)
#define CFG_TAIL (sizeof(moq_pub_accept_mode_t) == 1 ? 76 : 80)
#define CFG_SIZE (sizeof(moq_pub_accept_mode_t) == 1 ? 100 : 104)
#define CFG_ALIGN 4
#define RECORD_START 4
#define REQUEST_SIZE 88
#define TERMINAL_SIZE 52
#define RECORD_ALIGN 4
#elif defined(__arm__)
#define CFG_TAIL 80
#define CFG_SIZE 104
#define CFG_ALIGN 8
#define RECORD_START 8
#define REQUEST_SIZE 96
#define TERMINAL_SIZE 56
#define RECORD_ALIGN 8
#else
#error Unsupported publisher fill layout target
#endif

#define FIELD(type, field, offset, width) \
    PIN(offsetof(type, field) == (offset), #type " offset " #field); \
    PIN(offsetof(type, field) + sizeof(((type *)0)->field) == (offset) + (width), \
        #type " complete field end " #field)

/* The fill callbacks end at CFG_SIZE; the new-group callback pair and the
 * GOAWAY callback follow. */
PIN(sizeof(moq_pub_cfg_t) ==
    (CFG_SIZE + 3 * sizeof(void *) + CFG_ALIGN - 1) / CFG_ALIGN * CFG_ALIGN,
    "current config allocation");
PIN(ALIGNOF(moq_pub_cfg_t) == CFG_ALIGN, "current config alignment");
FIELD(moq_pub_cfg_t, max_fill_snapshot_bytes, CFG_TAIL, 8);
FIELD(moq_pub_cfg_t, on_fill_request, CFG_TAIL + 8, sizeof(void *));
FIELD(moq_pub_cfg_t, on_fill_request_ctx, CFG_TAIL + 8 + sizeof(void *), sizeof(void *));
FIELD(moq_pub_cfg_t, on_fill_terminal, CFG_TAIL + 8 + 2 * sizeof(void *), sizeof(void *));
FIELD(moq_pub_cfg_t, on_fill_terminal_ctx, CFG_TAIL + 8 + 3 * sizeof(void *), sizeof(void *));
PIN(offsetof(moq_pub_cfg_t, on_fill_terminal_ctx) + sizeof(void *) == CFG_SIZE,
    "complete fill callback tail");
FIELD(moq_pub_cfg_t, on_new_group_request, CFG_SIZE, sizeof(void *));
FIELD(moq_pub_cfg_t, on_new_group_request_ctx, CFG_SIZE + sizeof(void *), sizeof(void *));
FIELD(moq_pub_cfg_t, on_goaway, CFG_SIZE + 2 * sizeof(void *), sizeof(void *));

PIN(sizeof(moq_pub_fill_t) == 8, "value token width");
PIN(ALIGNOF(moq_pub_fill_t) == RECORD_ALIGN, "value token alignment");
FIELD(moq_pub_fill_t, _opaque, 0, 8);
PIN(sizeof(moq_pub_fill_request_t) == REQUEST_SIZE, "borrowed request allocation");
PIN(ALIGNOF(moq_pub_fill_request_t) == RECORD_ALIGN, "borrowed request alignment");
FIELD(moq_pub_fill_request_t, struct_size, 0, 4);
FIELD(moq_pub_fill_request_t, fill, RECORD_START, 8);
FIELD(moq_pub_fill_request_t, sub, RECORD_START + 8, 8);
FIELD(moq_pub_fill_request_t, pub, RECORD_START + 16, 8);
FIELD(moq_pub_fill_request_t, initiating_request_id, RECORD_START + 24, 8);
FIELD(moq_pub_fill_request_t, start_group, RECORD_START + 32, 8);
FIELD(moq_pub_fill_request_t, start_object, RECORD_START + 40, 8);
FIELD(moq_pub_fill_request_t, end_group, RECORD_START + 48, 8);
FIELD(moq_pub_fill_request_t, end_object, RECORD_START + 56, 8);
FIELD(moq_pub_fill_request_t, processed_at_us, RECORD_START + 64, 8);
FIELD(moq_pub_fill_request_t, fill_timeout_ms, RECORD_START + 72, 8);
FIELD(moq_pub_fill_request_t, group_order, RECORD_START + 80, 1);
FIELD(moq_pub_fill_request_t, subscriber_priority, RECORD_START + 81, 1);
FIELD(moq_pub_fill_request_t, has_fill_timeout, RECORD_START + 82, 1);
PIN(offsetof(moq_pub_fill_request_t, has_fill_timeout) + 1 <= REQUEST_SIZE,
    "request's complete readable prefix precedes trailing padding");

PIN(sizeof(moq_pub_fill_terminal_t) == TERMINAL_SIZE, "borrowed terminal allocation");
PIN(ALIGNOF(moq_pub_fill_terminal_t) == RECORD_ALIGN, "borrowed terminal alignment");
FIELD(moq_pub_fill_terminal_t, struct_size, 0, 4);
FIELD(moq_pub_fill_terminal_t, fill, RECORD_START, 8);
FIELD(moq_pub_fill_terminal_t, sub, RECORD_START + 8, 8);
FIELD(moq_pub_fill_terminal_t, pub, RECORD_START + 16, 8);
FIELD(moq_pub_fill_terminal_t, initiating_request_id, RECORD_START + 24, 8);
FIELD(moq_pub_fill_terminal_t, reason, RECORD_START + 32, 4);
FIELD(moq_pub_fill_terminal_t, error_domain, RECORD_START + 36, 4);
FIELD(moq_pub_fill_terminal_t, error_code, RECORD_START + 40, 8);
PIN(offsetof(moq_pub_fill_terminal_t, error_code) + 8 == TERMINAL_SIZE,
    "complete terminal prefix");

int main(void) { return 0; }
