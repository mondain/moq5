/* Only committed bytes and queued identities are conserved on refusal.
 * The unused send-buffer tail and the advancing session borrow epoch are not. */
#include <moq/moq.h>
#include <moq/control_d18.h>
#include <moq/transport_binding.h>
#include "../../core/src/session/session_internal.h"
#include "../../core/src/wire/control_d22_internal.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define IMAGE_ACTIONS 8
#define IMAGE_BYTES 16384

static int failures;
static unsigned rows, balanced;
static const char *row;
static uint8_t uri_bytes[8193];

#define CHECK(label, condition) do { if (!(condition)) { \
    fprintf(stderr, "FAIL %s: %s\n", row, label); failures++; \
} } while (0)
#define REQUIRE(label, condition) do { if (!(condition)) { \
    CHECK(label, false); goto done; \
} } while (0)

typedef struct memory_state {
    int64_t live;
    size_t calls;
} memory_state_t;

static void *count_alloc(size_t n, void *ctx)
{
    memory_state_t *m = ctx;
    m->calls++;
    void *p = malloc(n);
    if (p) m->live++;
    return p;
}

static void count_free(void *p, size_t n, void *ctx)
{
    (void)n;
    memory_state_t *m = ctx;
    if (p) m->live--;
    free(p);
}

static void *count_realloc(void *p, size_t old, size_t n, void *ctx)
{
    if (!p) return count_alloc(n, ctx);
    if (!n) { count_free(p, old, ctx); return NULL; }
    memory_state_t *m = ctx;
    m->calls++;
    return realloc(p, n);
}

typedef struct fixture {
    moq_session_t *s;
    memory_state_t memory;
    moq_alloc_t alloc;
    size_t original_send_cap;
} fixture_t;

static void destroy(fixture_t *f)
{
    if (f->s) {
        f->s->send_cap = f->original_send_cap;
        moq_session_destroy(f->s);
    }
    CHECK("allocator.balance", f->memory.live == 0);
    if (!f->memory.live) balanced++;
}

static void drain(moq_session_t *s)
{
    moq_action_t a;
    moq_event_t e;
    while (moq_session_poll_actions(s, &a, 1)) moq_action_cleanup(&a);
    while (moq_session_poll_events(s, &e, 1)) moq_event_cleanup(&e);
}

static uint32_t accept_uri(void *ctx, uint32_t present,
                           moq_bytes_t authority, moq_bytes_t path)
{
    (void)ctx; (void)present; (void)authority; (void)path;
    return MOQ_URI_ACCEPT;
}

static bool native_create(fixture_t *f, unsigned draft,
                          moq_perspective_t role, uint32_t actions,
                          uint32_t send_bytes)
{
    memset(f, 0, sizeof(*f));
    f->alloc = (moq_alloc_t){.ctx = &f->memory, .alloc = count_alloc,
        .realloc = count_realloc, .free = count_free};
    moq_session_cfg_t cfg;
    moq_session_cfg_init_sized(&cfg, sizeof(cfg), &f->alloc, role);
    cfg.version = draft == 16 ? MOQ_VERSION_DRAFT_16 :
        draft == 18 ? MOQ_VERSION_DRAFT_18 : MOQ_VERSION_DRAFT_22;
    cfg.max_actions = actions; cfg.max_events = 4;
    cfg.send_buffer_size = send_bytes; cfg.output_scratch_size = 64;
    cfg.send_request_capacity = true; cfg.initial_request_capacity = 64;
    cfg.goaway_timeout_us = 123456;
    if (moq_session_create(&cfg, 0, &f->s) != MOQ_OK) return false;
    f->original_send_cap = f->s->send_cap;
    if (draft == 22) {
        moq_transport_binding_t binding;
        moq_transport_binding_init(&binding);
        binding.kind = MOQ_BINDING_RAW_QUIC;
        binding.facts_known = binding.facts_supported = MOQ_BINDING_FACTS_ALL;
        if (role == MOQ_PERSPECTIVE_SERVER) binding.accept_uri = accept_uri;
        else {
            binding.target_present = MOQ_BINDING_TARGET_COMPLETE;
            binding.authority = MOQ_BYTES_LITERAL("example.test");
            binding.path_query = MOQ_BYTES_LITERAL("/");
        }
        if (moq_session_bind_transport(f->s, &binding, sizeof(binding)) != MOQ_OK)
            return false;
    }
    if ((draft != 16 || role == MOQ_PERSPECTIVE_CLIENT) &&
        moq_session_start(f->s, 0) != MOQ_OK) return false;
    drain(f->s);
    uint8_t setup[64];
    moq_buf_writer_t w = {setup, sizeof(setup), 0};
    moq_result_t rc;
    if (draft == 22) rc = moq_d22_encode_setup(&w, NULL, 0);
    else if (draft == 18) rc = moq_d18_encode_setup(&w);
    else {
        const uint8_t value[] = {0x40, 0x40};
        moq_kvp_entry_t cap = {.type = MOQ_SETUP_PARAM_MAX_REQUEST_ID,
            .is_varint = true, .value = value, .value_len = sizeof(value)};
        rc = role == MOQ_PERSPECTIVE_CLIENT ?
            moq_d16_encode_server_setup(&w, &cap, 1) :
            moq_d16_encode_client_setup(&w, &cap, 1);
    }
    if (rc != MOQ_OK ||
        moq_session_on_control_bytes(f->s, setup, w.pos, 1) != MOQ_OK)
        return false;
    moq_event_t e;
    if (moq_session_poll_events(f->s, &e, 1) != 1) return false;
    bool ready = e.kind == MOQ_EVENT_SETUP_COMPLETE &&
        moq_session_state(f->s) == MOQ_SESS_ESTABLISHED &&
        moq_session_version(f->s) == cfg.version;
    moq_event_cleanup(&e);
    drain(f->s);
    return ready && moq_session_tick(f->s, 2) == MOQ_OK;
}

typedef struct action_image {
    moq_action_t action;
    size_t bytes_len;
    uint8_t bytes[IMAGE_BYTES];
} action_image_t;

typedef struct goaway_image {
    bool valid;
    size_t head, tail, send_len;
    bool sent;
    moq_session_state_t state;
    uint64_t deadline;
    action_image_t actions[IMAGE_ACTIONS];
} goaway_image_t;

static goaway_image_t saved;

static bool active_bytes(const moq_action_t *a, const uint8_t **p, size_t *n)
{
    switch (a->kind) {
    case MOQ_ACTION_SEND_CONTROL:
        *p = a->u.send_control.data; *n = a->u.send_control.len; break;
    case MOQ_ACTION_OPEN_BIDI_STREAM:
    case MOQ_ACTION_SEND_BIDI_STREAM:
        *p = a->u.send_bidi_stream.data; *n = a->u.send_bidi_stream.len; break;
    case MOQ_ACTION_OPEN_UNI_CONTROL:
        *p = a->u.open_uni_control.data; *n = a->u.open_uni_control.len; break;
    default: CHECK("inventory.unsupported.kind", false); return false;
    }
    return true;
}

static bool image_take(const moq_session_t *s)
{
    memset(&saved, 0, sizeof(saved));
    saved.head = s->action_head; saved.tail = s->action_tail;
    saved.send_len = s->send_len; saved.sent = s->goaway_sent;
    saved.state = s->state; saved.deadline = s->goaway_deadline_us;
    if (saved.tail - saved.head > IMAGE_ACTIONS) {
        CHECK("inventory.action.limit", false); return false;
    }
    for (size_t i = saved.head; i < saved.tail; ++i) {
        action_image_t *image = &saved.actions[i - saved.head];
        image->action = s->actions[i % s->action_cap];
        const uint8_t *p; size_t n;
        if (!active_bytes(&image->action, &p, &n)) return false;
        if (n > IMAGE_BYTES) { CHECK("inventory.byte.limit", false); return false; }
        image->bytes_len = n;
        if (n) memcpy(image->bytes, p, n);
    }
    saved.valid = true;
    return true;
}

static void image_action_equal(const moq_action_t *a, const action_image_t *old)
{
    CHECK("identity.kind", a->kind == old->action.kind);
    CHECK("identity.detail_size", a->detail_size == old->action.detail_size);
    CHECK("identity.borrow_epoch", a->borrow_epoch == old->action.borrow_epoch);
    if (a->kind != old->action.kind) return;
    const uint8_t *p, *was; size_t n, was_n;
    if (!active_bytes(a, &p, &n) || !active_bytes(&old->action, &was, &was_n)) return;
    CHECK("identity.data", p == was);
    CHECK("identity.len", n == was_n && n == old->bytes_len);
    if (n == old->bytes_len)
        CHECK("inventory.borrowed.bytes", !n || !memcmp(p, old->bytes, n));
    if (a->kind == MOQ_ACTION_OPEN_BIDI_STREAM || a->kind == MOQ_ACTION_SEND_BIDI_STREAM) {
        CHECK("identity.stream_ref", a->u.send_bidi_stream.stream_ref._v ==
            old->action.u.send_bidi_stream.stream_ref._v);
        CHECK("identity.fin", a->u.send_bidi_stream.fin == old->action.u.send_bidi_stream.fin);
    } else if (a->kind == MOQ_ACTION_OPEN_UNI_CONTROL)
        CHECK("identity.stream_ref", a->u.open_uni_control.stream_ref._v ==
            old->action.u.open_uni_control.stream_ref._v);
}

static void image_unchanged(const moq_session_t *s)
{
    CHECK("inventory.valid", saved.valid);
    if (!saved.valid) return;
    CHECK("inventory.head", s->action_head == saved.head);
    CHECK("inventory.tail", s->action_tail == saved.tail);
    CHECK("inventory.send_len", s->send_len == saved.send_len);
    CHECK("inventory.sent", s->goaway_sent == saved.sent);
    CHECK("inventory.state", s->state == saved.state);
    CHECK("inventory.deadline", s->goaway_deadline_us == saved.deadline);
    if (s->action_head == saved.head && s->action_tail == saved.tail)
        for (size_t i = saved.head; i < saved.tail; ++i)
            image_action_equal(&s->actions[i % s->action_cap], &saved.actions[i - saved.head]);
}

static bool public_prefix(fixture_t *f)
{
    moq_bytes_t parts[] = {MOQ_BYTES_LITERAL("prefix"), MOQ_BYTES_LITERAL("ns")};
    moq_publish_namespace_cfg_t cfg; moq_publish_namespace_cfg_init(&cfg);
    cfg.track_namespace = (moq_namespace_t){parts, 2};
    moq_announcement_t h;
    return moq_session_publish_namespace(f->s, &cfg, 3, &h) == MOQ_OK;
}

static bool fill_queue(moq_session_t *s)
{
    const uint8_t filler[] = {0xa1, 0xb2};
    for (size_t i = 0; i < s->action_cap; ++i) {
        if (action_queue_full(s)) return true;
        if (queue_send_control(s, filler, sizeof(filler)) != MOQ_OK) return false;
    }
    return action_queue_full(s);
}

/* Independent D16 section 9.4, D18 section 10.4 and D22 section 9.2 images.
 * Only URI lengths up to 8192 are used: QUIC two-byte prefix is 01, vi64 is 10.
 * D18 alone appends a Request ID. Timeout 123 fits one vi64 byte. */
static size_t expected_wire(unsigned draft, size_t uri_len,
                             uint8_t header[5], size_t *header_len)
{
    bool two = uri_len >= (draft == 16 ? 64u : 128u);
    size_t body = (two ? 2u : 1u) + uri_len + (draft == 16 ? 0u : 1u) +
        (draft == 18 ? 1u : 0u);
    header[0] = 0x10;
    header[1] = (uint8_t)(body >> 8); header[2] = (uint8_t)body;
    header[3] = two ? (uint8_t)((draft == 16 ? 0x40 : 0x80) | (uri_len >> 8)) :
        (uint8_t)uri_len;
    if (two) header[4] = (uint8_t)uri_len;
    *header_len = two ? 5 : 4;
    return body + 3;
}

static void wire_check(const moq_action_t *a, unsigned draft, size_t uri_len, bool client)
{
    REQUIRE("wire.kind", a->kind == MOQ_ACTION_SEND_CONTROL);
    CHECK("wire.detail_size", a->detail_size == sizeof(moq_send_control_action_t));
    uint8_t header[5]; size_t h;
    size_t n = expected_wire(draft, uri_len, header, &h);
    REQUIRE("wire.exact.len", a->u.send_control.len == n);
    const uint8_t *p = a->u.send_control.data;
    CHECK("wire.header", !memcmp(p, header, h));
    CHECK("wire.uri", !uri_len || !memcmp(p + h, uri_bytes, uri_len));
    size_t end = h + uri_len;
    if (draft != 16) CHECK("wire.timeout", p[end++] == 123);
    if (draft == 18) CHECK("wire.request.id", p[end++] == (client ? 1 : 0));
    CHECK("wire.exact.end", end == n);
done:;
}

static void success_case(unsigned draft, size_t uri_len, bool client, bool exact)
{
    char label[96];
    snprintf(label, sizeof(label), "d%u.success.uri%zu.client%u.exact%u",
        draft, uri_len, client ? 1u : 0u, exact ? 1u : 0u);
    row = label; rows++;
    fixture_t f = {0};
    REQUIRE("create.actual.profile", native_create(&f, draft,
        client ? MOQ_PERSPECTIVE_CLIENT : MOQ_PERSPECTIVE_SERVER, 4, 16384));
    REQUIRE("prefix", public_prefix(&f));
    REQUIRE("prefix.one", f.s->action_tail - f.s->action_head == 1);
    REQUIRE("image", image_take(f.s));
    if (exact) {
        uint8_t header[5]; size_t h;
        f.s->send_cap = saved.send_len + expected_wire(draft, uri_len, header, &h);
    }
    size_t calls = f.memory.calls;
    REQUIRE("goaway", moq_session_goaway(f.s, uri_len ? uri_bytes : NULL, uri_len, 1000) == MOQ_OK);
    CHECK("no.allocation", f.memory.calls == calls);
    CHECK("tiny.output.scratch", f.s->output_scratch_cap == 64);
    CHECK("latch.state.deadline", f.s->goaway_sent && f.s->state == MOQ_SESS_DRAINING &&
        f.s->goaway_deadline_us == 124456);
    REQUIRE("queue.append", f.s->action_head == saved.head && f.s->action_tail == saved.tail + 1);
    image_action_equal(&f.s->actions[saved.head % f.s->action_cap], &saved.actions[0]);
    const moq_action_t *a = &f.s->actions[saved.tail % f.s->action_cap];
    CHECK("append.address", a->u.send_control.data == f.s->send_buf + saved.send_len);
    CHECK("append.conservation", f.s->send_len == saved.send_len + a->u.send_control.len);
    CHECK("append.borrow.epoch", a->borrow_epoch == f.s->borrow_epoch);
    if (exact) CHECK("capacity.exact", f.s->send_len == f.s->send_cap);
    wire_check(a, draft, uri_len, client);
    REQUIRE("repeat.image", image_take(f.s));
    CHECK("repeat.precedence", moq_session_goaway(f.s, NULL, 8193, 1001) == MOQ_ERR_WRONG_STATE);
    CHECK("repeat.no.allocation", f.memory.calls == calls);
    image_unchanged(f.s);
done:
    destroy(&f);
}

static void refusal_case(unsigned draft, unsigned which)
{
    char label[96];
    snprintf(label, sizeof(label), "d%u.refusal.class%u", draft, which);
    row = label; rows++;
    fixture_t f = {0};
    bool client = which == 2;
    bool full = which == 3 || (which >= 6 && which <= 8) || which >= 11;
    REQUIRE("create.actual.profile", native_create(&f, draft,
        client ? MOQ_PERSPECTIVE_CLIENT : MOQ_PERSPECTIVE_SERVER, 4, 16384));
    REQUIRE("prefix", public_prefix(&f));
    if (full) REQUIRE("full", fill_queue(f.s));
    const uint8_t *uri = uri_bytes; size_t len = 64;
    moq_result_t expected = MOQ_ERR_WOULD_BLOCK;
    if (which == 0 || which == 7) { uri = NULL; len = 1; expected = MOQ_ERR_INVAL; }
    if (which == 1 || which == 8) { len = 8193; expected = MOQ_ERR_INVAL; }
    if (which == 2) { len = 1; expected = MOQ_ERR_INVAL; }
    if (which == 4) { len = 8192; expected = MOQ_ERR_BUFFER; f.s->send_cap = f.s->send_len + 3; }
    if (which == 9 || which == 11) { uri = NULL; len = 0; }
    if (which == 10 || which == 12) len = 8192;
    if (which == 5 || which == 6 || which >= 9) {
        uint8_t header[5]; size_t h;
        size_t n = expected_wire(draft, len, header, &h);
        f.s->send_cap = f.s->send_len + n - 1;
        CHECK("short.by.one", n <= f.s->send_cap && n > f.s->send_cap - f.s->send_len);
        if (!full) expected = MOQ_ERR_BUFFER;
    }
    REQUIRE("image", image_take(f.s));
    REQUIRE("inventory.nonempty", saved.tail > saved.head && saved.send_len > 0);
    size_t calls = f.memory.calls;
    CHECK("result.precedence", moq_session_goaway(f.s, uri, len, 1000) == expected);
    CHECK("no.allocation", f.memory.calls == calls);
    image_unchanged(f.s);
done:
    destroy(&f);
}

static int oracle_control(const char *mode)
{
    fixture_t f = {0};
    row = mode;
    bool actions = !strcmp(mode, "--oracle-action-limit");
    bool bytes = !strcmp(mode, "--oracle-byte-limit");
    if (!native_create(&f, 22, MOQ_PERSPECTIVE_SERVER,
        actions ? IMAGE_ACTIONS + 1 : 4, 32768)) { CHECK("create", false); goto done; }
    if (actions) {
        if (!fill_queue(f.s)) { CHECK("fill", false); goto done; }
    } else if (bytes) {
        static uint8_t big[IMAGE_BYTES + 1];
        if (queue_send_control(f.s, big, sizeof(big)) != MOQ_OK) { CHECK("queue", false); goto done; }
    } else if (!public_prefix(&f)) { CHECK("prefix", false); goto done; }
    if (!image_take(f.s)) {
        image_unchanged(f.s); /* A truncated image must remain unusable. */
        goto done;
    }
    moq_action_t *a = &f.s->actions[saved.head % f.s->action_cap];
    if (!strcmp(mode, "--oracle-byte")) {
        const uint8_t *p; size_t n;
        if (!active_bytes(a, &p, &n) || !n) { CHECK("bytes", false); goto done; }
        uint8_t *live = (uint8_t *)p;
        live[0] ^= 1;
        image_unchanged(f.s);
        live[0] ^= 1;
    } else {
        a->borrow_epoch ^= 1;
        image_unchanged(f.s);
        a->borrow_epoch ^= 1;
    }
done:
    destroy(&f);
    printf("ORACLE_FAILURES=%d BALANCED=%u\n", failures, balanced);
    return failures ? 1 : 0;
}

int main(int argc, char **argv)
{
    for (size_t i = 0; i < sizeof(uri_bytes); ++i) uri_bytes[i] = (uint8_t)('a' + i % 26);
    if (argc == 2 && (!strcmp(argv[1], "--oracle-byte") ||
        !strcmp(argv[1], "--oracle-identity") || !strcmp(argv[1], "--oracle-action-limit") ||
        !strcmp(argv[1], "--oracle-byte-limit"))) return oracle_control(argv[1]);
    if (argc != 1) return 2;
    const unsigned drafts[] = {16, 18, 22};
    const size_t lengths[] = {0, 1, 63, 64, 127, 128, 8192};
    for (size_t d = 0; d < sizeof(drafts) / sizeof(drafts[0]); ++d) {
        unsigned draft = drafts[d];
        success_case(draft, 0, true, true);
        for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i)
            success_case(draft, lengths[i], false, false);
        success_case(draft, 1, false, true);
        success_case(draft, 8192, false, true);
        for (unsigned which = 0; which < 13; ++which) refusal_case(draft, which);
    }
    row = "null.session";
    CHECK("result", moq_session_goaway(NULL, uri_bytes, 1, 1) == MOQ_ERR_INVAL);
    printf("GOAWAY_ROWS=%u FAILURES=%d BALANCED=%u\n", rows, failures, balanced);
    return failures ? 1 : 0;
}
