#ifndef TEST_CFG_OLD_LAYOUT_H
#define TEST_CFG_OLD_LAYOUT_H

#include <moq/session.h>

/* Historical records of configs whose layouts later gained appended fields,
 * declared as a binary built against those layouts declared them. A caller
 * of that age allocates exactly this sizeof and calls the exported
 * initializer symbol on it. */

/* moq_subgroup_cfg_t, ending at end_of_group. */
typedef struct test_subgroup_cfg_old {
    uint32_t struct_size;
    uint64_t group_id;
    uint64_t subgroup_id;
    uint8_t  publisher_priority;
    uint8_t  _reserved_sg[7];
    bool     object_properties;
    uint8_t  _reserved_sg2[7];
    bool     end_of_group;
} test_subgroup_cfg_old_t;

/* moq_object_cfg_t, ending at properties. */
typedef struct test_object_cfg_old {
    uint32_t     struct_size;
    uint64_t     object_id;
    moq_rcbuf_t *payload;
    moq_rcbuf_t *properties;
} test_object_cfg_old_t;

/* moq_track_status_cfg_t, ending at auth_token_count. */
typedef struct test_track_status_cfg_old {
    uint32_t                struct_size;
    moq_namespace_t         track_namespace;
    moq_bytes_t             track_name;
    const moq_auth_token_t *auth_tokens;
    size_t                  auth_token_count;
} test_track_status_cfg_old_t;

/* moq_accept_track_status_cfg_t, ending at expires_ms. */
typedef struct test_accept_track_status_cfg_old {
    uint32_t struct_size;
    bool     has_largest;
    uint64_t largest_group;
    uint64_t largest_object;
    bool     has_expires;
    uint64_t expires_ms;
} test_accept_track_status_cfg_old_t;

#endif
