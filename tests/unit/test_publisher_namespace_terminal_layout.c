#include <moq/publisher.h>
#ifdef __cplusplus
#define PIN static_assert
#else
#define PIN _Static_assert
#endif
typedef struct old_info {
    uint32_t struct_size;
    moq_pub_namespace_terminal_kind_t kind;
    moq_namespace_t namespace_;
    moq_request_error_t error_code;
    bool can_retry;
    uint64_t retry_after_ms;
    moq_bytes_t reason;
} old_info_t;
#if UINTPTR_MAX == UINT64_MAX
#define OLD_SIZE 64
#elif defined(__i386__)
#define OLD_SIZE 44
#else
#define OLD_SIZE 48
#endif
PIN(sizeof(old_info_t) == OLD_SIZE, "old complete physical extent");
PIN(MOQ_PUB_NAMESPACE_TERMINAL_INFO_V0_SIZE == sizeof(old_info_t), "append past old extent");
#define SAME(member) PIN(offsetof(old_info_t, member) == offsetof(moq_pub_namespace_terminal_info_t, member), #member)
SAME(struct_size); SAME(kind); SAME(namespace_); SAME(error_code);
SAME(can_retry); SAME(retry_after_ms); SAME(reason);
PIN(offsetof(moq_pub_namespace_terminal_info_t, cause) == OLD_SIZE, "cause");
PIN(offsetof(moq_pub_namespace_terminal_info_t, local_role) == OLD_SIZE + 4, "role");
PIN(offsetof(moq_pub_namespace_terminal_info_t, stream_error_code) == OLD_SIZE + 8, "stream code");
PIN(sizeof(moq_pub_namespace_terminal_info_t) == OLD_SIZE + 16, "full extent");
PIN(sizeof(((moq_pub_namespace_terminal_info_t *)0)->cause) == 4, "cause width");
PIN(sizeof(((moq_pub_namespace_terminal_info_t *)0)->local_role) == 4, "role width");
PIN(MOQ_PUB_NAMESPACE_TERMINATED == 3, "new kind");
int main(void) { return 0; }
