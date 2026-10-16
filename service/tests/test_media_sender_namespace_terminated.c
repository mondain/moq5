#include <moq/media_sender.h>
#include <moq/sim.h>
#include <stdio.h>
#include <pthread.h>

moq_result_t moq_media_sender_test_construct(const moq_media_sender_cfg_t *, moq_media_sender_t **);
void moq_media_sender_test_free(moq_media_sender_t *);
void moq_media_sender_test_pump(moq_media_sender_t *, moq_session_t *, uint64_t);
void moq_media_sender_test_set_ep(moq_media_sender_t *, moq_endpoint_t *);
void moq_media_sender_test_set_callbacks(moq_media_sender_t *, const moq_media_sender_callbacks_t *);
moq_endpoint_t *moq_endpoint_test_make_bare(void);
void moq_endpoint_test_free_bare(moq_endpoint_t *);
void moq_media_sender_test_set_wait_entry_hook(void (*)(void *), void *);

static unsigned failures, setups;
#define CHECK(x) do { if (!(x)) { fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); ++failures; } } while (0)
#define NEED(x) do { if (!(x)) { fprintf(stderr, "SETUP:%d: %s\n", __LINE__, #x); ++setups; return false; } } while (0)
typedef struct fixture {
    moq_simpair_t *pair;
    moq_endpoint_t *ep;
    moq_media_sender_t *sender[2];
    moq_media_track_t *track[2];
    moq_stream_ref_t refs[2];
    unsigned opened, closed;
    uint64_t code;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool entered, completed;
    moq_result_t write_result;
} fixture_t;
static moq_result_t write_one(fixture_t *f)
{
    moq_media_send_object_t o = {.struct_size = sizeof(o), .starts_group = true,
        .ends_group = true, .is_sync = true};
    moq_media_metadata_t metadata = {.struct_size = sizeof(metadata)};
    moq_loc_metadata_init_sized(&metadata.loc, sizeof(metadata.loc));
    metadata.loc.present = MOQ_LOC_METADATA_HAS_TIMESTAMP;
    o.metadata = &metadata;
    const uint8_t data[] = {1};
    moq_result_t rc = moq_rcbuf_create(moq_alloc_default(), data, sizeof(data), &o.payload);
    if (rc != MOQ_OK) return rc;
    rc = moq_media_sender_write(f->sender[0], f->track[0], &o);
    if (rc != MOQ_OK) moq_rcbuf_decref(o.payload);
    return rc;
}
static void entered_wait(void *ctx)
{
    fixture_t *f = ctx;
    pthread_mutex_lock(&f->mutex); f->entered = true;
    pthread_cond_signal(&f->condition); pthread_mutex_unlock(&f->mutex);
}
static void *writer(void *ctx)
{
    fixture_t *f = ctx;
    moq_result_t rc = write_one(f);
    pthread_mutex_lock(&f->mutex); f->write_result = rc; f->completed = true;
    pthread_cond_signal(&f->condition); pthread_mutex_unlock(&f->mutex);
    return NULL;
}
static void closed(void *ctx, moq_media_sender_t *s, bool fatal, uint64_t code)
{
    fixture_t *f = ctx;
    CHECK(s == f->sender[0]); CHECK(fatal);
    ++f->closed; f->code = code;
}
static void trace(void *ctx, const moq_sim_trace_record_t *r)
{
    fixture_t *f = ctx;
    if (r->kind == MOQ_SIM_TRACE_ACTION && r->from == MOQ_PERSPECTIVE_CLIENT &&
        r->action_kind == MOQ_ACTION_OPEN_BIDI_STREAM && f->opened < 2)
        f->refs[f->opened++] = r->stream_ref;
}
static void drain(moq_session_t *s)
{
    moq_event_t e; while (moq_session_poll_events(s, &e, 1)) moq_event_cleanup(&e);
}
static bool run(fixture_t *f, bool accepted, bool stop, uint64_t code)
{
    moq_simpair_cfg_t cfg = MOQ_SIMPAIR_CFG_INIT;
    cfg.alloc = moq_alloc_default(); cfg.version = MOQ_VERSION_DRAFT_22;
    cfg.trace_fn = trace; cfg.trace_ctx = f;
    NEED(moq_simpair_create(&cfg, &f->pair) == MOQ_OK);
    NEED(moq_simpair_start(f->pair) == MOQ_OK);
    NEED(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    moq_session_t *client = moq_simpair_client(f->pair), *server = moq_simpair_server(f->pair);
    drain(client); drain(server);
    f->ep = moq_endpoint_test_make_bare(); NEED(f->ep != NULL);
    for (unsigned i = 0; i < 2; ++i) {
        moq_media_sender_cfg_t sc; moq_media_sender_cfg_init_sized(&sc, sizeof(sc));
        moq_bytes_t part = i ? MOQ_BYTES_LITERAL("b") : MOQ_BYTES_LITERAL("a");
        sc.namespace_ = (moq_namespace_t){&part, 1}; sc.validate_cmaf = false;
        sc.backpressure = MOQ_MEDIA_SEND_BP_BLOCK_TIMEOUT;
        sc.block_timeout_us = 10000000;
        sc.queue_max_objects = sc.pre_ready_max_objects = 1;
        sc.companion_profile = MOQ_MEDIA_COMPANION_D22_MSF01_LOC04_PUBLIC;
        NEED(moq_media_sender_test_construct(&sc, &f->sender[i]) == MOQ_OK);
        moq_media_sender_test_set_ep(f->sender[i], f->ep);
        moq_media_sender_callbacks_t cb; moq_media_sender_callbacks_init_sized(&cb, sizeof(cb));
        cb.ctx = f; cb.on_closed = closed;
        moq_media_sender_test_set_callbacks(f->sender[i], &cb);
        moq_media_track_cfg_t tc; moq_media_track_cfg_init(&tc);
        tc.name = MOQ_BYTES_LITERAL("v"); tc.codec = MOQ_BYTES_LITERAL("av01");
        tc.media_type = MOQ_MEDIA_TYPE_VIDEO; tc.packaging = MOQ_MEDIA_PACKAGING_RAW; tc.bitrate = 1000;
        NEED(moq_media_sender_add_track(f->sender[i], &tc, &f->track[i]) == MOQ_OK);
        moq_media_sender_test_pump(f->sender[i], client, 1000 + i);
    }
    NEED(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    moq_announcement_t peer[2]; unsigned requests = 0; moq_event_t e;
    while (moq_session_poll_events(server, &e, 1)) {
        CHECK(e.kind == MOQ_EVENT_NAMESPACE_PUBLISHED);
        if (e.kind == MOQ_EVENT_NAMESPACE_PUBLISHED && requests < 2) peer[requests++] = e.u.namespace_published.ann;
        moq_event_cleanup(&e);
    }
    NEED(requests == 2 && f->opened == 2);
    if (accepted) {
        moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
        NEED(moq_session_accept_namespace(server, peer[0], &ac, 1002) == MOQ_OK);
        NEED(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
        moq_media_sender_test_pump(f->sender[0], client, 1003);
    }
    NEED(write_one(f) == MOQ_OK);
    pthread_t thread;
    moq_media_sender_test_set_wait_entry_hook(entered_wait, f);
    NEED(pthread_create(&thread, NULL, writer, f) == 0);
    pthread_mutex_lock(&f->mutex);
    while (!f->entered && !f->completed) pthread_cond_wait(&f->condition, &f->mutex);
    CHECK(f->entered && !f->completed);
    pthread_mutex_unlock(&f->mutex);
    CHECK((stop ? moq_session_on_bidi_stream_stop(client, f->refs[0], code, 1004)
                : moq_session_on_bidi_stream_reset(client, f->refs[0], code, 1004)) == MOQ_OK);
    moq_media_sender_test_pump(f->sender[0], client, 1005);
    CHECK(pthread_join(thread, NULL) == 0);
    moq_media_sender_test_set_wait_entry_hook(NULL, NULL);
    CHECK(f->completed && f->write_result == MOQ_ERR_CLOSED);
    CHECK(moq_media_sender_is_fatal(f->sender[0]));
    CHECK(moq_media_sender_fatal_code(f->sender[0]) == 6);
    CHECK(f->closed == 1 && f->code == 6);
    CHECK(moq_media_sender_wait(f->sender[0], 0) == MOQ_ERR_CLOSED);
    moq_media_send_object_t object = {.struct_size = sizeof(object)};
    const uint8_t data[] = {1};
    NEED(moq_rcbuf_create(moq_alloc_default(), data, sizeof(data), &object.payload) == MOQ_OK);
    moq_result_t written = moq_media_sender_write(f->sender[0], f->track[0], &object);
    CHECK(written == MOQ_ERR_CLOSED);
    if (written != MOQ_OK) moq_rcbuf_decref(object.payload);
    moq_media_sender_test_pump(f->sender[0], client, 1006);
    CHECK(f->closed == 1);
    CHECK(!moq_endpoint_is_closed(f->ep) && !moq_endpoint_is_fatal(f->ep));
    CHECK(!moq_media_sender_is_fatal(f->sender[1]));
    moq_accept_namespace_cfg_t ac; moq_accept_namespace_cfg_init(&ac);
    NEED(moq_session_accept_namespace(server, peer[1], &ac, 1007) == MOQ_OK);
    NEED(moq_simpair_run_until_quiescent(f->pair, 32, NULL) == MOQ_OK);
    moq_media_sender_test_pump(f->sender[1], client, 1008);
    CHECK(moq_media_sender_is_ready(f->sender[1]));
    CHECK(moq_session_state(client) == MOQ_SESS_ESTABLISHED);
    return true;
}
int main(void)
{
    for (unsigned a = 0; a < 2; ++a) for (unsigned s = 0; s < 2; ++s) for (unsigned c = 0; c < 2; ++c) {
        fixture_t f = {0};
        pthread_mutex_init(&f.mutex, NULL); pthread_cond_init(&f.condition, NULL);
        (void)run(&f, a != 0, s != 0, c ? UINT64_MAX : 0);
        for (unsigned i = 0; i < 2; ++i) if (f.sender[i]) {
            moq_media_sender_test_set_ep(f.sender[i], NULL);
            moq_media_sender_test_free(f.sender[i]);
        }
        moq_endpoint_test_free_bare(f.ep); moq_simpair_destroy(f.pair);
        pthread_cond_destroy(&f.condition); pthread_mutex_destroy(&f.mutex);
    }
    printf("media_sender_namespace_terminated: 8 cases, %u failures, %u setup failures\n", failures, setups);
    return setups ? 2 : failures ? 1 : 0;
}
