#ifndef TEST_PUBLISHER_TRACK_OLD_LAYOUT_H
#define TEST_PUBLISHER_TRACK_OLD_LAYOUT_H

#include <moq/session.h>

/* Independent historical records, with only their real fields. */
typedef struct test_pub_track_original {
    uint32_t struct_size;
    moq_namespace_t track_namespace;
    moq_bytes_t track_name;
    bool advertise_namespace;
    uint8_t publisher_priority;
} test_pub_track_original_t;

typedef struct test_pub_track_pre_monotonic {
    uint32_t struct_size;
    moq_namespace_t track_namespace;
    moq_bytes_t track_name;
    bool advertise_namespace;
    uint8_t publisher_priority;
    uint64_t max_retained_bytes;
    bool has_publisher_priority;
} test_pub_track_pre_monotonic_t;

/* Scalar _Alignof(uint64_t) is not its i686 struct-member alignment. */
typedef struct test_pub_track_u64_member { char prefix; uint64_t value; }
    test_pub_track_u64_member_t;
#define PUB_TRACK_DIM(lp64, i686, arm) \
    (sizeof(void *) == 8 ? (lp64) : \
     offsetof(test_pub_track_u64_member_t, value) == 8 ? (arm) : (i686))

_Static_assert(sizeof(test_pub_track_original_t) == PUB_TRACK_DIM(48, 24, 24),
               "original track allocation");
_Static_assert(_Alignof(test_pub_track_original_t) == PUB_TRACK_DIM(8, 4, 4),
               "original track alignment");
_Static_assert(sizeof(test_pub_track_pre_monotonic_t) == PUB_TRACK_DIM(64, 36, 40),
               "pre-monotonic track allocation");
_Static_assert(_Alignof(test_pub_track_pre_monotonic_t) == PUB_TRACK_DIM(8, 4, 8),
               "pre-monotonic track alignment");

#define PUB_TRACK_OLD_FIELD(T, f, off64, off32, width64, width32) \
    _Static_assert(offsetof(T, f) == (sizeof(void *) == 8 ? (off64) : (off32)), \
                   #T "." #f " offset"); \
    _Static_assert(sizeof(((T *)0)->f) == \
                   (sizeof(void *) == 8 ? (width64) : (width32)), #T "." #f " width")
#define PUB_TRACK_OLD_COMMON(T) \
    PUB_TRACK_OLD_FIELD(T, struct_size, 0, 0, 4, 4); \
    PUB_TRACK_OLD_FIELD(T, track_namespace, 8, 4, 16, 8); \
    PUB_TRACK_OLD_FIELD(T, track_name, 24, 12, 16, 8); \
    PUB_TRACK_OLD_FIELD(T, advertise_namespace, 40, 20, 1, 1); \
    PUB_TRACK_OLD_FIELD(T, publisher_priority, 41, 21, 1, 1)
PUB_TRACK_OLD_COMMON(test_pub_track_original_t);
PUB_TRACK_OLD_COMMON(test_pub_track_pre_monotonic_t);
PUB_TRACK_OLD_FIELD(test_pub_track_pre_monotonic_t, max_retained_bytes, 48, 24, 8, 8);
PUB_TRACK_OLD_FIELD(test_pub_track_pre_monotonic_t, has_publisher_priority, 56, 32, 1, 1);
#undef PUB_TRACK_OLD_COMMON
#undef PUB_TRACK_OLD_FIELD

#endif
