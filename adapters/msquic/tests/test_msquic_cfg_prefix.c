/*
 * Size-qualified config-prefix derivation, on the production path.
 *
 * The appended block of moq_msquic_managed_cfg_t is honored only when the
 * caller's struct_size covers it: an old binary's prefix-sized cfg must
 * default every appended field, a cfg sized THROUGH max_connections must
 * honor that field while still defaulting lane_count, and a full-size cfg
 * must honor all of them.
 *
 * Three size boundaries are exercised because they are not equivalent. The
 * connection cap defaults to 1, and the lane count is clamped to the cap,
 * so the v0 row cannot discriminate lane_count at all -- whether the field
 * is read or defaulted, the clamp forces 1. The through-max_connections row
 * exists to remove that mask.
 *
 * Every appended value the rows declare is a VALID bounded sentinel, never
 * an all-ones tail read: a supported non-default version, a small cap, a
 * lane count inside the cap. The tail is poisoned first only as a
 * conservation aid -- correct code reads none of it, and nothing here
 * depends on what a poisoned read would produce.
 *
 * Row E pins the client's identity derived from its dial parameters: kept
 * whole up to the derivation's bound, refused at create beyond it.
 *
 * The facade comes from the lanes-only testing constructor, which consumes
 * the SAME derivation as the public one, so this needs no listener, socket
 * or certificate. Admission runs through the exact production listener
 * callback.
 */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "msquic_internal.h"

#include "support/fake_msq_table.h"

#include <moq/msquic_managed.h>

/* MOQ_MSQUIC_TESTING-only seams. */
extern moq_result_t moq_msq_test_managed_create_lanes_only_api(
    const moq_msquic_managed_cfg_t *cfg, const QUIC_API_TABLE *api,
    moq_msquic_managed_t **out);
extern QUIC_STATUS moq_msq_test_listener_accept(moq_msquic_managed_t *m,
                                                HQUIC connection);
extern const moq_connection_target_t *moq_msq_test_managed_target(
    const moq_msquic_managed_t *m);

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

enum { MAX_CONNS = 8 };

static fake_msq_t g_table_fake;
static fake_msq_t g_conn_fake[MAX_CONNS];

static int quiet_pump(moq_msquic_managed_t *m,
                      moq_msquic_managed_lane_t *lane, uint64_t now_us,
                      void *user)
{
    (void)m;
    (void)lane;
    (void)now_us;
    (void)user;
    return 0;
}

struct rig {
    moq_msquic_managed_t *m;
    bool live[MAX_CONNS];
};

static bool rig_up(struct rig *r, const moq_msquic_managed_cfg_t *cfg)
{
    memset(r, 0, sizeof(*r));
    fake_msq_init(&g_table_fake, false);
    for (int i = 0; i < MAX_CONNS; i++)
        fake_msq_init(&g_conn_fake[i], false);
    return moq_msq_test_managed_create_lanes_only_api(
               cfg, fake_msq_table(&g_table_fake), &r->m) == MOQ_OK;
}

/* Admission through the exact production listener callback. */
static QUIC_STATUS rig_accept(struct rig *r, int slot)
{
    QUIC_STATUS st = moq_msq_test_listener_accept(
        r->m, fake_msq_conn_handle(&g_conn_fake[slot]));

    if (QUIC_SUCCEEDED(st))
        r->live[slot] = true;
    return st;
}

/* An admitted child is waited on by stop(), so hand each one the transport
 * terminal a real shutdown would deliver. */
static void rig_down(struct rig *r)
{
    if (r->m == NULL)
        return;
    for (int i = 0; i < MAX_CONNS; i++) {
        QUIC_CONNECTION_EVENT ev;

        if (!r->live[i] || !fake_msq_conn_cb_installed(&g_conn_fake[i]))
            continue;
        memset(&ev, 0, sizeof(ev));
        ev.Type = QUIC_CONNECTION_EVENT_SHUTDOWN_COMPLETE;
        (void)fake_msq_deliver_conn_event(&g_conn_fake[i], &ev);
        r->live[i] = false;
    }
    (void)moq_msquic_managed_stop(r->m);
    moq_msquic_managed_destroy(r->m);
    r->m = NULL;
}

/* Poison the whole appended block, then let each row declare the exact
 * values it wants read back. Correct code reads none of the poison; it is
 * here so a derivation that ignored struct_size would pick up garbage in
 * the fields no row declares. */
static void poison_appended(moq_msquic_managed_cfg_t *cfg)
{
    size_t off = offsetof(moq_msquic_managed_cfg_t, version);

    memset((unsigned char *)cfg + off, 0xff, sizeof(*cfg) - off);
}

static void base_server_cfg(moq_msquic_managed_cfg_t *cfg, size_t size)
{
    moq_msquic_managed_cfg_init_sized(cfg, size);
    cfg->alloc = moq_alloc_default();
    cfg->perspective = MOQ_PERSPECTIVE_SERVER;
    cfg->host = "127.0.0.1";
    cfg->port = 0;
    cfg->on_lane_pump = quiet_pump;
    cfg->send_request_capacity = true;
    cfg->initial_request_capacity = 16;
}

/* --- Row A: the frozen v0 prefix ---------------------------------------- */

/* struct_size stops before `version`, so every appended field defaults:
 * draft-16, cap 1, one lane. The row declares valid non-default values for
 * the appended fields anyway -- a derivation that dropped its size guard
 * would adopt exactly those, and each named oracle says which. */
static void t_row_a_v0_prefix(void)
{
    int before = failures;
    struct rig r;
    moq_msquic_managed_cfg_t cfg;

    base_server_cfg(&cfg, offsetof(moq_msquic_managed_cfg_t, version));
    poison_appended(&cfg);
    /* declared, valid, bounded: a supported non-default version, a small
     * cap, a lane count inside that cap, a real _Bool */
    cfg.version = MOQ_VERSION_DRAFT_18;
    cfg.max_connections = 2;
    cfg.lane_count = 1;
    cfg.streaming_objects = true;

    if (!rig_up(&r, &cfg)) {
        CHECK(0 && "row A create");
        return;
    }

    /* the appended block is excluded, so every default stands */
    CHECK(moq_msquic_managed_negotiated_version(r.m) ==
          MOQ_VERSION_DRAFT_16);
    CHECK(moq_msquic_managed_lane_count(r.m) == 1);

    /* cap 1: the first admission succeeds, the second is refused */
    CHECK(rig_accept(&r, 0) == QUIC_STATUS_SUCCESS);
    CHECK(moq_msquic_managed_conn_count(r.m) == 1);
    CHECK(rig_accept(&r, 1) == QUIC_STATUS_CONNECTION_REFUSED);
    CHECK(moq_msquic_managed_conn_count(r.m) == 1);
    /* the refused identity was never adopted */
    CHECK(!fake_msq_conn_cb_installed(&g_conn_fake[1]));

    rig_down(&r);
    if (failures == before)
        printf("PASS: row_a_v0_prefix\n");
}

/* --- Row B: sized THROUGH max_connections ------------------------------- */

/* version and max_connections are inside struct_size and must be honored;
 * lane_count is outside it and must default to 1. With the cap at 4 the
 * clamp no longer masks a lane_count read, so this row is the one that can
 * discriminate it. */
static void t_row_b_through_max_connections(void)
{
    int before = failures;
    struct rig r;
    moq_msquic_managed_cfg_t cfg;

    base_server_cfg(&cfg, offsetof(moq_msquic_managed_cfg_t, lane_count));
    poison_appended(&cfg);
    cfg.version = MOQ_VERSION_DRAFT_16; /* inside struct_size */
    cfg.max_connections = 4;            /* inside struct_size */
    cfg.lane_count = 4;                 /* excluded: must be ignored */
    cfg.streaming_objects = true;       /* excluded: must be ignored */

    if (!rig_up(&r, &cfg)) {
        CHECK(0 && "row B create");
        return;
    }

    CHECK(moq_msquic_managed_negotiated_version(r.m) ==
          MOQ_VERSION_DRAFT_16);
    /* the discriminating oracle: excluded lane_count defaults to one lane
     * even though the cap would have allowed four */
    CHECK(moq_msquic_managed_lane_count(r.m) == 1);

    /* the honored cap admits exactly four */
    for (int i = 0; i < 4; i++)
        CHECK(rig_accept(&r, i) == QUIC_STATUS_SUCCESS);
    CHECK(moq_msquic_managed_conn_count(r.m) == 4);
    CHECK(rig_accept(&r, 4) == QUIC_STATUS_CONNECTION_REFUSED);
    CHECK(moq_msquic_managed_conn_count(r.m) == 4);

    rig_down(&r);
    if (failures == before)
        printf("PASS: row_b_through_max_connections\n");
}

/* --- Row C: the full struct --------------------------------------------- */

/* The positive control: with every appended field inside struct_size, each
 * declared non-default value must be honored. A derivation that hard-coded
 * a default regardless of size passes rows A and B and fails only here. */
static void t_row_c_full(void)
{
    int before = failures;
    struct rig r;
    moq_msquic_managed_cfg_t cfg;

    base_server_cfg(&cfg, sizeof(cfg));
    cfg.version = MOQ_VERSION_DRAFT_18;
    cfg.max_connections = 3;
    cfg.lane_count = 2;
    cfg.streaming_objects = true;

    if (!rig_up(&r, &cfg)) {
        CHECK(0 && "row C create");
        return;
    }

    CHECK(moq_msquic_managed_negotiated_version(r.m) ==
          MOQ_VERSION_DRAFT_18);
    CHECK(moq_msquic_managed_lane_count(r.m) == 2);

    for (int i = 0; i < 3; i++)
        CHECK(rig_accept(&r, i) == QUIC_STATUS_SUCCESS);
    CHECK(moq_msquic_managed_conn_count(r.m) == 3);
    CHECK(rig_accept(&r, 3) == QUIC_STATUS_CONNECTION_REFUSED);
    CHECK(moq_msquic_managed_conn_count(r.m) == 3);

    rig_down(&r);
    if (failures == before)
        printf("PASS: row_c_full\n");
}

/* --- Row D: the connection-target tail ----------------------------------- */

static uint32_t row_d_policy(void *ctx, uint32_t present, moq_bytes_t authority,
                             moq_bytes_t path_query)
{
    (void)ctx; (void)present; (void)authority; (void)path_query;
    return MOQ_URI_ACCEPT;
}

/* The appended `target` is read only when struct_size covers it: a caller
 * sized just short of it may leave the field as garbage and is served (a
 * poisoned pointer is never dereferenced); a full-sized caller's malformed
 * target is refused before any allocation, and a well-formed policy is
 * accepted. */
static void t_row_d_target_tail(void)
{
    int before = failures;
    struct rig r;
    moq_msquic_managed_cfg_t cfg;
    moq_msquic_managed_t *m = NULL;

    base_server_cfg(&cfg, offsetof(moq_msquic_managed_cfg_t, target));
    /* every field inside struct_size keeps its initialized default; the
     * excluded target is a poisoned pointer */
    memset(&cfg.target, 0xff, sizeof(cfg.target));
    if (!rig_up(&r, &cfg)) {
        CHECK(0 && "row D prefix create");
        return;
    }
    CHECK(rig_accept(&r, 0) == QUIC_STATUS_SUCCESS);
    rig_down(&r);

    /* inside struct_size, a target below the v0 floor is malformed */
    moq_connection_target_t bad;
    memset(&bad, 0, sizeof(bad));
    bad.struct_size = (uint32_t)MOQ_CONNECTION_TARGET_V0_SIZE - 1;
    bad.accept_uri = row_d_policy;
    base_server_cfg(&cfg, sizeof(cfg));
    cfg.target = &bad;
    CHECK(moq_msq_test_managed_create_lanes_only_api(
              &cfg, fake_msq_table(&g_table_fake), &m) == MOQ_ERR_INVAL);
    CHECK(m == NULL);

    /* a client target with a length but no bytes is malformed too */
    moq_connection_target_t dangling;
    memset(&dangling, 0, sizeof(dangling));
    dangling.struct_size = (uint32_t)sizeof(dangling);
    dangling.target_present = MOQ_BINDING_TARGET_COMPLETE;
    dangling.authority.len = 4;
    cfg.target = &dangling;
    CHECK(moq_msq_test_managed_create_lanes_only_api(
              &cfg, fake_msq_table(&g_table_fake), &m) == MOQ_ERR_INVAL);
    CHECK(m == NULL);

    /* the positive control: a well-formed policy is honored */
    moq_connection_target_t good;
    memset(&good, 0, sizeof(good));
    good.struct_size = (uint32_t)sizeof(good);
    good.accept_uri = row_d_policy;
    cfg.target = &good;
    if (!rig_up(&r, &cfg)) {
        CHECK(0 && "row D full create");
        return;
    }
    CHECK(rig_accept(&r, 0) == QUIC_STATUS_SUCCESS);
    rig_down(&r);

    if (failures == before)
        printf("PASS: row_d_target_tail\n");
}

/* --- the prefix boundary itself ----------------------------------------- */

/* A struct_size below the frozen v0 prefix is not an old caller, it is a
 * malformed one, and both constructors refuse it. */
static void t_below_prefix_refused(void)
{
    int before = failures;
    moq_msquic_managed_cfg_t cfg;
    moq_msquic_managed_t *m = NULL;

    base_server_cfg(&cfg, sizeof(cfg));
    cfg.struct_size =
        (uint32_t)(offsetof(moq_msquic_managed_cfg_t, version) - 1);
    CHECK(moq_msq_test_managed_create_lanes_only_api(
              &cfg, fake_msq_table(&g_table_fake), &m) == MOQ_ERR_INVAL);
    CHECK(m == NULL);
    CHECK(moq_msquic_managed_create(&cfg, &m) == MOQ_ERR_INVAL);
    CHECK(m == NULL);

    if (failures == before)
        printf("PASS: below_prefix_refused\n");
}

/* --- Row E: a client's derived identity ------------------------------- */

/* A client with no target is identified by its dial parameters, never left
 * unidentified: the derived authority host:port fits up to 1022 bytes and is
 * kept whole; one byte more is refused at create, before the MsQuic API is
 * opened (the public constructor) and with no facade returned. */
static void t_row_e_client_dial_identity(void)
{
    int before = failures;
    static char host[1024];
    moq_msquic_managed_cfg_t cfg;
    moq_msquic_managed_t *m = NULL;

    /* "<1018 x h>:443" is 1022 bytes */
    memset(host, 'h', 1018);
    host[1018] = '\0';
    base_server_cfg(&cfg, sizeof(cfg));
    cfg.perspective = MOQ_PERSPECTIVE_CLIENT;
    cfg.host = host;
    cfg.port = 443;
    fake_msq_init(&g_table_fake, false);
    CHECK(moq_msq_test_managed_create_lanes_only_api(
              &cfg, fake_msq_table(&g_table_fake), &m) == MOQ_OK);
    if (m != NULL) {
        const moq_connection_target_t *t = moq_msq_test_managed_target(m);
        CHECK(t != NULL && t->target_present == MOQ_BINDING_TARGET_COMPLETE);
        CHECK(t != NULL && t->authority.len == 1022 &&
              memcmp(t->authority.data, host, 1018) == 0 &&
              memcmp(t->authority.data + 1018, ":443", 4) == 0);
        CHECK(t != NULL && t->path_query.len == 0);
        (void)moq_msquic_managed_stop(m);
        moq_msquic_managed_destroy(m);
        m = NULL;
    }

    /* 1023 bytes: refused on both constructors */
    host[1018] = 'h';
    host[1019] = '\0';
    CHECK(moq_msq_test_managed_create_lanes_only_api(
              &cfg, fake_msq_table(&g_table_fake), &m) == MOQ_ERR_INVAL);
    CHECK(m == NULL);
    if (m != NULL) {
        (void)moq_msquic_managed_stop(m);
        moq_msquic_managed_destroy(m);
        m = NULL;
    }
    CHECK(moq_msquic_managed_create(&cfg, &m) == MOQ_ERR_INVAL);
    CHECK(m == NULL);
    if (m != NULL) {
        (void)moq_msquic_managed_stop(m);
        moq_msquic_managed_destroy(m);
    }

    if (failures == before)
        printf("PASS: row_e_client_dial_identity\n");
}

int main(void)
{
    t_row_a_v0_prefix();
    t_row_b_through_max_connections();
    t_row_c_full();
    t_row_d_target_tail();
    t_row_e_client_dial_identity();
    t_below_prefix_refused();

    if (failures == 0)
        printf("PASS: msquic_cfg_prefix\n");
    else
        fprintf(stderr, "FAIL: msquic_cfg_prefix (%d)\n", failures);
    return failures;
}
