/*
 * MPEG-2 TS through the service tier over a real threaded-picoquic loopback,
 * for draft-16 and draft-18 each pinned exactly. Media endpoints are clients
 * and there is no relay in this tree, so each half meets a raw-session test
 * server:
 *
 *  - Sender: a moq_media_sender writes a generated TS (188, 192, and an MPTS
 *    through the per-program filter its record implies) with
 *    write_mpeg2ts(); the server subscribes and checks every Object it
 *    receives -- the concatenated payloads (byte-identical to the source for
 *    a single program), Groups that open with the PAT and a random access
 *    point, continuous continuity counters, and the END_OF_TRACK.
 *  - Receiver: the server publishes packetizer output on a TS track whose
 *    first Groups go out before anyone subscribes; an auto-subscribing
 *    moq_media_receiver joins mid-stream and must start at a Group with the
 *    PAT and a random access point, then deliver every later Object
 *    unchanged with no gap and no unknown tail.
 */
#include <moq/media_receiver.h>
#include <moq/media_sender.h>
#include <moq/mpeg2ts.h>
#include <moq/msf.h>
#include <moq/picoquic_threaded.h>
#include <moq/publisher.h>
#include <moq/session.h>
#include "test_support.h"
#include "ts_stream.h"

#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static int failures = 0;

static const char *vname(moq_version_t v)
{
    return v == MOQ_VERSION_DRAFT_18 ? "d18" : "d16";
}

/* -- Shared server plumbing ----------------------------------------------- */

typedef int (*pump_fn_t)(moq_pq_threaded_t *, moq_pq_threaded_lane_t *,
                         uint64_t, void *);

static moq_pq_threaded_t *start_server(const char *cert, const char *key,
                                       pump_fn_t pump, void *ctx,
                                       int *out_port)
{
    /* A port range disjoint from the other network tests. */
    static int calls = 0;
    int base = 21500 + (int)(getpid() % 997) + (calls++ * 131);
    static const char *const alpn[] = { "moqt-18", "moqt-16" };
    for (int attempt = 0; attempt < 8; attempt++) {
        moq_pq_threaded_cfg_t cfg;
        moq_pq_threaded_cfg_init_sized(&cfg, sizeof(cfg));
        cfg.alloc = moq_alloc_default();
        cfg.perspective = MOQ_PERSPECTIVE_SERVER;
        cfg.cert_path = cert;
        cfg.key_path = key;
        cfg.port = base + attempt * 13;
        cfg.send_request_capacity = true;
        cfg.initial_request_capacity = 16;
        cfg.on_lane_pump = pump;
        cfg.on_lane_pump_ctx = ctx;
        /* The server picks the first of these the client offers; each client
         * offers exactly one draft. */
        cfg.alpn_list = alpn;
        cfg.alpn_count = 2;
        moq_pq_threaded_t *srv = NULL;
        if (moq_pq_threaded_create(&cfg, &srv) == MOQ_OK) {
            *out_port = cfg.port;
            return srv;
        }
    }
    return NULL;
}

static void stop_server(moq_pq_threaded_t *srv)
{
    if (!srv) return;
    moq_pq_threaded_stop(srv);
    moq_pq_threaded_destroy(srv);
}

/* An endpoint cfg for the loopback server, offering exactly `ver`. */
static moq_endpoint_cfg_t ep_cfg(char *url, size_t cap, int port,
                                 const moq_version_t *ver)
{
    moq_endpoint_cfg_t c;
    moq_endpoint_cfg_init_sized(&c, sizeof(c));
    snprintf(url, cap, "moqt://127.0.0.1:%d", port);
    c.url = (moq_bytes_t){ (const uint8_t *)url, strlen(url) };
    c.insecure_skip_verify = true;
    c.versions.struct_size = sizeof(moq_version_offer_t);
    c.versions.policy = MOQ_VERSION_POLICY_EXACT;
    c.versions.versions = ver;
    c.versions.version_count = 1;
    return c;
}

static moq_bytes_t g_ns[2] = { { (const uint8_t *)"svc", 3 },
                               { (const uint8_t *)"ts", 2 } };

/* -- Stream checks ---------------------------------------------------------- */

#define MAX_STREAM (64u * 1024u)

static size_t ts_off(uint32_t ps) { return ps == 192u ? 4u : 0u; }
static uint16_t pkt_pid(const uint8_t *p)
{
    return (uint16_t)(((p[1] & 0x1F) << 8) | p[2]);
}

/* Continuity counters advance by one per payload-carrying packet of each
 * PID, across the whole delivered sequence. */
static bool cc_continuous(const uint8_t *d, size_t len, uint32_t ps)
{
    int last[0x2000];
    for (size_t i = 0; i < 0x2000; i++) last[i] = -1;
    for (size_t o = 0; o + ps <= len; o += ps) {
        const uint8_t *p = d + o + ts_off(ps);
        uint16_t pid = pkt_pid(p);
        int cc = p[3] & 0x0F;
        if (!(p[3] & 0x10)) continue;                /* no payload */
        if (last[pid] >= 0 && cc != ((last[pid] + 1) & 0x0F)) return false;
        last[pid] = cc;
    }
    return true;
}

/* An Object that opens a Group starts with the PAT and holds the video
 * random access point (adaptation field with random_access_indicator). */
static bool group_start_ok(const uint8_t *d, size_t len, uint32_t ps)
{
    if (len < ps || d[ts_off(ps)] != 0x47 || pkt_pid(d + ts_off(ps)) != 0)
        return false;
    for (size_t o = 0; o + ps <= len; o += ps) {
        const uint8_t *p = d + o + ts_off(ps);
        if (pkt_pid(p) == TS_VIDEO_PID && (p[3] & 0x20) && p[4] > 0 &&
            (p[5] & 0x40))
            return true;
    }
    return false;
}

/* -- Sender half -------------------------------------------------------------- */

typedef struct snd_srv {
    pthread_mutex_t    mu;
    atomic_bool        subscribed;
    bool               ns_ok;
    bool               sub_issued;
    moq_subscription_t sub;
    uint32_t           ps;
    /* What arrived, in arrival order: Groups ride separate streams, so a
     * later Group can arrive before the tail of an earlier one. */
    uint8_t            data[MAX_STREAM];
    size_t             len;
    int                objects;
    bool               eog_ok;
    uint64_t           seen_g[256], seen_o[256];
    size_t             seen_off[256], seen_len[256];
    atomic_int         eot;
} snd_srv_t;

static snd_srv_t g_snd;

static int snd_pump(moq_pq_threaded_t *t, moq_pq_threaded_lane_t *lane,
                    uint64_t now_us, void *ctx)
{
    (void)lane;
    snd_srv_t *st = (snd_srv_t *)ctx;
    moq_session_t *s = moq_pq_threaded_session(t);
    if (!s || moq_session_state(s) != MOQ_SESS_ESTABLISHED) return 0;
    moq_event_t ev;
    while (moq_session_poll_events(s, &ev, 1) == 1) {
        if (ev.kind == MOQ_EVENT_NAMESPACE_PUBLISHED) {
            moq_accept_namespace_cfg_t acc;
            moq_accept_namespace_cfg_init(&acc);
            (void)moq_session_accept_namespace(s, ev.u.namespace_published.ann,
                                               &acc, now_us);
            st->ns_ok = true;
        } else if (ev.kind == MOQ_EVENT_SUBSCRIBE_OK) {
            atomic_store(&st->subscribed, true);
        } else if (ev.kind == MOQ_EVENT_OBJECT_RECEIVED &&
                   st->sub_issued &&
                   moq_subscription_eq(ev.u.object_received.sub, st->sub)) {
            const moq_object_received_event_t *o = &ev.u.object_received;
            pthread_mutex_lock(&st->mu);
            if (o->status == MOQ_OBJECT_END_OF_TRACK) {
                atomic_fetch_add(&st->eot, 1);
            } else if (o->status == MOQ_OBJECT_NORMAL && o->payload) {
                const uint8_t *d = moq_rcbuf_data(o->payload);
                size_t n = moq_rcbuf_len(o->payload);
                if (!o->end_of_group) st->eog_ok = false;
                if (st->objects < 256 && st->len + n <= sizeof(st->data)) {
                    int k = st->objects++;
                    st->seen_g[k] = o->group_id;
                    st->seen_o[k] = o->object_id;
                    st->seen_off[k] = st->len;
                    st->seen_len[k] = n;
                    memcpy(st->data + st->len, d, n);
                    st->len += n;
                } else {
                    st->eog_ok = false;   /* over the capture bound */
                }
            }
            pthread_mutex_unlock(&st->mu);
        }
        moq_event_cleanup(&ev);
    }
    if (st->ns_ok && !st->sub_issued) {
        moq_subscribe_cfg_t sc;
        moq_subscribe_cfg_init(&sc);
        sc.track_namespace = (moq_namespace_t){ g_ns, 2 };
        sc.track_name = (moq_bytes_t){ (const uint8_t *)"prog", 4 };
        sc.filter = MOQ_SUBSCRIBE_FILTER_LARGEST_OBJECT;
        if (moq_session_subscribe(s, &sc, now_us, &st->sub) == MOQ_OK)
            st->sub_issued = true;
    }
    return 0;
}

/* Write the whole source, waiting out backpressure. */
static bool snd_write_all(moq_media_sender_t *s, moq_media_track_t *t,
                          const uint8_t *src, size_t len)
{
    static const size_t chunks[] = { 188 * 3, 1000, 77, 188 * 7 };
    size_t off = 0;
    for (int i = 0; off < len && i < 100000; i++) {
        size_t n = chunks[i % 4];
        if (n > len - off) n = len - off;
        moq_media_send_mpeg2ts_t in;
        memset(&in, 0, sizeof(in));
        in.struct_size = sizeof(in);
        in.data = (moq_bytes_t){ src + off, n };
        size_t c = 0;
        moq_result_t rc = moq_media_sender_write_mpeg2ts(s, t, &in, &c);
        off += c;
        if (rc == MOQ_ERR_WOULD_BLOCK) {
            (void)moq_media_sender_wait(s, 100000);
            continue;
        }
        if (rc != MOQ_OK) {
            fprintf(stderr, "write_mpeg2ts: %d\n", (int)rc);
            return false;
        }
    }
    /* Objects held back by a short write go out on empty calls. */
    for (int i = 0; i < 1000; i++) {
        moq_media_sender_track_stats_t st;
        if (moq_media_sender_get_track_stats(s, t, &st, sizeof(st)) != MOQ_OK)
            return false;
        if (st.mpeg2ts_objects_parked == 0) break;
        (void)moq_media_sender_wait(s, 100000);
        moq_media_send_mpeg2ts_t in;
        memset(&in, 0, sizeof(in));
        in.struct_size = sizeof(in);
        size_t c = 0;
        moq_result_t rc = moq_media_sender_write_mpeg2ts(s, t, &in, &c);
        if (rc != MOQ_OK && rc != MOQ_ERR_WOULD_BLOCK) return false;
    }
    for (int i = 0; i < 1000; i++) {
        moq_result_t rc = moq_media_sender_end_track(s, t);
        if (rc == MOQ_OK) return true;
        if (rc != MOQ_ERR_WOULD_BLOCK) return false;
        (void)moq_media_sender_wait(s, 100000);
    }
    return false;
}

typedef enum { SRC_188, SRC_192, SRC_MPTS } src_kind_t;

static void test_sender_loopback(const char *cert, const char *key,
                                 moq_version_t ver, src_kind_t kind)
{
    int before = failures;
    static const char *const names[] = { "188", "192", "mpts" };
    uint32_t ps = kind == SRC_192 ? 192u : 188u;
    ts_gen_t g;
    ts_gen_init(&g, ps, 1, kind == SRC_MPTS);
    static uint8_t src[MAX_STREAM];
    size_t len = 0;
    for (int i = 0; i < 6; i++) len += ts_gen_gop(&g, src + len, 6);

    memset(&g_snd, 0, sizeof(g_snd));
    pthread_mutex_init(&g_snd.mu, NULL);
    g_snd.ps = ps;
    g_snd.eog_ok = true;
    int port = 0;
    moq_pq_threaded_t *srv = start_server(cert, key, snd_pump, &g_snd, &port);
    if (!srv) { failures++; return; }

    char url[64];
    moq_endpoint_cfg_t ec = ep_cfg(url, sizeof(url), port, &ver);
    moq_media_sender_cfg_t cfg;
    moq_media_sender_cfg_init_live_sized(&cfg, sizeof(cfg));
    cfg.namespace_ = (moq_namespace_t){ g_ns, 2 };
    cfg.endpoint = &ec;
    cfg.backpressure = MOQ_MEDIA_SEND_BP_BLOCK_TIMEOUT;
    moq_media_sender_t *s = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_media_sender_create(&cfg, &s), (int)MOQ_OK);
    if (!s) { stop_server(srv); failures++; return; }

    moq_msf_mpeg2ts_t rec;
    moq_msf_mpeg2ts_init(&rec);
    rec.has_mode = true;
    rec.mode = kind == SRC_MPTS ? MOQ_MSF_MPEG2TS_MODE_PER_PROGRAM
                                : MOQ_MSF_MPEG2TS_MODE_UNMODIFIED_PROGRAM;
    rec.has_packet_size = true;
    rec.packet_size = ps;
    rec.has_program_number = true;
    rec.program_number = 1;
    moq_msf_track_ext_t ext = { .kind = MOQ_MSF_EXT_MPEG2TS,
                                .u = { .mpeg2ts = &rec } };
    moq_media_track_cfg_t tc;
    moq_media_track_cfg_init(&tc);
    tc.name = (moq_bytes_t){ (const uint8_t *)"prog", 4 };
    tc.media_type = MOQ_MEDIA_TYPE_VIDEO;
    tc.packaging = MOQ_MEDIA_PACKAGING_MPEG2TS;
    tc.bitrate = 4000000;
    tc.extensions = &ext;
    tc.extension_count = 1;
    tc.mpeg2ts_object_packets = 4;
    moq_media_track_t *t = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_media_sender_add_track(s, &tc, &t),
                          (int)MOQ_OK);

    for (int i = 0; i < 200 && !(t && moq_media_sender_track_has_subscriber(s, t));
         i++)
        (void)moq_media_sender_wait(s, 50000);
    MOQ_TEST_CHECK(t && moq_media_sender_track_has_subscriber(s, t));
    if (t && moq_media_sender_track_has_subscriber(s, t))
        MOQ_TEST_CHECK(snd_write_all(s, t, src, len));
    for (int i = 0; i < 200 && atomic_load(&g_snd.eot) == 0; i++)
        usleep(20000);

    pthread_mutex_lock(&g_snd.mu);
    MOQ_TEST_CHECK_EQ_INT(atomic_load(&g_snd.eot), 1);
    MOQ_TEST_CHECK(g_snd.eog_ok);
    /* Rebuild Location order (MSFTS: ascending Group, then Object). */
    int idx[256];
    int n = g_snd.objects;
    for (int i = 0; i < n; i++) idx[i] = i;
    for (int i = 1; i < n; i++)
        for (int j = i; j > 0; j--) {
            int a = idx[j - 1], b = idx[j];
            bool gt = g_snd.seen_g[a] > g_snd.seen_g[b] ||
                      (g_snd.seen_g[a] == g_snd.seen_g[b] &&
                       g_snd.seen_o[a] > g_snd.seen_o[b]);
            if (!gt) break;
            idx[j - 1] = b; idx[j] = a;
        }
    static uint8_t ordered[MAX_STREAM];
    size_t olen = 0;
    int groups = 0;
    bool dense = true, starts_ok = true;
    for (int i = 0; i < n; i++) {
        int k = idx[i];
        bool opens = i == 0 || g_snd.seen_g[k] != g_snd.seen_g[idx[i - 1]];
        uint64_t want = opens ? 0 : g_snd.seen_o[idx[i - 1]] + 1;
        if (g_snd.seen_o[k] != want) dense = false;
        if (opens) {
            groups++;
            if (i > 0 && g_snd.seen_g[k] != g_snd.seen_g[idx[i - 1]] + 1)
                dense = false;
            if (!group_start_ok(g_snd.data + g_snd.seen_off[k],
                                g_snd.seen_len[k], ps))
                starts_ok = false;
        }
        memcpy(ordered + olen, g_snd.data + g_snd.seen_off[k],
               g_snd.seen_len[k]);
        olen += g_snd.seen_len[k];
    }
    MOQ_TEST_CHECK(dense && starts_ok);
    MOQ_TEST_CHECK_EQ_INT(groups, 6);
    if (kind == SRC_MPTS) {
        /* Program 1 only: nothing of program 2's PMT or video. */
        bool clean = olen > 0 && olen % ps == 0;
        for (size_t o = 0; clean && o < olen; o += ps) {
            uint16_t pid = pkt_pid(ordered + o + ts_off(ps));
            if (pid == TS_PMT2_PID || pid == TS_VIDEO2_PID) clean = false;
        }
        MOQ_TEST_CHECK(clean);
    } else {
        MOQ_TEST_CHECK_EQ_U64(olen, len);
        MOQ_TEST_CHECK(olen == len && memcmp(ordered, src, len) == 0);
        MOQ_TEST_CHECK(cc_continuous(ordered, olen, ps));
    }
    if (failures != before) {
        fprintf(stderr, "sender %s %s: %d Objects:", names[kind], vname(ver),
                g_snd.objects);
        for (int i = 0; i < g_snd.objects && i < 256; i++)
            fprintf(stderr, " (%llu,%llu)/%zu",
                    (unsigned long long)g_snd.seen_g[i],
                    (unsigned long long)g_snd.seen_o[i], g_snd.seen_len[i]);
        fprintf(stderr, "\n");
    }
    pthread_mutex_unlock(&g_snd.mu);
    moq_media_sender_track_stats_t st;
    if (t && moq_media_sender_get_track_stats(s, t, &st, sizeof(st)) == MOQ_OK) {
        MOQ_TEST_CHECK(st.objects_written == st.objects_sent);
        MOQ_TEST_CHECK_EQ_U64(st.objects_dropped, 0);
        MOQ_TEST_CHECK_EQ_U64((uint64_t)g_snd.objects, st.objects_sent);
    }
    moq_media_sender_destroy(s);
    stop_server(srv);
    pthread_mutex_destroy(&g_snd.mu);
    if (failures == before)
        printf("PASS: ts_loopback_sender_%s_%s\n", names[kind], vname(ver));
}

/* -- Receiver half ---------------------------------------------------------- */

#define RCV_GROUPS     6
#define RCV_EARLY      2   /* Groups published before anyone subscribes */
#define RCV_MAX_OBJS   64

static const char k_catalog[] =
    "{\"version\":1,\"tracks\":["
    "{\"name\":\"prog\",\"packaging\":\"mpeg2ts\",\"isLive\":true,"
    "\"bitrate\":4000000,\"mpeg2tsMode\":\"unmodified-program\","
    "\"mpeg2tsPacketSize\":188,\"mpeg2tsProgramNumber\":1}]}";

typedef struct rcv_obj {
    uint64_t group, object;
    size_t   off, len;      /* in rcv_srv_t.data */
} rcv_obj_t;

typedef struct rcv_srv {
    moq_publisher_t *pub;
    moq_pub_track_t *cat, *ts;
    uint8_t          data[MAX_STREAM];
    size_t           len;
    rcv_obj_t        objs[RCV_MAX_OBJS];
    int              nobjs;
    int              published;    /* Objects written so far */
    atomic_bool      ended;        /* END_OF_TRACK written */
    atomic_bool      subscribed;
    int              first_live;   /* first Object index written live */
    bool             failed;
} rcv_srv_t;

static rcv_srv_t g_rcv;

static void rcv_emit(void *ctx, const moq_mpeg2ts_emitted_object_t *o)
{
    rcv_srv_t *st = (rcv_srv_t *)ctx;
    if (st->nobjs == RCV_MAX_OBJS || st->len + o->payload.len > sizeof(st->data)) {
        st->failed = true;
        return;
    }
    rcv_obj_t *r = &st->objs[st->nobjs++];
    r->group = o->group;
    r->object = o->object;
    r->off = st->len;
    r->len = o->payload.len;
    memcpy(st->data + st->len, o->payload.data, o->payload.len);
    st->len += o->payload.len;
}

/* Packetize RCV_GROUPS GOPs into the Objects the server will publish. */
static bool rcv_prepare(rcv_srv_t *st, uint8_t *psi, size_t *psi_len)
{
    ts_gen_t g;
    ts_gen_init(&g, 188, 1, false);
    static uint8_t src[MAX_STREAM];
    size_t len = 0;
    for (int i = 0; i < RCV_GROUPS; i++) len += ts_gen_gop(&g, src + len, 6);
    memcpy(psi, src, 2 * 188);
    *psi_len = 2 * 188;
    static uint8_t obj_buf[4 * 188], lead_buf[3 * 188];
    moq_mpeg2ts_packetizer_cfg_t pc;
    moq_mpeg2ts_packetizer_cfg_init(&pc);
    pc.packet_size = 188;
    pc.object_packets = 4;
    pc.group_at_random_access = true;
    pc.wait_for_random_access = true;
    pc.object_buf = obj_buf;
    pc.object_cap = sizeof(obj_buf);
    pc.lead_buf = lead_buf;
    pc.lead_cap = sizeof(lead_buf);
    pc.emit = rcv_emit;
    pc.ctx = st;
    static moq_mpeg2ts_packetizer_t pz;
    if (moq_mpeg2ts_packetizer_init(&pz, sizeof(pz), &pc) != MOQ_OK) return false;
    for (size_t o = 0; o < len; o += 188)
        if (moq_mpeg2ts_packetizer_push(&pz, (moq_bytes_t){ src + o, 188 }) !=
            MOQ_OK)
            return false;
    return moq_mpeg2ts_packetizer_flush(&pz) == MOQ_OK && !st->failed;
}

static int rcv_pump(moq_pq_threaded_t *t, moq_pq_threaded_lane_t *lane,
                    uint64_t now_us, void *ctx)
{
    (void)lane;
    rcv_srv_t *st = (rcv_srv_t *)ctx;
    moq_session_t *s = moq_pq_threaded_session(t);
    if (!s || st->failed) return 0;
    if (!st->pub) {
        moq_pub_cfg_t pc;
        moq_pub_cfg_init_sized(&pc, sizeof(pc));
        pc.accept_mode = MOQ_PUB_ACCEPT_ALL;
        if (moq_pub_create(s, moq_alloc_default(), &pc, &st->pub) != MOQ_OK) {
            st->failed = true;
            return -1;
        }
        moq_pub_track_cfg_t tc;
        moq_pub_track_cfg_init_sized(&tc, sizeof(tc));
        tc.track_namespace = (moq_namespace_t){ g_ns, 2 };
        tc.track_name = MOQ_BYTES_LITERAL(MOQ_MSF_CATALOG_TRACK_NAME);
        if (moq_pub_add_track(st->pub, &tc, now_us, &st->cat) != MOQ_OK) {
            st->failed = true;
            return -1;
        }
        tc.track_name = (moq_bytes_t){ (const uint8_t *)"prog", 4 };
        if (moq_pub_add_track(st->pub, &tc, now_us, &st->ts) != MOQ_OK) {
            st->failed = true;
            return -1;
        }
        moq_rcbuf_t *json = NULL;
        moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)k_catalog,
                         strlen(k_catalog), &json);
        moq_pub_retained_object_t ro = { .object_id = 0, .payload = json };
        moq_pub_retained_group_cfg_t rg;
        moq_pub_retained_group_cfg_init(&rg);
        rg.group_id = 0;
        rg.objects = &ro;
        rg.object_count = 1;
        moq_result_t rc = moq_pub_set_retained_group(st->pub, st->cat, &rg);
        moq_rcbuf_decref(json);
        if (rc != MOQ_OK) { st->failed = true; return -1; }
    }
    (void)moq_pub_tick(st->pub, now_us);
    bool sub = moq_pub_has_subscriber(st->pub, st->ts);
    if (sub && !atomic_load(&st->subscribed)) {
        atomic_store(&st->subscribed, true);
        st->first_live = st->published;
    }
    /* The early Groups go out with no subscriber, as a live track's would;
     * the rest once the receiver has joined. */
    while (st->published < st->nobjs) {
        const rcv_obj_t *o = &st->objs[st->published];
        if (!sub && o->group >= RCV_EARLY) break;
        moq_rcbuf_t *b = NULL;
        moq_rcbuf_create(moq_alloc_default(), st->data + o->off, o->len, &b);
        moq_pub_object_cfg_t oc;
        moq_pub_object_cfg_init_sized(&oc, sizeof(oc));
        oc.group_id = o->group;
        oc.object_id = o->object;
        oc.payload = b;
        oc.end_of_group = true;
        moq_result_t rc = moq_pub_write_object_ex(st->pub, st->ts, &oc, now_us);
        moq_rcbuf_decref(b);
        if (rc == MOQ_ERR_WOULD_BLOCK || rc == MOQ_ERR_WRONG_STATE) break;
        if (rc != MOQ_OK) { st->failed = true; return -1; }
        st->published++;
    }
    if (st->published == st->nobjs && sub && !atomic_load(&st->ended)) {
        moq_result_t rc = moq_pub_end_track(st->pub, st->ts, now_us);
        if (rc == MOQ_OK) atomic_store(&st->ended, true);
        else if (rc != MOQ_ERR_WOULD_BLOCK) { st->failed = true; return -1; }
    }
    (void)moq_pub_tick(st->pub, now_us);
    return 0;
}

static void test_receiver_loopback(const char *cert, const char *key,
                                   moq_version_t ver)
{
    int before = failures;
    memset(&g_rcv, 0, sizeof(g_rcv));
    uint8_t psi[2 * 188];
    size_t psi_len = 0;
    if (!rcv_prepare(&g_rcv, psi, &psi_len)) { failures++; return; }
    int port = 0;
    moq_pq_threaded_t *srv = start_server(cert, key, rcv_pump, &g_rcv, &port);
    if (!srv) { failures++; return; }

    char url[64];
    moq_endpoint_cfg_t ec = ep_cfg(url, sizeof(url), port, &ver);
    moq_media_receiver_cfg_t rc;
    moq_media_receiver_cfg_init_live_sized(&rc, sizeof(rc));
    rc.namespace_ = (moq_namespace_t){ g_ns, 2 };
    rc.endpoint = &ec;
    rc.auto_subscribe = true;
    rc.overflow.policy = MOQ_MEDIA_OVERFLOW_FLOW_CONTROL;
    rc.overflow.max_objects = 256;
    moq_media_receiver_t *r = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_media_receiver_create(&rc, &r), (int)MOQ_OK);
    if (!r) { stop_server(srv); failures++; return; }

    static uint8_t out[MAX_STREAM];
    size_t out_len = 0;
    int objects = 0, gaps = 0, ended = 0;
    uint64_t first_group = UINT64_MAX;
    bool first_ok = false, starts_ok = true, packaging_ok = false;
    for (int i = 0; i < 400 && !ended; i++) {
        (void)moq_media_receiver_wait(r, 25000);
        moq_media_track_event_t ev;
        while (moq_media_receiver_poll_track(r, &ev, sizeof(ev)) == MOQ_OK) {
            if (ev.kind == MOQ_MEDIA_TRACK_ADDED && ev.desc)
                packaging_ok = ev.desc->info.packaging ==
                               MOQ_MEDIA_PACKAGING_MPEG2TS;
            if (ev.kind == MOQ_MEDIA_TRACK_ENDED) ended++;
        }
        moq_media_object_t o;
        while (moq_media_receiver_poll_object(r, &o, sizeof(o)) == MOQ_OK) {
            if (o.status == MOQ_OBJECT_NORMAL) {
                if (objects == 0) {
                    first_group = o.group_id;
                    first_ok = o.object_id == 0 && o.keyframe;
                }
                if (o.object_id == 0 &&
                    !group_start_ok(o.payload.data, o.payload.len, 188))
                    starts_ok = false;
                if (!o.has_loss_report || o.gap || o.gap_tail_unknown) gaps++;
                if (out_len + o.payload.len <= sizeof(out)) {
                    memcpy(out + out_len, o.payload.data, o.payload.len);
                    out_len += o.payload.len;
                }
                objects++;
            }
            moq_media_object_cleanup(&o);
        }
    }
    MOQ_TEST_CHECK(!g_rcv.failed);
    MOQ_TEST_CHECK(packaging_ok);
    MOQ_TEST_CHECK_EQ_INT(ended, 1);
    /* The join lands on a Group start at or after the first live one. */
    MOQ_TEST_CHECK(first_ok && first_group >= RCV_EARLY);
    MOQ_TEST_CHECK(starts_ok);
    MOQ_TEST_CHECK_EQ_INT(gaps, 0);
    /* Everything from the joined Group on, unchanged. */
    size_t from = 0;
    int expect = 0;
    for (int i = 0; i < g_rcv.nobjs; i++) {
        if (g_rcv.objs[i].group < first_group) continue;
        if (expect == 0) from = g_rcv.objs[i].off;
        expect++;
    }
    MOQ_TEST_CHECK_EQ_INT(objects, expect);
    MOQ_TEST_CHECK_EQ_U64(out_len, g_rcv.len - from);
    MOQ_TEST_CHECK(out_len == g_rcv.len - from &&
                   memcmp(out, g_rcv.data + from, out_len) == 0);
    MOQ_TEST_CHECK(cc_continuous(out, out_len, 188));
    moq_media_receiver_stats_t rs;
    if (moq_media_receiver_get_stats(r, &rs, sizeof(rs)) == MOQ_OK) {
        MOQ_TEST_CHECK_EQ_U64(rs.gaps_reported, 0);
        MOQ_TEST_CHECK_EQ_U64(rs.gap_tails_unknown, 0);
        MOQ_TEST_CHECK_EQ_U64(rs.objects_dropped, 0);
    }
    moq_media_receiver_destroy(r);
    stop_server(srv);
    if (failures == before)
        printf("PASS: ts_loopback_receiver_%s\n", vname(ver));
}

int main(int argc, char **argv)
{
    const char *cert = NULL, *key = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "--cert") && i + 1 < argc) cert = argv[++i];
        else if (!strcmp(argv[i], "--key") && i + 1 < argc) key = argv[++i];
    }
    if (!cert || !key) {
        fprintf(stderr, "usage: --cert <pem> --key <pem>\n");
        return 1;
    }
    static const moq_version_t vers[2] = { MOQ_VERSION_DRAFT_16,
                                           MOQ_VERSION_DRAFT_18 };
    for (int v = 0; v < 2; v++) {
        test_sender_loopback(cert, key, vers[v], SRC_188);
        test_sender_loopback(cert, key, vers[v], SRC_192);
        test_sender_loopback(cert, key, vers[v], SRC_MPTS);
        test_receiver_loopback(cert, key, vers[v]);
    }
    if (failures) {
        printf("FAIL: %d mpeg2ts loopback failures\n", failures);
        return 1;
    }
    printf("PASS: all mpeg2ts loopback tests\n");
    return 0;
}
