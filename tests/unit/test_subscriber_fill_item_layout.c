#include <moq/subscriber.h>

_Static_assert(sizeof(((moq_sub_fill_item_t *)0)->struct_size) == 4, "size width");
_Static_assert(sizeof(((moq_sub_fill_item_t *)0)->kind) == 4, "kind width");
_Static_assert(sizeof(((moq_sub_fill_item_t *)0)->u.gap.range_kind) == 4, "gap width");
_Static_assert(offsetof(moq_sub_fill_item_t, struct_size) == 0, "size offset");
_Static_assert(offsetof(moq_sub_fill_item_t, kind) == 4, "kind offset");
_Static_assert(offsetof(moq_sub_fill_item_t, request) == 8, "child offset");
_Static_assert(offsetof(moq_sub_fill_item_t, track) == 16, "track offset");
_Static_assert(MOQ_SUB_FILL_OBJECT == 1 && MOQ_SUB_FILL_GAP == 2 &&
               MOQ_SUB_FILL_TERMINAL == 3 && MOQ_SUB_FILL_CREATED == 4, "kinds");

#if UINTPTR_MAX == UINT64_MAX
#define EXPECTED_INITIATOR 24
#define EXPECTED_UNION 32
#define EXPECTED_PAYLOAD 64
#define EXPECTED_PROPERTIES 72
#define EXPECTED_SIZE 80
#define EXPECTED_FETCH_SIZE 56
#define EXPECTED_FETCH_REQUEST 8
#elif UINTPTR_MAX == UINT32_MAX
#if defined(__i386__)
#define EXPECTED_INITIATOR 20
#define EXPECTED_UNION 28
#define EXPECTED_PAYLOAD 56
#define EXPECTED_PROPERTIES 60
#define EXPECTED_SIZE 64
#define EXPECTED_FETCH_SIZE 36
#define EXPECTED_FETCH_REQUEST 4
#else
#define EXPECTED_INITIATOR 24
#define EXPECTED_UNION 32
#define EXPECTED_PAYLOAD 60
#define EXPECTED_PROPERTIES 64
#define EXPECTED_SIZE 72
#define EXPECTED_FETCH_SIZE 40
#define EXPECTED_FETCH_REQUEST 4
#endif
#else
#error Unsupported layout probe target
#endif

_Static_assert(offsetof(moq_sub_fill_item_t, initiating_request_id) == EXPECTED_INITIATOR,
               "initiator offset");
_Static_assert(offsetof(moq_sub_fill_item_t, u) == EXPECTED_UNION, "union offset");
_Static_assert(offsetof(moq_sub_fill_item_t, u.object.payload) == EXPECTED_PAYLOAD,
               "payload offset");
_Static_assert(offsetof(moq_sub_fill_item_t, u.object.properties) == EXPECTED_PROPERTIES,
               "properties offset");
_Static_assert(sizeof(moq_sub_fill_item_t) == EXPECTED_SIZE, "full v0 layout");
_Static_assert(MOQ_SUB_FILL_ITEM_V0_SIZE == EXPECTED_SIZE, "frozen v0 floor");
_Static_assert(offsetof(moq_sub_fetch_item_t, kind) == 0, "unchanged FETCH kind");
_Static_assert(offsetof(moq_sub_fetch_item_t, request) == EXPECTED_FETCH_REQUEST,
               "unchanged FETCH request");
_Static_assert(sizeof(moq_sub_fetch_item_t) == EXPECTED_FETCH_SIZE, "unchanged FETCH layout");

static moq_result_t (*const sized_poll)(moq_subscriber_t *, moq_sub_fill_item_t *,
                                      size_t) = moq_sub_poll_fill;
static moq_result_t (*const fetch_poll)(moq_subscriber_t *, moq_sub_fetch_item_t *) =
    moq_sub_poll_fetch;

int main(void) { return sized_poll == NULL || fetch_poll == NULL; }
