/*
 * Application deadlines and lane wakes never stall a lane.
 *
 * NETWORK-FREE. The facade borrows the fake QUIC_API_TABLE and creates only
 * lanes and doorbells; time is the injected clock. Three properties:
 *
 *   served once -- a lane pumps for an application deadline once. A query
 *     that keeps answering an instant a pump of that lane has already run at
 *     or after leaves the lane idle instead of pumping it again at once,
 *     forever, with the lane lock held.
 *
 *   per lane -- the per-lane query is asked by each lane for itself only, so
 *     one lane's due deadline neither pumps nor wakes another lane, and the
 *     query block is read only when struct_size covers all of it.
 *
 *   never blocks -- a wake from any thread returns while the target lane is
 *     inside a pump (holding its lock), and a lane whose deadlines keep
 *     falling due still lets other threads take its lock between passes.
 *
 * The step rows drive the production doorbell iteration directly
 * (moq_msq_test_lane_step), so their verdicts are exact. The thread rows use
 * real doorbells; their timed waits are fail-closed guards around named
 * predicates, and a row whose lane would otherwise stay locked is released
 * by facade stop, which a doorbell observes without its lane lock.
 */

#if defined(__linux__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "msquic_internal.h"
#include "support/fake_msq_table.h"
#include "support/msq_test_seams.h"

#include <moq/msquic_managed.h>

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

extern void moq_msq_test_bell_ring_raw(moq_msquic_managed_lane_t *lane);
extern moq_result_t moq_msq_test_managed_create_lanes_only_api(
    const moq_msquic_managed_cfg_t *cfg, const QUIC_API_TABLE *api,
    moq_msquic_managed_t **out);

/* The per-lane query block is appended past the whole layout before it,
 * tail padding included, so a caller sized for that layout never covers it. */
#define LAYOUT_THROUGH_TARGET                                                \
    ((offsetof(moq_msquic_managed_cfg_t, target) + sizeof(void *) +          \
      _Alignof(moq_msquic_managed_cfg_t) - 1) /                              \
     _Alignof(moq_msquic_managed_cfg_t) * _Alignof(moq_msquic_managed_cfg_t))
_Static_assert(offsetof(moq_msquic_managed_cfg_t, app_lane_deadline_us) >=
                   LAYOUT_THROUGH_TARGET,
               "the per-lane deadline block starts past the previous sizeof");
_Static_assert(offsetof(moq_msquic_managed_cfg_t, app_lane_deadline_ctx) >
                   offsetof(moq_msquic_managed_cfg_t, app_lane_deadline_us),
               "the per-lane deadline block is callback, then context");

static int failures;

#define CHECK(expr)                                                         \
    do {                                                                    \
        if (!(expr)) {                                                      \
            fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__,    \
                    #expr);                                                 \
            failures++;                                                     \
        }                                                                   \
    } while (0)

enum { LANES = 2 };
#define GUARD_US (3u * 1000u * 1000u)

static void guard_deadline(struct timespec *abs)
{
    clock_gettime(CLOCK_REALTIME, abs);
    abs->tv_sec += (time_t)(GUARD_US / 1000000u);
    abs->tv_nsec += (long)(GUARD_US % 1000000u) * 1000;
    if (abs->tv_nsec >= 1000000000L) {
        abs->tv_sec++;
        abs->tv_nsec -= 1000000000L;
    }
}

/* --- injected clock -------------------------------------------------------- *
 * Fixed unless `g_now_step` is nonzero, in which case every read advances it
 * by that much: a clock under which a query answering "just before now" is a
 * new, already-due instant on every read. */
static _Atomic uint64_t g_now_us;
static _Atomic uint64_t g_now_step;

static uint64_t test_now_us(void)
{
    uint64_t step = atomic_load(&g_now_step);

    if (step == 0)
        return atomic_load(&g_now_us);
    return atomic_fetch_add(&g_now_us, step) + step;
}

/* --- deadline queries ------------------------------------------------------ */
static _Atomic uint64_t g_facade_target = UINT64_MAX;
static _Atomic uint64_t g_lane_target[LANES];
static _Atomic uint64_t g_lane_asked[LANES];
static _Atomic uint64_t g_lane_asked_bad;
static _Atomic uint64_t g_lane_bad_ctx;
static int g_lane_ctx;
/* when set, the facade-wide query answers one microsecond before the clock */
static atomic_bool g_facade_trails_clock;

static uint64_t facade_deadline(void *ctx)
{
    (void)ctx;
    if (atomic_load(&g_facade_trails_clock))
        return atomic_load(&g_now_us) - 1;
    return atomic_load(&g_facade_target);
}

static uint64_t lane_deadline(void *ctx, uint32_t lane_index)
{
    if (ctx != &g_lane_ctx)
        atomic_fetch_add(&g_lane_bad_ctx, 1);
    if (lane_index >= LANES) {
        atomic_fetch_add(&g_lane_asked_bad, 1);
        return UINT64_MAX;
    }
    atomic_fetch_add(&g_lane_asked[lane_index], 1);
    return atomic_load(&g_lane_target[lane_index]);
}

/* --- the pump: counts per lane; lane 0 can be held inside the callback ----- */
static struct {
    pthread_mutex_t mu;
    pthread_cond_t cv;
    uint64_t calls[LANES];
    bool hold;          /* the next lane-0 pump stays inside the callback */
    bool held;          /* ... and is there now */
    bool release;
} g_pump = {
    .mu = PTHREAD_MUTEX_INITIALIZER,
    .cv = PTHREAD_COND_INITIALIZER,
};

static int test_pump(moq_msquic_managed_t *m,
                     moq_msquic_managed_lane_t *lane, uint64_t now_us,
                     void *user)
{
    uint32_t idx = moq_msquic_lane_index(lane);
    struct timespec abs;

    (void)m;
    (void)now_us;
    (void)user;
    pthread_mutex_lock(&g_pump.mu);
    if (idx < LANES)
        g_pump.calls[idx]++;
    if (idx == 0 && g_pump.hold) {
        g_pump.hold = false;
        g_pump.held = true;
        pthread_cond_broadcast(&g_pump.cv);
        guard_deadline(&abs);
        while (!g_pump.release)
            if (pthread_cond_timedwait(&g_pump.cv, &g_pump.mu, &abs) != 0)
                break;
        g_pump.held = false;
    }
    pthread_cond_broadcast(&g_pump.cv);
    pthread_mutex_unlock(&g_pump.mu);
    return 0;
}

static uint64_t pump_calls(uint32_t idx)
{
    pthread_mutex_lock(&g_pump.mu);
    uint64_t n = g_pump.calls[idx];
    pthread_mutex_unlock(&g_pump.mu);
    return n;
}

static void pump_reset(void)
{
    pthread_mutex_lock(&g_pump.mu);
    memset(g_pump.calls, 0, sizeof(g_pump.calls));
    g_pump.hold = false;
    g_pump.held = false;
    g_pump.release = false;
    pthread_mutex_unlock(&g_pump.mu);
}

static void queries_reset(void)
{
    atomic_store(&g_facade_target, UINT64_MAX);
    atomic_store(&g_facade_trails_clock, false);
    for (uint32_t i = 0; i < LANES; i++) {
        atomic_store(&g_lane_target[i], UINT64_MAX);
        atomic_store(&g_lane_asked[i], 0);
    }
    atomic_store(&g_lane_asked_bad, 0);
    atomic_store(&g_lane_bad_ctx, 0);
    atomic_store(&g_now_step, 0);
}

/* --- rig ------------------------------------------------------------------- */
struct rig {
    fake_msq_t fake;
    moq_msquic_managed_t *m;
    moq_msquic_managed_lane_t *lane[LANES];
};

enum { USE_FACADE = 1u, USE_LANE = 2u };

static bool rig_up(struct rig *r, unsigned queries, size_t cfg_size,
                   bool doorbells)
{
    moq_msquic_managed_cfg_t cfg;

    memset(r, 0, sizeof(*r));
    fake_msq_init(&r->fake, false);
    moq_msquic_managed_cfg_init_sized(&cfg, sizeof(cfg));
    cfg.alloc = moq_alloc_default();
    cfg.perspective = MOQ_PERSPECTIVE_SERVER;
    cfg.max_connections = 4;
    cfg.lane_count = LANES;
    cfg.on_lane_pump = test_pump;
    if (queries & USE_FACADE)
        cfg.app_deadline_us = facade_deadline;
    if (queries & USE_LANE) {
        cfg.app_lane_deadline_us = lane_deadline;
        cfg.app_lane_deadline_ctx = &g_lane_ctx;
    }
    cfg.struct_size = (uint32_t)cfg_size;
    moq_msq_test_no_doorbell = !doorbells;
    moq_result_t rc = moq_msq_test_managed_create_lanes_only_api(
        &cfg, fake_msq_table(&r->fake), &r->m);
    moq_msq_test_no_doorbell = false;
    if (rc != MOQ_OK || r->m == NULL) {
        CHECK(rc == MOQ_OK);
        return false;
    }
    for (uint32_t i = 0; i < LANES; i++) {
        r->lane[i] = moq_msquic_managed_lane(r->m, i);
        CHECK(r->lane[i] != NULL);
    }
    return true;
}

static void rig_down(struct rig *r)
{
    pthread_mutex_lock(&g_pump.mu);
    g_pump.release = true;
    pthread_cond_broadcast(&g_pump.cv);
    pthread_mutex_unlock(&g_pump.mu);
    if (r->m != NULL) {
        CHECK(moq_msquic_managed_stop(r->m) == MOQ_OK);
        moq_msquic_managed_destroy(r->m);
        r->m = NULL;
    }
}

static moq_msq_test_step_t step(struct rig *r, uint32_t lane)
{
    return moq_msq_test_lane_step(r->lane[lane]);
}

/* --- served once ----------------------------------------------------------- */

/* A facade-wide deadline that has fallen due pumps each lane once. Stepping
 * again finds the lane idle while the query keeps answering the same instant
 * -- the instant another lane, or the application, has not moved. A later
 * instant earns exactly one more pump. */
static void t_past_deadline_pumps_once(void)
{
    int before = failures;
    struct rig r;

    queries_reset();
    pump_reset();
    atomic_store(&g_now_us, 1000000);
    if (!rig_up(&r, USE_FACADE, sizeof(moq_msquic_managed_cfg_t), false))
        return;

    atomic_store(&g_facade_target, 1000000 - 500);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_TICKED);
    CHECK(pump_calls(0) == 1);
    for (int i = 0; i < 8; i++)
        CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(pump_calls(0) == 1);

    /* the other lane, under the same facade-wide answer: one pump, then idle */
    CHECK(step(&r, 1) == MOQ_MSQ_TEST_STEP_TICKED);
    CHECK(step(&r, 1) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(pump_calls(1) == 1);

    /* the same instant, the clock moved on: still served */
    atomic_store(&g_now_us, 1000500);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);

    /* a later instant, once due, is a new deadline: one pump */
    atomic_store(&g_facade_target, 1000700);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);     /* not due yet */
    atomic_store(&g_now_us, 1000700);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_TICKED);   /* exactly at it */
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(pump_calls(0) == 2);

    /* a wake still pumps an idle lane holding a served deadline, and that
     * pump does not make the served instant due again */
    CHECK(moq_msquic_lane_wake(r.lane[0]) == MOQ_OK);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_PUMPED);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(pump_calls(0) == 3);

    atomic_store(&g_facade_target, UINT64_MAX);
    rig_down(&r);
    if (failures == before)
        printf("PASS: a due application deadline pumps a lane once\n");
}

/* A pump that ran before the deadline does not serve it; one at or after it
 * does, whatever caused the pump. */
static void t_served_by_any_later_pump(void)
{
    int before = failures;
    struct rig r;

    queries_reset();
    pump_reset();
    atomic_store(&g_now_us, 2000000);
    if (!rig_up(&r, USE_FACADE, sizeof(moq_msquic_managed_cfg_t), false))
        return;

    /* a wake-driven pump BEFORE the deadline */
    atomic_store(&g_facade_target, 2000100);
    CHECK(moq_msquic_lane_wake(r.lane[0]) == MOQ_OK);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_PUMPED);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);
    /* the deadline arrives: that earlier pump did not serve it */
    atomic_store(&g_now_us, 2000100);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_TICKED);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);

    /* a wake-driven pump AFTER the next deadline fell due serves it: no
     * deadline pass follows */
    atomic_store(&g_facade_target, 2000200);
    atomic_store(&g_now_us, 2000300);
    CHECK(moq_msquic_lane_wake(r.lane[0]) == MOQ_OK);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_PUMPED);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(pump_calls(0) == 3);

    atomic_store(&g_facade_target, UINT64_MAX);
    rig_down(&r);
    if (failures == before)
        printf("PASS: any pump at or after a deadline serves it\n");
}

/* --- per lane -------------------------------------------------------------- */

/* Each lane asks the per-lane query for itself only and folds only its own
 * answer: lane 1's deadline, future or due, never pumps lane 0. */
static void t_lane_query_is_per_lane(void)
{
    int before = failures;
    struct rig r;

    queries_reset();
    pump_reset();
    atomic_store(&g_now_us, 3000000);
    if (!rig_up(&r, USE_LANE, sizeof(moq_msquic_managed_cfg_t), false))
        return;

    atomic_store(&g_lane_target[1], 3000100);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(step(&r, 1) == MOQ_MSQ_TEST_STEP_IDLE);
    atomic_store(&g_now_us, 3000100);
    /* lane 1's deadline is due: lane 0 stays idle however often it looks */
    for (int i = 0; i < 4; i++)
        CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(pump_calls(0) == 0);
    CHECK(step(&r, 1) == MOQ_MSQ_TEST_STEP_TICKED);
    CHECK(step(&r, 1) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(pump_calls(1) == 1);
    /* lane 0's own deadline is lane 0's alone */
    atomic_store(&g_lane_target[0], 3000100);
    CHECK(step(&r, 1) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_TICKED);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(pump_calls(0) == 1);
    CHECK(pump_calls(1) == 1);

    /* every query named an existing lane, with the configured context, and
     * each lane asked */
    CHECK(atomic_load(&g_lane_asked_bad) == 0);
    CHECK(atomic_load(&g_lane_bad_ctx) == 0);
    CHECK(atomic_load(&g_lane_asked[0]) > 0);
    CHECK(atomic_load(&g_lane_asked[1]) > 0);

    atomic_store(&g_lane_target[0], UINT64_MAX);
    atomic_store(&g_lane_target[1], UINT64_MAX);
    rig_down(&r);
    if (failures == before)
        printf("PASS: a lane folds only its own per-lane deadline\n");
}

/* Both queries set: a lane folds both, the earlier one wins. */
static void t_both_queries_fold(void)
{
    int before = failures;
    struct rig r;

    queries_reset();
    pump_reset();
    atomic_store(&g_now_us, 4000000);
    if (!rig_up(&r, USE_FACADE | USE_LANE, sizeof(moq_msquic_managed_cfg_t),
                false))
        return;

    atomic_store(&g_facade_target, 4000200);
    atomic_store(&g_lane_target[0], 4000100);
    atomic_store(&g_now_us, 4000100);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_TICKED);   /* the lane's own */
    CHECK(step(&r, 1) == MOQ_MSQ_TEST_STEP_IDLE);
    atomic_store(&g_now_us, 4000200);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_TICKED);   /* the facade-wide */
    CHECK(step(&r, 1) == MOQ_MSQ_TEST_STEP_TICKED);
    CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(step(&r, 1) == MOQ_MSQ_TEST_STEP_IDLE);
    CHECK(pump_calls(0) == 2);
    CHECK(pump_calls(1) == 1);

    atomic_store(&g_facade_target, UINT64_MAX);
    atomic_store(&g_lane_target[0], UINT64_MAX);
    rig_down(&r);
    if (failures == before)
        printf("PASS: a lane folds the facade-wide and its own deadline\n");
}

/* The per-lane block is read only when struct_size covers all of it: a size
 * that ends inside it, and the size that ends just before it, leave the lane
 * with no per-lane query at all -- a due answer is never asked for. */
static void t_lane_query_gate(void)
{
    int before = failures;
    const size_t sizes[] = {
        offsetof(moq_msquic_managed_cfg_t, app_lane_deadline_us),
        offsetof(moq_msquic_managed_cfg_t, app_lane_deadline_ctx),
        offsetof(moq_msquic_managed_cfg_t, app_lane_deadline_ctx) +
            sizeof(void *) - 1u,
    };

    for (size_t k = 0; k < sizeof(sizes) / sizeof(sizes[0]); k++) {
        struct rig r;

        queries_reset();
        pump_reset();
        atomic_store(&g_now_us, 5000000);
        if (!rig_up(&r, USE_LANE, sizes[k], false))
            return;
        atomic_store(&g_lane_target[0], 5000000 - 1);
        CHECK(step(&r, 0) == MOQ_MSQ_TEST_STEP_IDLE);
        CHECK(pump_calls(0) == 0);
        CHECK(atomic_load(&g_lane_asked[0]) == 0);
        atomic_store(&g_lane_target[0], UINT64_MAX);
        rig_down(&r);
    }
    if (failures == before)
        printf("PASS: the per-lane query is honored only when fully covered\n");
}

/* --- never blocks ---------------------------------------------------------- */

struct waker {
    moq_msquic_managed_lane_t *lane;
    moq_result_t rc;
    bool done;
    pthread_mutex_t mu;
    pthread_cond_t cv;
};

static void *waker_main(void *arg)
{
    struct waker *w = arg;
    moq_result_t rc = moq_msquic_lane_wake(w->lane);

    pthread_mutex_lock(&w->mu);
    w->rc = rc;
    w->done = true;
    pthread_cond_broadcast(&w->cv);
    pthread_mutex_unlock(&w->mu);
    return NULL;
}

static bool waker_await(struct waker *w)
{
    struct timespec abs;
    bool done;

    guard_deadline(&abs);
    pthread_mutex_lock(&w->mu);
    while (!w->done)
        if (pthread_cond_timedwait(&w->cv, &w->mu, &abs) != 0)
            break;
    done = w->done;
    pthread_mutex_unlock(&w->mu);
    return done;
}

static bool pump_await(uint32_t idx, uint64_t want, bool held)
{
    struct timespec abs;
    bool ok;

    guard_deadline(&abs);
    pthread_mutex_lock(&g_pump.mu);
    while (g_pump.calls[idx] < want || (held && !g_pump.held))
        if (pthread_cond_timedwait(&g_pump.cv, &g_pump.mu, &abs) != 0)
            break;
    ok = g_pump.calls[idx] >= want && (!held || g_pump.held);
    pthread_mutex_unlock(&g_pump.mu);
    return ok;
}

/* A thread that is none of this facade's lanes -- an application thread, or
 * another facade's lane -- wakes a lane whose doorbell is inside a pump, so
 * holds the lane's lock. The wake returns while the pump is still held, and
 * is not lost: the lane pumps again once the held pump returns. */
static void t_foreign_wake_never_blocks(void)
{
    int before = failures;
    struct rig r;
    struct waker w = {
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    moq_msquic_lane_stats_t a, b;
    pthread_t th;

    queries_reset();
    pump_reset();
    atomic_store(&g_now_us, 6000000);
    if (!rig_up(&r, 0, sizeof(moq_msquic_managed_cfg_t), true))
        return;
    CHECK(moq_msquic_lane_get_stats(r.lane[0], &a, sizeof(a)) == MOQ_OK);

    pthread_mutex_lock(&g_pump.mu);
    g_pump.hold = true;
    pthread_mutex_unlock(&g_pump.mu);
    CHECK(moq_msquic_lane_wake(r.lane[0]) == MOQ_OK);
    CHECK(pump_await(0, 1, true));

    w.lane = r.lane[0];
    w.rc = MOQ_ERR_INTERNAL;
    CHECK(pthread_create(&th, NULL, waker_main, &w) == 0);
    bool returned = waker_await(&w);
    CHECK(returned);                    /* returned while the pump is held */
    pthread_mutex_lock(&g_pump.mu);
    CHECK(g_pump.held);
    g_pump.release = true;
    pthread_cond_broadcast(&g_pump.cv);
    pthread_mutex_unlock(&g_pump.mu);
    pthread_join(th, NULL);
    CHECK(w.rc == MOQ_OK);

    /* the held pump returned; the wake it raced is delivered */
    CHECK(pump_await(0, 2, false));
    CHECK(moq_msquic_lane_get_stats(r.lane[0], &b, sizeof(b)) == MOQ_OK);
    CHECK(b.wakes_external == a.wakes_external + 2);
    CHECK(b.wakes_cross_lane == a.wakes_cross_lane);
    CHECK(b.wakes_same_lane == a.wakes_same_lane);
    CHECK(pump_calls(1) == 0);          /* only the woken lane pumped */
    rig_down(&r);
    if (failures == before)
        printf("PASS: a foreign wake returns while the lane is pumping\n");
}

/* A facade-wide deadline query can tie two lanes on different facades into
 * a wait cycle, and neither half of it may hold. The query answers a due
 * deadline that only a foreign lane (here, a plain thread standing in for a
 * WebTransport lane) clears, and that lane must first wake this facade's
 * lane 0 -- as a cross-shard push does -- before it can finish its pump and
 * clear the deadline. Lane 0 must neither keep pumping for a deadline its
 * pumps cannot move nor hold its lock against the wake, so the foreign wake
 * returns, the deadline clears and the wake is delivered. */
static void t_shared_deadline_cycle(void)
{
    int before = failures;
    struct rig r;
    struct waker w = {
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    pthread_t th;

    queries_reset();
    pump_reset();
    atomic_store(&g_now_us, 8000000);
    if (!rig_up(&r, USE_FACADE, sizeof(moq_msquic_managed_cfg_t), true))
        return;

    /* the foreign lane's deadline falls due; lane 0 looks at once */
    atomic_store(&g_facade_target, 8000000 - 1);
    moq_msq_test_bell_ring_raw(r.lane[0]);
    CHECK(pump_await(0, 1, false));

    /* lane 0 pumped for the deadline once, not on every pass of its
     * doorbell */
    uint64_t ticked = pump_calls(0);
    CHECK(ticked == 1);

    /* the foreign lane's pump: wake lane 0, then clear its own deadline */
    w.lane = r.lane[0];
    w.rc = MOQ_ERR_INTERNAL;
    CHECK(pthread_create(&th, NULL, waker_main, &w) == 0);
    bool returned = waker_await(&w);
    CHECK(returned);
    atomic_store(&g_facade_target, UINT64_MAX);
    /* the wake is delivered */
    if (returned)
        CHECK(pump_await(0, ticked + 1, false));

    rig_down(&r);       /* stop also releases a lane that kept its lock */
    pthread_join(th, NULL);
    CHECK(w.rc == MOQ_OK);
    if (failures == before)
        printf("PASS: a shared due deadline cannot lock out a foreign wake\n");
}

struct reader {
    moq_msquic_managed_lane_t *lane;
    int reads;
    int ok;
    bool done;
    pthread_mutex_t mu;
    pthread_cond_t cv;
};

static void *reader_main(void *arg)
{
    struct reader *rd = arg;
    int ok = 0;

    for (int i = 0; i < rd->reads; i++) {
        moq_msquic_lane_stats_t st;

        if (moq_msquic_lane_get_stats(rd->lane, &st, sizeof(st)) == MOQ_OK)
            ok++;
    }
    pthread_mutex_lock(&rd->mu);
    rd->ok = ok;
    rd->done = true;
    pthread_cond_broadcast(&rd->cv);
    pthread_mutex_unlock(&rd->mu);
    return NULL;
}

/* A facade-wide query that always answers "just before now" is a new, due
 * deadline at every look, so the lane runs deadline pass after deadline pass.
 * Between passes it releases its lock: an external reader, which takes that
 * lock, completes all its reads while the passes continue. */
static void t_deadline_passes_release_the_lane(void)
{
    int before = failures;
    struct rig r;
    struct reader rd = {
        .reads = 32,
        .mu = PTHREAD_MUTEX_INITIALIZER,
        .cv = PTHREAD_COND_INITIALIZER,
    };
    moq_msquic_lane_stats_t a, b;
    pthread_t th;
    struct timespec abs;

    queries_reset();
    pump_reset();
    atomic_store(&g_now_us, 7000000);
    if (!rig_up(&r, USE_FACADE, sizeof(moq_msquic_managed_cfg_t), true))
        return;
    CHECK(moq_msquic_lane_get_stats(r.lane[0], &a, sizeof(a)) == MOQ_OK);

    atomic_store(&g_now_step, 10);
    atomic_store(&g_facade_trails_clock, true);
    /* the passes have begun */
    moq_msq_test_bell_ring_raw(r.lane[0]);
    CHECK(pump_await(0, 4, false));

    rd.lane = r.lane[0];
    CHECK(pthread_create(&th, NULL, reader_main, &rd) == 0);
    guard_deadline(&abs);
    pthread_mutex_lock(&rd.mu);
    while (!rd.done)
        if (pthread_cond_timedwait(&rd.cv, &rd.mu, &abs) != 0)
            break;
    bool done = rd.done;
    pthread_mutex_unlock(&rd.mu);
    CHECK(done);
    /* still passing after the reads: they got in between passes, not after
     * the passes stopped */
    uint64_t mid = pump_calls(0);
    CHECK(pump_await(0, mid + 2, false));

    /* release: stop is observed without the lane lock */
    atomic_store(&g_facade_trails_clock, false);
    atomic_store(&g_now_step, 0);
    CHECK(moq_msquic_managed_stop(r.m) == MOQ_OK);
    pthread_join(th, NULL);
    CHECK(rd.ok == rd.reads);
    CHECK(moq_msquic_lane_get_stats(r.lane[0], &b, sizeof(b)) == MOQ_OK);
    CHECK(b.deadline_sweeps >= a.deadline_sweeps + 6);
    rig_down(&r);
    if (failures == before)
        printf("PASS: deadline passes release the lane between passes\n");
}

int main(void)
{
    moq_msq_test_now_us = test_now_us;

    t_past_deadline_pumps_once();
    t_served_by_any_later_pump();
    t_lane_query_is_per_lane();
    t_both_queries_fold();
    t_lane_query_gate();
    t_foreign_wake_never_blocks();
    t_shared_deadline_cycle();
    t_deadline_passes_release_the_lane();

    moq_msq_test_now_us = NULL;
    if (failures == 0)
        printf("PASS: msquic_lane_deadline\n");
    else
        fprintf(stderr, "FAIL: msquic_lane_deadline (%d)\n", failures);
    return failures != 0;
}
