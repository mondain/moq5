/*
 * MSF catalog support for the "mpeg2ts" packaging (draft-gregoire-moq-msfts):
 * every catalog example of the draft, parse leniency, the subscriber's track
 * check, strict encode (independent, delta and initialization data), delta
 * updates, and the track's extension list that carries the mpeg2ts record.
 */

#include <moq/msf.h>
#include <moq/rcbuf.h>

#include <stddef.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int failures = 0;

#define CHECK(expr) do { \
    if (!(expr)) { \
        fprintf(stderr, "FAIL: %s:%d: %s\n", __FILE__, __LINE__, #expr); \
        failures++; \
    } \
} while (0)

/* -- Counting allocator: every parse is cleaned up to a zero balance. ---- */

typedef struct { int64_t balance; } count_ctx_t;

static void *c_alloc(size_t n, void *ctx)
{
    ((count_ctx_t *)ctx)->balance += (int64_t)n;
    return malloc(n ? n : 1);
}

static void c_free(void *p, size_t n, void *ctx)
{
    ((count_ctx_t *)ctx)->balance -= (int64_t)n;
    free(p);
}

static void *c_realloc(void *p, size_t old_n, size_t new_n, void *ctx)
{
    ((count_ctx_t *)ctx)->balance += (int64_t)new_n - (int64_t)old_n;
    return realloc(p, new_n ? new_n : 1);
}

static count_ctx_t g_count;
static moq_alloc_t g_alloc = {
    .ctx = &g_count, .alloc = c_alloc, .realloc = c_realloc, .free = c_free,
};

static moq_bytes_t lit(const char *s)
{
    return (moq_bytes_t){ (const uint8_t *)s, strlen(s) };
}

static bool bv_eq(moq_bytes_t a, const char *s)
{
    size_t n = strlen(s);
    return a.len == n && (n == 0 || memcmp(a.data, s, n) == 0);
}

/* Bounded substring search: encoder output is not NUL-terminated. */
static bool buf_contains(const moq_rcbuf_t *b, const char *needle)
{
    size_t n = strlen(needle), len = moq_rcbuf_len(b);
    const uint8_t *d = moq_rcbuf_data(b);
    for (size_t i = 0; n <= len && i + n <= len; i++)
        if (memcmp(d + i, needle, n) == 0) return true;
    return false;
}

static char *read_fixture(const char *name, size_t *out_len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", MOQ_MSF_FIXTURE_DIR, name);
    FILE *fp = fopen(path, "rb");
    if (!fp) return NULL;
    fseek(fp, 0, SEEK_END);
    long sz = ftell(fp);
    fseek(fp, 0, SEEK_SET);
    if (sz < 0) { fclose(fp); return NULL; }
    char *buf = malloc((size_t)sz + 1);
    size_t rd = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    buf[rd] = '\0';
    *out_len = rd;
    return buf;
}

static moq_result_t parse(const char *json, moq_msf_catalog_t *cat)
{
    return moq_msf_catalog_parse(&g_alloc, lit(json), cat);
}

/* The mpeg2ts fields a parse recorded for track t (all absent when the
 * track carries no record). */
static moq_msf_mpeg2ts_t ts_of(const moq_msf_track_t *t)
{
    moq_msf_mpeg2ts_t m;
    moq_msf_mpeg2ts_init(&m);
    const moq_msf_mpeg2ts_t *r = moq_msf_track_mpeg2ts(t);
    if (r) m = *r;
    return m;
}

static moq_msf_mpeg2ts_error_t check_reason(const moq_msf_track_t *t)
{
    moq_msf_mpeg2ts_error_t r = MOQ_MSF_MPEG2TS_OK;
    moq_result_t rc = moq_msf_track_mpeg2ts_check(t, &r);
    if (rc == MOQ_OK) return MOQ_MSF_MPEG2TS_OK;
    return r;
}

/* Field-by-field equality of the mpeg2ts members. */
static bool mpeg2ts_eq(const moq_msf_mpeg2ts_t *a, const moq_msf_mpeg2ts_t *b)
{
    if (a->has_mode != b->has_mode || a->mode != b->mode) return false;
    if (a->has_packet_size != b->has_packet_size ||
        a->packet_size != b->packet_size) return false;
    if (a->has_es_pid != b->has_es_pid || a->es_pid != b->es_pid) return false;
    if (a->has_program_number != b->has_program_number ||
        a->program_number != b->program_number) return false;
    if (a->has_pcr_pid != b->has_pcr_pid || a->pcr_pid != b->pcr_pid) return false;
    if (a->has_mux_rate != b->has_mux_rate || a->mux_rate != b->mux_rate) return false;
    if (a->has_si_pids != b->has_si_pids || a->si_pid_count != b->si_pid_count)
        return false;
    for (size_t i = 0; i < a->si_pid_count; i++)
        if (a->si_pids[i] != b->si_pids[i]) return false;
    if (a->has_random_access != b->has_random_access ||
        a->random_access != b->random_access) return false;
    if (a->has_timestamp_mode != b->has_timestamp_mode ||
        a->timestamp_mode != b->timestamp_mode) return false;
    if (a->has_scte35_pid != b->has_scte35_pid ||
        a->scte35_pid != b->scte35_pid) return false;
    return a->malformed == b->malformed;
}

/* -- The draft's catalog examples ---------------------------------------- */

static const char *const EXAMPLES[] = {
    "mpeg2ts_live_ts.json",
    "mpeg2ts_live_m2ts.json",
    "mpeg2ts_per_program.json",
    "mpeg2ts_init_data.json",
    "mpeg2ts_unmodified_multiplex.json",
    "mpeg2ts_abr.json",
    "mpeg2ts_es_packets.json",
};

/* Parse, check every track, and round-trip: encode is a fixpoint after one
 * pass (byte-exact), and the re-parsed tracks equal the originals. */
/* The record's sized initializer: every prefix 0 .. sizeof + 16. Below
 * struct_size nothing is written; otherwise exactly min(n, sizeof) octets
 * are zeroed and struct_size is stamped with that count; nothing past it is
 * touched. The macro form passes the caller's sizeof. */
static void test_record_init_sized(void)
{
    union { uint8_t b[sizeof(moq_msf_mpeg2ts_t) + 16]; uint64_t a; } u;
    for (size_t n = 0; n <= sizeof(u.b); n++) {
        memset(u.b, 0xA5, sizeof(u.b));
        moq_msf_mpeg2ts_init_sized((moq_msf_mpeg2ts_t *)u.b, n);
        size_t w = n < sizeof(uint32_t) ? 0
                 : (n < sizeof(moq_msf_mpeg2ts_t) ? n
                                                  : sizeof(moq_msf_mpeg2ts_t));
        bool ok = true;
        for (size_t i = 0; i < sizeof(u.b); i++) {
            uint8_t want = i < w ? 0 : 0xA5;
            if (i < sizeof(uint32_t) && w) {
                uint32_t stamp = (uint32_t)w;
                want = ((const uint8_t *)&stamp)[i];
            }
            if (u.b[i] != want) ok = false;
        }
        if (!ok) {
            fprintf(stderr, "FAIL: record init prefix %zu\n", n);
            failures++;
        }
    }
    moq_msf_mpeg2ts_t m;
    memset(&m, 0xA5, sizeof(m));
    moq_msf_mpeg2ts_init(&m);
    CHECK(m.struct_size == sizeof(m) && !m.has_mode && m.malformed == 0);
    CHECK(MOQ_MSF_MPEG2TS_V0_SIZE ==
          offsetof(moq_msf_mpeg2ts_t, malformed) + sizeof(uint32_t));
    moq_msf_mpeg2ts_init_sized(NULL, sizeof(m));
}

static void test_examples_round_trip(void)
{
    for (size_t k = 0; k < sizeof(EXAMPLES) / sizeof(EXAMPLES[0]); k++) {
        size_t len = 0;
        char *json = read_fixture(EXAMPLES[k], &len);
        CHECK(json != NULL);
        if (!json) continue;
        moq_msf_catalog_t a;
        CHECK(moq_msf_catalog_parse(&g_alloc, (moq_bytes_t){ (uint8_t *)json, len },
                                    &a) == MOQ_OK);
        CHECK(a.track_count > 0);
        for (size_t i = 0; i < a.track_count; i++) {
            CHECK(check_reason(&a.tracks[i]) == MOQ_MSF_MPEG2TS_OK);
            if (check_reason(&a.tracks[i]) != MOQ_MSF_MPEG2TS_OK)
                fprintf(stderr, "  %s track %zu\n", EXAMPLES[k], i);
        }

        /* The example that shortens its Base64 with "..." is not authorable
         * (Use of MSF Initialization Data); the receive path re-encodes it. */
        bool abbreviated = strcmp(EXAMPLES[k], "mpeg2ts_init_data.json") == 0;
        moq_rcbuf_t *e1 = NULL, *e2 = NULL;
        if (abbreviated) {
            CHECK(moq_msf_catalog_encode(&g_alloc, &a, &e1) == MOQ_ERR_INVAL);
            moq_msf_catalog_cleanup(&g_alloc, &a);
            free(json);
            continue;
        }
        CHECK(moq_msf_catalog_encode(&g_alloc, &a, &e1) == MOQ_OK);
        moq_msf_catalog_t b;
        CHECK(moq_msf_catalog_parse(&g_alloc,
              (moq_bytes_t){ moq_rcbuf_data(e1), moq_rcbuf_len(e1) }, &b) == MOQ_OK);
        CHECK(b.track_count == a.track_count);
        for (size_t i = 0; i < a.track_count && i < b.track_count; i++) {
            moq_msf_mpeg2ts_t ma = ts_of(&a.tracks[i]);
            moq_msf_mpeg2ts_t mb = ts_of(&b.tracks[i]);
            CHECK(mpeg2ts_eq(&ma, &mb));
        }
        CHECK(moq_msf_catalog_encode(&g_alloc, &b, &e2) == MOQ_OK);
        CHECK(moq_rcbuf_len(e1) == moq_rcbuf_len(e2) &&
              memcmp(moq_rcbuf_data(e1), moq_rcbuf_data(e2), moq_rcbuf_len(e1)) == 0);
        moq_rcbuf_decref(e1);
        moq_rcbuf_decref(e2);
        moq_msf_catalog_cleanup(&g_alloc, &b);
        moq_msf_catalog_cleanup(&g_alloc, &a);
        free(json);
    }
    CHECK(g_count.balance == 0);
}

/* Spot values from each example. */
static void test_examples_values(void)
{
    size_t len;
    moq_msf_catalog_t c;

    char *j = read_fixture("mpeg2ts_live_ts.json", &len);
    CHECK(moq_msf_catalog_parse(&g_alloc, (moq_bytes_t){ (uint8_t *)j, len }, &c) == MOQ_OK);
    moq_msf_mpeg2ts_t mv = ts_of(&c.tracks[0]);
    const moq_msf_mpeg2ts_t *m = &mv;
    CHECK(m->has_mode && m->mode == MOQ_MSF_MPEG2TS_MODE_UNMODIFIED_PROGRAM);
    CHECK(bv_eq(m->mode_name, "unmodified-program"));
    CHECK(m->has_packet_size && m->packet_size == 188);
    CHECK(m->has_program_number && m->program_number == 1);
    CHECK(m->has_pcr_pid && m->pcr_pid == 257);
    CHECK(m->has_random_access && m->random_access);
    CHECK(!m->has_es_pid && !m->has_mux_rate && !m->has_timestamp_mode);
    CHECK(m->malformed == 0);
    moq_msf_catalog_cleanup(&g_alloc, &c);
    free(j);

    j = read_fixture("mpeg2ts_live_m2ts.json", &len);
    CHECK(moq_msf_catalog_parse(&g_alloc, (moq_bytes_t){ (uint8_t *)j, len }, &c) == MOQ_OK);
    mv = ts_of(&c.tracks[0]);
    CHECK(m->packet_size == 192);
    CHECK(m->has_timestamp_mode &&
          m->timestamp_mode == MOQ_MSF_MPEG2TS_TIMESTAMP_ARRIVAL_TIME);
    moq_msf_catalog_cleanup(&g_alloc, &c);
    free(j);

    j = read_fixture("mpeg2ts_per_program.json", &len);
    CHECK(moq_msf_catalog_parse(&g_alloc, (moq_bytes_t){ (uint8_t *)j, len }, &c) == MOQ_OK);
    CHECK(c.track_count == 2);
    mv = ts_of(&c.tracks[1]);
    CHECK(mv.mode == MOQ_MSF_MPEG2TS_MODE_PER_PROGRAM);
    CHECK(mv.program_number == 2);
    CHECK(mv.mux_rate == 4500000);
    moq_msf_catalog_cleanup(&g_alloc, &c);
    free(j);

    j = read_fixture("mpeg2ts_unmodified_multiplex.json", &len);
    CHECK(moq_msf_catalog_parse(&g_alloc, (moq_bytes_t){ (uint8_t *)j, len }, &c) == MOQ_OK);
    mv = ts_of(&c.tracks[0]);
    CHECK(mv.mode == MOQ_MSF_MPEG2TS_MODE_UNMODIFIED_MULTIPLEX);
    CHECK(!mv.has_program_number);
    moq_msf_catalog_cleanup(&g_alloc, &c);
    free(j);

    j = read_fixture("mpeg2ts_es_packets.json", &len);
    CHECK(moq_msf_catalog_parse(&g_alloc, (moq_bytes_t){ (uint8_t *)j, len }, &c) == MOQ_OK);
    CHECK(c.track_count == 6);
    for (size_t i = 0; i < c.track_count; i++) {
        mv = ts_of(&c.tracks[i]);
        CHECK(mv.mode == MOQ_MSF_MPEG2TS_MODE_ES_PACKETS);
        CHECK(mv.has_es_pid);
    }
    CHECK(ts_of(&c.tracks[4]).es_pid == 0);    /* role "pat" */
    CHECK(ts_of(&c.tracks[5]).es_pid == 256);  /* role "pmt" */
    CHECK(ts_of(&c.tracks[3]).es_pid == 18);   /* role "eit" */
    moq_msf_catalog_cleanup(&g_alloc, &c);
    free(j);
    CHECK(g_count.balance == 0);
}

/* A deterministic encode, pinned byte-exact. The fixture's version is the
 * draft name, which the parser reports and the encoder keeps (MSF-01
 * section 5.1.1's "draft-XX" convention); the numeral form encodes "1". */
static void test_golden_encode(void)
{
    size_t len;
    char *j = read_fixture("mpeg2ts_live_m2ts.json", &len);
    static const char *const version[2] = { "\"draft-01\"", "\"1\"" };
    for (int round = 0; round < 2; round++) {
        moq_msf_catalog_t c;
        CHECK(moq_msf_catalog_parse(&g_alloc, (moq_bytes_t){ (uint8_t *)j, len }, &c) == MOQ_OK);
        CHECK(c.version_form == MOQ_MSF_VERSION_FORM_DRAFT_NAME);
        if (round) c.version_form = MOQ_MSF_VERSION_FORM_NUMERAL;
        moq_rcbuf_t *out = NULL;
        CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_OK);
        char want[1024];
        snprintf(want, sizeof(want),
            "{\"version\":%s,\"generatedAt\":1746104606044,\"tracks\":[{"
            "\"name\":\"program-1-mpeg2ts\",\"packaging\":\"mpeg2ts\","
            "\"isLive\":true,\"namespace\":\"contribution.example.net/feed/a\","
            "\"role\":\"video\",\"bitrate\":12000000,\"targetLatency\":500,"
            "\"mimeType\":\"video/mp2t\",\"mpeg2tsMode\":\"unmodified-program\","
            "\"mpeg2tsPacketSize\":192,\"mpeg2tsProgramNumber\":1,"
            "\"mpeg2tsRandomAccess\":true,"
            "\"mpeg2tsTimestampMode\":\"arrival-time\"}]}", version[round]);
        size_t wl = strlen(want);
        CHECK(out && moq_rcbuf_len(out) == wl &&
              memcmp(moq_rcbuf_data(out), want, wl) == 0);
        if (out && (moq_rcbuf_len(out) != wl ||
                    memcmp(moq_rcbuf_data(out), want, wl) != 0))
            fprintf(stderr, "  got: %.*s\n", (int)moq_rcbuf_len(out),
                    (const char *)moq_rcbuf_data(out));
        moq_rcbuf_decref(out);
        moq_msf_catalog_cleanup(&g_alloc, &c);
    }
    free(j);
}

/* -- Negative cases: one per catalog MUST ------------------------------- */

/* A one-track catalog whose mpeg2ts keys are `fields` (a JSON fragment
 * without a leading comma). */
static char *one_track(const char *fields, const char *extra)
{
    static char buf[2048];
    snprintf(buf, sizeof(buf),
             "{\"version\":\"1\",\"tracks\":[{\"name\":\"t\","
             "\"packaging\":\"mpeg2ts\",\"isLive\":true%s%s%s}]}",
             fields[0] ? "," : "", fields, extra ? extra : "");
    return buf;
}

typedef struct neg_case {
    const char             *fields;
    moq_msf_mpeg2ts_error_t want;
} neg_case_t;

static void test_track_check_negatives(void)
{
    static const neg_case_t cases[] = {
        /* Mode: required, one of four names, MUST reject unrecognized. */
        { "\"mpeg2tsPacketSize\":188", MOQ_MSF_MPEG2TS_ERR_MODE_MISSING },
        { "\"mpeg2tsMode\":\"es-units\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsEsPid\":258", MOQ_MSF_MPEG2TS_ERR_MODE_UNRECOGNIZED },
        { "\"mpeg2tsMode\":\"media-frames\",\"mpeg2tsPacketSize\":188",
          MOQ_MSF_MPEG2TS_ERR_MODE_UNRECOGNIZED },
        { "\"mpeg2tsMode\":\"Unmodified-Program\",\"mpeg2tsPacketSize\":188",
          MOQ_MSF_MPEG2TS_ERR_MODE_UNRECOGNIZED },
        { "\"mpeg2tsMode\":1,\"mpeg2tsPacketSize\":188",
          MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        /* Packet Size: required, 188 or 192. */
        { "\"mpeg2tsMode\":\"unmodified-program\"",
          MOQ_MSF_MPEG2TS_ERR_PACKET_SIZE_MISSING },
        { "\"mpeg2tsMode\":\"unmodified-program\",\"mpeg2tsPacketSize\":204",
          MOQ_MSF_MPEG2TS_ERR_PACKET_SIZE },
        { "\"mpeg2tsMode\":\"unmodified-program\",\"mpeg2tsPacketSize\":0",
          MOQ_MSF_MPEG2TS_ERR_PACKET_SIZE },
        { "\"mpeg2tsMode\":\"unmodified-program\",\"mpeg2tsPacketSize\":\"188\"",
          MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        { "\"mpeg2tsMode\":\"unmodified-program\",\"mpeg2tsPacketSize\":188.5",
          MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        { "\"mpeg2tsMode\":\"unmodified-program\",\"mpeg2tsPacketSize\":-188",
          MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        { "\"mpeg2tsMode\":\"unmodified-program\","
          "\"mpeg2tsPacketSize\":99999999999",
          MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        /* Timestamp Mode: MUST NOT be present at 188. */
        { "\"mpeg2tsMode\":\"unmodified-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsTimestampMode\":\"opaque\"",
          MOQ_MSF_MPEG2TS_ERR_TIMESTAMP_MODE_188 },
        { "\"mpeg2tsMode\":\"unmodified-program\",\"mpeg2tsPacketSize\":192,"
          "\"mpeg2tsTimestampMode\":7", MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        /* ES PID: present in es-packets, absent otherwise. */
        { "\"mpeg2tsMode\":\"es-packets\",\"mpeg2tsPacketSize\":188",
          MOQ_MSF_MPEG2TS_ERR_ES_PID_MISSING },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsEsPid\":257", MOQ_MSF_MPEG2TS_ERR_ES_PID_FORBIDDEN },
        { "\"mpeg2tsMode\":\"unmodified-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsEsPid\":257", MOQ_MSF_MPEG2TS_ERR_ES_PID_FORBIDDEN },
        { "\"mpeg2tsMode\":\"unmodified-multiplex\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsEsPid\":257", MOQ_MSF_MPEG2TS_ERR_ES_PID_FORBIDDEN },
        /* Ranges (Security Considerations). */
        { "\"mpeg2tsMode\":\"es-packets\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsEsPid\":8192", MOQ_MSF_MPEG2TS_ERR_PID_RANGE },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsPcrPid\":8192", MOQ_MSF_MPEG2TS_ERR_PID_RANGE },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsScte35Pid\":65535", MOQ_MSF_MPEG2TS_ERR_PID_RANGE },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsSiPids\":[17,9000]", MOQ_MSF_MPEG2TS_ERR_PID_RANGE },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsProgramNumber\":0", MOQ_MSF_MPEG2TS_ERR_PROGRAM_NUMBER_RANGE },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsProgramNumber\":65536",
          MOQ_MSF_MPEG2TS_ERR_PROGRAM_NUMBER_RANGE },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsMuxRate\":0", MOQ_MSF_MPEG2TS_ERR_MUX_RATE_RANGE },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsPcrPid\":null", MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsMuxRate\":1.5e0", MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsMuxRate\":1e20", MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":1.885e2",
          MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":18800e-3",
          MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        /* Mux Rate: absent in unmodified-multiplex. */
        { "\"mpeg2tsMode\":\"unmodified-multiplex\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsMuxRate\":20000000", MOQ_MSF_MPEG2TS_ERR_MUX_RATE_FORBIDDEN },
        /* SI PIDs: absent with ES PID and in unmodified-multiplex; listed
         * once each; an array of numbers. */
        { "\"mpeg2tsMode\":\"es-packets\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsEsPid\":18,\"mpeg2tsSiPids\":[18]",
          MOQ_MSF_MPEG2TS_ERR_SI_PIDS_FORBIDDEN },
        { "\"mpeg2tsMode\":\"unmodified-multiplex\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsSiPids\":[17]", MOQ_MSF_MPEG2TS_ERR_SI_PIDS_FORBIDDEN },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsSiPids\":[17,18,17]", MOQ_MSF_MPEG2TS_ERR_SI_PIDS_DUPLICATE },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsSiPids\":17", MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsSiPids\":[17,\"18\"]", MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        /* SCTE-35 PID: same presence rule as SI PIDs. */
        { "\"mpeg2tsMode\":\"es-packets\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsEsPid\":500,\"mpeg2tsScte35Pid\":500",
          MOQ_MSF_MPEG2TS_ERR_SCTE35_PID_FORBIDDEN },
        { "\"mpeg2tsMode\":\"unmodified-multiplex\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsScte35Pid\":500", MOQ_MSF_MPEG2TS_ERR_SCTE35_PID_FORBIDDEN },
        /* Random Access: absent in unmodified-multiplex without a reference
         * program. */
        { "\"mpeg2tsMode\":\"unmodified-multiplex\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsRandomAccess\":true",
          MOQ_MSF_MPEG2TS_ERR_RANDOM_ACCESS_FORBIDDEN },
        { "\"mpeg2tsMode\":\"unmodified-multiplex\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsRandomAccess\":false",
          MOQ_MSF_MPEG2TS_ERR_RANDOM_ACCESS_FORBIDDEN },
        { "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsRandomAccess\":\"yes\"", MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD },
        /* Switching: an unmodified-multiplex track is in no altGroup. */
        { "\"mpeg2tsMode\":\"unmodified-multiplex\",\"mpeg2tsPacketSize\":188,"
          "\"altGroup\":1", MOQ_MSF_MPEG2TS_ERR_ALT_GROUP_FORBIDDEN },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        moq_msf_catalog_t c;
        /* Lenient parse: the catalog is accepted, the track is judged. */
        moq_result_t rc = parse(one_track(cases[i].fields, NULL), &c);
        CHECK(rc == MOQ_OK);
        if (rc != MOQ_OK) { fprintf(stderr, "  case %zu\n", i); continue; }
        moq_msf_mpeg2ts_error_t r = MOQ_MSF_MPEG2TS_OK;
        CHECK(moq_msf_track_mpeg2ts_check(&c.tracks[0], &r) == MOQ_ERR_PROTO);
        CHECK(r == cases[i].want);
        if (r != cases[i].want)
            fprintf(stderr, "  case %zu: reason %d, want %d\n", i, (int)r,
                    (int)cases[i].want);

        /* Strict encode refuses the same track. */
        moq_rcbuf_t *out = NULL;
        CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_ERR_INVAL);
        CHECK(out == NULL);
        moq_msf_catalog_cleanup(&g_alloc, &c);
    }
    CHECK(g_count.balance == 0);
}

static void test_track_check_positives(void)
{
    static const char *const ok[] = {
        /* The editor's copy lets unmodified-multiplex name a reference
         * program with its PCR PID, and then carry mpeg2tsRandomAccess. */
        "\"mpeg2tsMode\":\"unmodified-multiplex\",\"mpeg2tsPacketSize\":188,"
        "\"mpeg2tsProgramNumber\":3,\"mpeg2tsPcrPid\":769,"
        "\"mpeg2tsRandomAccess\":true",
        /* An unrecognized timestamp mode at 192 reads as opaque. */
        "\"mpeg2tsMode\":\"unmodified-program\",\"mpeg2tsPacketSize\":192,"
        "\"mpeg2tsTimestampMode\":\"gps\"",
        /* 192 without a timestamp mode. */
        "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":192",
        /* Every optional per-program field at the edge of its range. */
        "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
        "\"mpeg2tsProgramNumber\":65535,\"mpeg2tsPcrPid\":8191,"
        "\"mpeg2tsMuxRate\":1,\"mpeg2tsSiPids\":[16,17,18,20],"
        "\"mpeg2tsScte35Pid\":0,\"mpeg2tsRandomAccess\":false",
        /* es-packets carrying the PAT. */
        "\"mpeg2tsMode\":\"es-packets\",\"mpeg2tsPacketSize\":188,"
        "\"mpeg2tsEsPid\":0,\"role\":\"pat\"",
        /* Unknown keys, mpeg2ts-prefixed or not, are ignored. */
        "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
        "\"mpeg2tsFutureField\":{\"x\":1},\"somethingElse\":[1,2]",
        /* An empty SI PID list. */
        "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
        "\"mpeg2tsSiPids\":[]",
        /* Integral values in any JSON number spelling. */
        "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188.0,"
        "\"mpeg2tsMuxRate\":1e6,\"mpeg2tsProgramNumber\":2.0e0,"
        "\"mpeg2tsPcrPid\":25.6E1,\"mpeg2tsSiPids\":[1.7e1,18E0]",
        "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":18800e-2",
    };
    for (size_t i = 0; i < sizeof(ok) / sizeof(ok[0]); i++) {
        moq_msf_catalog_t c;
        CHECK(parse(one_track(ok[i], NULL), &c) == MOQ_OK);
        moq_msf_mpeg2ts_error_t r = MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD;
        CHECK(moq_msf_track_mpeg2ts(&c.tracks[0]) != NULL);
        CHECK(moq_msf_track_mpeg2ts_check(&c.tracks[0], &r) == MOQ_OK);
        CHECK(r == MOQ_MSF_MPEG2TS_OK);
        if (r != MOQ_MSF_MPEG2TS_OK)
            fprintf(stderr, "  positive %zu: reason %d\n", i, (int)r);
        moq_msf_catalog_cleanup(&g_alloc, &c);
    }

    /* One invalid mpeg2ts track never fails the catalog; the others stay. */
    const char *two =
        "{\"version\":\"1\",\"tracks\":["
        "{\"name\":\"bad\",\"packaging\":\"mpeg2ts\",\"isLive\":true,"
        "\"mpeg2tsMode\":\"es-units\",\"mpeg2tsEsPid\":258,"
        "\"mpeg2tsEsPayload\":\"pes\"},"
        "{\"name\":\"good\",\"packaging\":\"mpeg2ts\",\"isLive\":true,"
        "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188}]}";
    moq_msf_catalog_t c;
    CHECK(parse(two, &c) == MOQ_OK);
    CHECK(c.track_count == 2);
    CHECK(check_reason(&c.tracks[0]) == MOQ_MSF_MPEG2TS_ERR_MODE_UNRECOGNIZED);
    CHECK(check_reason(&c.tracks[1]) == MOQ_MSF_MPEG2TS_OK);
    {
        /* Integral spellings land as their values. */
        moq_msf_catalog_t d;
        CHECK(parse(one_track("\"mpeg2tsMode\":\"per-program\","
                              "\"mpeg2tsPacketSize\":188.0,\"mpeg2tsMuxRate\":1e6,"
                              "\"mpeg2tsSiPids\":[1.7e1]", NULL), &d) == MOQ_OK);
        moq_msf_mpeg2ts_t m = ts_of(&d.tracks[0]);
        CHECK(m.packet_size == 188 && m.mux_rate == 1000000);
        CHECK(m.si_pid_count == 1 && m.si_pids[0] == 17);
        moq_msf_catalog_cleanup(&g_alloc, &d);
    }
    moq_msf_catalog_cleanup(&g_alloc, &c);

    /* Not an mpeg2ts track. */
    CHECK(parse("{\"version\":\"1\",\"tracks\":[{\"name\":\"v\","
                "\"packaging\":\"loc\",\"isLive\":true}]}", &c) == MOQ_OK);
    moq_msf_mpeg2ts_error_t r = MOQ_MSF_MPEG2TS_OK;
    CHECK(c.tracks[0].extension_count == 0 &&
          moq_msf_track_mpeg2ts(&c.tracks[0]) == NULL);
    CHECK(moq_msf_track_mpeg2ts_check(&c.tracks[0], &r) == MOQ_ERR_INVAL);
    CHECK(r == MOQ_MSF_MPEG2TS_ERR_NOT_MPEG2TS);
    CHECK(moq_msf_track_mpeg2ts_check(NULL, &r) == MOQ_ERR_INVAL);
    moq_msf_catalog_cleanup(&g_alloc, &c);

    /* An mpeg2ts track without the record lacks Mode. */
    CHECK(parse("{\"version\":\"1\",\"tracks\":[{\"name\":\"v\","
                "\"packaging\":\"mpeg2ts\",\"isLive\":true}]}", &c) == MOQ_OK);
    CHECK(moq_msf_track_mpeg2ts(&c.tracks[0]) == NULL);
    CHECK(moq_msf_track_mpeg2ts_check(&c.tracks[0], &r) == MOQ_ERR_PROTO);
    CHECK(r == MOQ_MSF_MPEG2TS_ERR_MODE_MISSING);
    moq_msf_catalog_cleanup(&g_alloc, &c);
    CHECK(g_count.balance == 0);
}

/* -- Strict encode of hand-built tracks ---------------------------------- */

static void base_track(moq_msf_track_t *t, moq_msf_mpeg2ts_t *m)
{
    memset(t, 0, sizeof(*t));
    t->struct_size = sizeof(*t);
    t->name = lit("p1");
    t->packaging = lit("mpeg2ts");
    t->is_live = true;
    moq_msf_mpeg2ts_init(m);
    m->has_mode = true;
    m->mode = MOQ_MSF_MPEG2TS_MODE_PER_PROGRAM;
    m->has_packet_size = true;
    m->packet_size = 188;
}

/* Encode a catalog of one track carrying mpeg2ts fields m (none when NULL). */
static moq_result_t encode_one(const moq_msf_track_t *t,
                               const moq_msf_mpeg2ts_t *m, moq_rcbuf_t **out)
{
    moq_msf_track_t tt = *t;
    moq_msf_track_ext_t ext[] = { { .kind = MOQ_MSF_EXT_MPEG2TS, .u.mpeg2ts = m } };
    tt.extensions = m ? ext : NULL;
    tt.extension_count = m ? 1 : 0;
    moq_msf_catalog_t c;
    memset(&c, 0, sizeof(c));
    c.struct_size = sizeof(c);
    c.version = MOQ_MSF_VERSION;
    c.tracks = &tt;
    c.track_count = 1;
    *out = NULL;
    return moq_msf_catalog_encode(&g_alloc, &c, out);
}

static void test_strict_encode(void)
{
    moq_msf_track_t t;
    moq_msf_mpeg2ts_t m;
    moq_rcbuf_t *out = NULL;

    base_track(&t, &m);
    uint32_t si[] = { 17, 18 };
    m.has_si_pids = true;
    m.si_pids = si;
    m.si_pid_count = 2;
    m.has_program_number = true;
    m.program_number = 4;
    CHECK(encode_one(&t, &m, &out) == MOQ_OK);
    const char *want =
        "{\"version\":\"1\",\"tracks\":[{\"name\":\"p1\",\"packaging\":\"mpeg2ts\","
        "\"isLive\":true,\"mpeg2tsMode\":\"per-program\","
        "\"mpeg2tsPacketSize\":188,\"mpeg2tsProgramNumber\":4,"
        "\"mpeg2tsSiPids\":[17,18]}]}";
    CHECK(out && moq_rcbuf_len(out) == strlen(want) &&
          memcmp(moq_rcbuf_data(out), want, strlen(want)) == 0);
    if (out && moq_rcbuf_len(out) != strlen(want))
        fprintf(stderr, "  got: %.*s\n", (int)moq_rcbuf_len(out),
                (const char *)moq_rcbuf_data(out));
    moq_rcbuf_decref(out);

    /* An mpeg2ts track without the record lacks Mode and Packet Size. */
    CHECK(encode_one(&t, NULL, &out) == MOQ_ERR_INVAL);

    /* The enum, not a stale name, is what is written. */
    base_track(&t, &m);
    m.mode_name = lit("es-units");
    CHECK(encode_one(&t, &m, &out) == MOQ_OK);
    CHECK(out && !buf_contains(out, "es-units"));
    moq_rcbuf_decref(out);

    base_track(&t, &m);
    m.mode = MOQ_MSF_MPEG2TS_MODE_UNRECOGNIZED;
    m.mode_name = lit("es-units");
    CHECK(encode_one(&t, &m, &out) == MOQ_ERR_INVAL);
    m.mode = (moq_msf_mpeg2ts_mode_t)99;
    CHECK(encode_one(&t, &m, &out) == MOQ_ERR_INVAL);

    base_track(&t, &m);
    m.packet_size = 190;
    CHECK(encode_one(&t, &m, &out) == MOQ_ERR_INVAL);

    /* Only the two defined timestamp modes are authored. */
    base_track(&t, &m);
    m.packet_size = 192;
    m.has_timestamp_mode = true;
    m.timestamp_mode = MOQ_MSF_MPEG2TS_TIMESTAMP_UNRECOGNIZED;
    m.timestamp_mode_name = lit("gps");
    CHECK(encode_one(&t, &m, &out) == MOQ_ERR_INVAL);
    m.timestamp_mode = (moq_msf_mpeg2ts_timestamp_mode_t)99;
    CHECK(encode_one(&t, &m, &out) == MOQ_ERR_INVAL);
    CHECK(out == NULL);
    m.timestamp_mode = MOQ_MSF_MPEG2TS_TIMESTAMP_OPAQUE;
    CHECK(encode_one(&t, &m, &out) == MOQ_OK);
    CHECK(out && buf_contains(out, "\"mpeg2tsTimestampMode\":\"opaque\""));
    moq_rcbuf_decref(out);

    base_track(&t, &m);
    m.has_si_pids = true;
    m.si_pid_count = 2;            /* count without an array */
    CHECK(encode_one(&t, &m, &out) == MOQ_ERR_INVAL);

    base_track(&t, &m);
    m.malformed = MOQ_MSF_MPEG2TS_F_PCR_PID;
    CHECK(encode_one(&t, &m, &out) == MOQ_ERR_INVAL);

    /* mpeg2ts fields belong to an mpeg2ts track. */
    base_track(&t, &m);
    t.packaging = lit("loc");
    CHECK(encode_one(&t, &m, &out) == MOQ_ERR_INVAL);
    CHECK(encode_one(&t, NULL, &out) == MOQ_OK);
    CHECK(out && !buf_contains(out, "mpeg2ts"));
    moq_rcbuf_decref(out);

    /* The extension list itself: an empty list encodes nothing; a count
     * without an array, an unknown kind, a kind listed twice, a NULL record
     * and an undersized record are refused. */
    base_track(&t, &m);
    moq_msf_catalog_t c;
    memset(&c, 0, sizeof(c));
    c.struct_size = sizeof(c);
    c.version = MOQ_MSF_VERSION;
    c.tracks = &t;
    c.track_count = 1;
    moq_msf_track_ext_t ext[2] = {
        { .kind = MOQ_MSF_EXT_MPEG2TS, .u.mpeg2ts = &m },
        { .kind = MOQ_MSF_EXT_MPEG2TS, .u.mpeg2ts = &m },
    };
    t.extensions = ext;
    t.extension_count = 1;
    CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_OK);
    moq_rcbuf_decref(out);
    t.extension_count = 2;                              /* duplicate kind */
    CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_ERR_INVAL);
    t.extension_count = 1;
    ext[0].kind = (moq_msf_ext_kind_t)7;                /* unknown kind */
    CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_ERR_INVAL);
    ext[0].kind = MOQ_MSF_EXT_NONE;
    CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_ERR_INVAL);
    ext[0].kind = MOQ_MSF_EXT_MPEG2TS;
    ext[0].u.mpeg2ts = NULL;                            /* no record */
    CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_ERR_INVAL);
    moq_msf_mpeg2ts_t small = m;
    small.struct_size = 8;                              /* below v0 */
    ext[0].u.mpeg2ts = &small;
    CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_ERR_INVAL);
    moq_msf_mpeg2ts_error_t why;
    CHECK(moq_msf_track_mpeg2ts_check(&t, &why) == MOQ_ERR_INVAL);
    t.extensions = NULL;                                /* count, no array */
    CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_ERR_INVAL);
    t.extension_count = 0;
    t.packaging = lit("loc");
    CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_OK);
    CHECK(out && !buf_contains(out, "mpeg2ts"));
    moq_rcbuf_decref(out);
    /* The kind belongs to "mpeg2ts": an empty record on another packaging
     * is refused too. */
    moq_msf_mpeg2ts_t empty;
    moq_msf_mpeg2ts_init(&empty);
    ext[0].u.mpeg2ts = &empty;
    t.extensions = ext;
    t.extension_count = 1;
    CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_ERR_INVAL);
    CHECK(moq_msf_ext_find(ext, 1, MOQ_MSF_EXT_MPEG2TS) == &ext[0]);
    CHECK(moq_msf_ext_find(ext, 1, MOQ_MSF_EXT_NONE) == NULL);
    CHECK(moq_msf_ext_find(NULL, 3, MOQ_MSF_EXT_MPEG2TS) == NULL);
    CHECK(moq_msf_ext_mpeg2ts(ext, 1) == &empty);
    CHECK(moq_msf_track_mpeg2ts(NULL) == NULL);
    CHECK(g_count.balance == 0);
}

/* The record travels in the track's extension list: arrays of tracks carry
 * it in both directions, a track without mpeg2ts keys has no entry, an
 * mpeg2ts-prefixed key the draft does not define adds none, and cleanup
 * frees the parsed list, record and SI PID array. */
static void test_track_extensions(void)
{
    moq_msf_catalog_t c;
    CHECK(parse("{\"version\":\"1\",\"tracks\":["
                "{\"name\":\"a\",\"packaging\":\"loc\",\"isLive\":true,"
                "\"mpeg2tsFutureField\":1},"
                "{\"name\":\"bb\",\"packaging\":\"mpeg2ts\",\"isLive\":true,"
                "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":192,"
                "\"mpeg2tsSiPids\":[17,18]}]}",
                &c) == MOQ_OK);
    CHECK(c.track_count == 2 && bv_eq(c.tracks[1].name, "bb"));
    CHECK(c.tracks[0].extension_count == 0 && c.tracks[0].extensions == NULL);
    CHECK(c.tracks[1].extension_count == 1 &&
          c.tracks[1].extensions[0].kind == MOQ_MSF_EXT_MPEG2TS);
    const moq_msf_mpeg2ts_t *pm = moq_msf_track_mpeg2ts(&c.tracks[1]);
    CHECK(pm && pm->struct_size == sizeof(*pm) && pm->packet_size == 192);
    CHECK(pm && pm->si_pid_count == 2 && pm->si_pids[1] == 18);
    CHECK(check_reason(&c.tracks[1]) == MOQ_MSF_MPEG2TS_OK);
    moq_msf_catalog_cleanup(&g_alloc, &c);

    moq_msf_track_t two[2];
    moq_msf_mpeg2ts_t m;
    base_track(&two[0], &m);
    two[0].packaging = lit("loc");
    base_track(&two[1], &m);
    two[1].name = lit("two");
    moq_msf_track_ext_t ext[] = { { .kind = MOQ_MSF_EXT_MPEG2TS, .u.mpeg2ts = &m } };
    two[1].extensions = ext;
    two[1].extension_count = 1;
    memset(&c, 0, sizeof(c));
    c.struct_size = sizeof(c);
    c.version = MOQ_MSF_VERSION;
    c.tracks = two;
    c.track_count = 2;
    moq_rcbuf_t *out = NULL;
    CHECK(moq_msf_catalog_encode(&g_alloc, &c, &out) == MOQ_OK);
    CHECK(out && buf_contains(out, "\"two\"") && buf_contains(out, "per-program"));
    moq_rcbuf_decref(out);
    CHECK(g_count.balance == 0);
}

/* -- Delta updates -------------------------------------------------------- */

static void test_delta(void)
{
    moq_msf_catalog_t base, delta, eff;
    CHECK(parse("{\"version\":\"1\",\"tracks\":[{\"name\":\"p1\","
                "\"packaging\":\"mpeg2ts\",\"isLive\":true,"
                "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
                "\"mpeg2tsProgramNumber\":1,\"mpeg2tsSiPids\":[17,18],"
                "\"mpeg2tsPcrPid\":null}]}", &base) == MOQ_OK);
    CHECK(check_reason(&base.tracks[0]) == MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD);

    /* Clone with a new program number: the rest is inherited, including a
     * malformed field, which stays malformed through the re-encode. */
    CHECK(parse("{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p1\","
                "\"name\":\"p2\",\"mpeg2tsProgramNumber\":2}]}]}", &delta) == MOQ_OK);
    CHECK(ts_of(&delta.delta_update[0].tracks[0]).program_number == 2);
    CHECK(moq_msf_catalog_apply_delta(&g_alloc, &base, &delta, &eff) == MOQ_OK);
    CHECK(eff.track_count == 2);
    moq_msf_mpeg2ts_t mv = ts_of(&eff.tracks[1]);
    const moq_msf_mpeg2ts_t *m = &mv;
    CHECK(m->program_number == 2);
    CHECK(m->mode == MOQ_MSF_MPEG2TS_MODE_PER_PROGRAM && m->packet_size == 188);
    CHECK(m->si_pid_count == 2 && m->si_pids[1] == 18);
    CHECK(m->has_pcr_pid && (m->malformed & MOQ_MSF_MPEG2TS_F_PCR_PID));
    CHECK(check_reason(&eff.tracks[1]) == MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD);
    /* The parent keeps its own fields. */
    CHECK(ts_of(&eff.tracks[0]).program_number == 1);
    moq_msf_catalog_cleanup(&g_alloc, &eff);
    moq_msf_catalog_cleanup(&g_alloc, &delta);

    /* A clone that repairs the malformed field. */
    CHECK(parse("{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p1\","
                "\"name\":\"p3\",\"mpeg2tsPcrPid\":257}]}]}", &delta) == MOQ_OK);
    CHECK(moq_msf_catalog_apply_delta(&g_alloc, &base, &delta, &eff) == MOQ_OK);
    CHECK(ts_of(&eff.tracks[1]).pcr_pid == 257);
    CHECK(check_reason(&eff.tracks[1]) == MOQ_MSF_MPEG2TS_OK);
    moq_msf_catalog_cleanup(&g_alloc, &eff);
    moq_msf_catalog_cleanup(&g_alloc, &delta);

    /* An add carries its own fields; a remove drops the record with the
     * track. */
    CHECK(parse("{\"deltaUpdate\":[{\"op\":\"add\",\"tracks\":[{\"name\":\"p4\","
                "\"packaging\":\"mpeg2ts\",\"isLive\":true,"
                "\"mpeg2tsMode\":\"es-packets\",\"mpeg2tsPacketSize\":188,"
                "\"mpeg2tsEsPid\":256}]},{\"op\":\"remove\",\"tracks\":"
                "[{\"name\":\"p1\"}]}]}", &delta) == MOQ_OK);
    CHECK(moq_msf_catalog_apply_delta(&g_alloc, &base, &delta, &eff) == MOQ_OK);
    CHECK(eff.track_count == 1 && bv_eq(eff.tracks[0].name, "p4"));
    CHECK(ts_of(&eff.tracks[0]).es_pid == 256);
    CHECK(check_reason(&eff.tracks[0]) == MOQ_MSF_MPEG2TS_OK);
    moq_msf_catalog_cleanup(&g_alloc, &eff);
    moq_msf_catalog_cleanup(&g_alloc, &delta);

    /* An unrecognized mode survives the receive-path re-encode as itself. */
    moq_msf_catalog_t b2;
    CHECK(parse("{\"version\":\"1\",\"tracks\":[{\"name\":\"x\","
                "\"packaging\":\"mpeg2ts\",\"isLive\":true,"
                "\"mpeg2tsMode\":\"es-units\"}]}", &b2) == MOQ_OK);
    CHECK(parse("{\"deltaUpdate\":[{\"op\":\"add\",\"tracks\":[{\"name\":\"y\","
                "\"packaging\":\"loc\",\"isLive\":true}]}]}", &delta) == MOQ_OK);
    CHECK(moq_msf_catalog_apply_delta(&g_alloc, &b2, &delta, &eff) == MOQ_OK);
    CHECK(bv_eq(ts_of(&eff.tracks[0]).mode_name, "es-units"));
    CHECK(check_reason(&eff.tracks[0]) == MOQ_MSF_MPEG2TS_ERR_MODE_UNRECOGNIZED);
    moq_msf_catalog_cleanup(&g_alloc, &eff);
    moq_msf_catalog_cleanup(&g_alloc, &delta);
    moq_msf_catalog_cleanup(&g_alloc, &b2);

    /* A remove op carries name and namespace only. */
    CHECK(parse("{\"deltaUpdate\":[{\"op\":\"remove\",\"tracks\":[{\"name\":\"p1\","
                "\"mpeg2tsMode\":\"per-program\"}]}]}", &delta) == MOQ_ERR_PROTO);
    moq_msf_catalog_cleanup(&g_alloc, &base);
    CHECK(g_count.balance == 0);
}

/* Strict authoring of delta updates: the same rules as an independent
 * catalog for a whole added track, and each clone override on its own. */
static void test_delta_authoring(void)
{
    static const struct { const char *json; moq_result_t want; } cases[] = {
        { "{\"deltaUpdate\":[{\"op\":\"add\",\"tracks\":[{\"name\":\"t\","
          "\"packaging\":\"mpeg2ts\",\"isLive\":true,\"mpeg2tsMode\":\"per-program\","
          "\"mpeg2tsPacketSize\":188}]}]}", MOQ_OK },
        { "{\"deltaUpdate\":[{\"op\":\"add\",\"tracks\":[{\"name\":\"t\","
          "\"packaging\":\"mpeg2ts\",\"isLive\":true,\"mpeg2tsMode\":\"per-program\","
          "\"mpeg2tsPacketSize\":204}]}]}", MOQ_ERR_INVAL },
        { "{\"deltaUpdate\":[{\"op\":\"add\",\"tracks\":[{\"name\":\"t\","
          "\"packaging\":\"mpeg2ts\",\"isLive\":true}]}]}", MOQ_ERR_INVAL },
        { "{\"deltaUpdate\":[{\"op\":\"add\",\"tracks\":[{\"name\":\"t\","
          "\"packaging\":\"loc\",\"isLive\":true,\"mpeg2tsMode\":\"per-program\"}]}]}",
          MOQ_ERR_INVAL },
        { "{\"deltaUpdate\":[{\"op\":\"add\",\"tracks\":[{\"name\":\"t\","
          "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188}]}]}",
          MOQ_ERR_INVAL },
        { "{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p\","
          "\"name\":\"q\",\"mpeg2tsProgramNumber\":2}]}]}", MOQ_OK },
        { "{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p\","
          "\"name\":\"q\",\"mpeg2tsPacketSize\":204}]}]}", MOQ_ERR_INVAL },
        { "{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p\","
          "\"name\":\"q\",\"mpeg2tsProgramNumber\":0}]}]}", MOQ_ERR_INVAL },
        { "{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p\","
          "\"name\":\"q\",\"mpeg2tsMode\":\"es-units\"}]}]}", MOQ_ERR_INVAL },
        { "{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p\","
          "\"name\":\"q\",\"mpeg2tsPcrPid\":null}]}]}", MOQ_ERR_INVAL },
        { "{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p\","
          "\"name\":\"q\",\"mpeg2tsMode\":\"per-program\",\"mpeg2tsEsPid\":5}]}]}",
          MOQ_ERR_INVAL },
        { "{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p\","
          "\"name\":\"q\",\"mpeg2tsPacketSize\":188,"
          "\"mpeg2tsTimestampMode\":\"opaque\"}]}]}", MOQ_ERR_INVAL },
        { "{\"deltaUpdate\":[{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p\","
          "\"name\":\"q\",\"packaging\":\"loc\",\"mpeg2tsProgramNumber\":2}]}]}",
          MOQ_ERR_INVAL },
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        moq_msf_catalog_t d;
        CHECK(parse(cases[i].json, &d) == MOQ_OK);
        moq_rcbuf_t *out = NULL;
        moq_result_t rc = moq_msf_catalog_encode(&g_alloc, &d, &out);
        CHECK(rc == cases[i].want);
        if (rc != cases[i].want) fprintf(stderr, "  delta case %zu: %d\n", i, rc);
        if (out) moq_rcbuf_decref(out);
        moq_msf_catalog_cleanup(&g_alloc, &d);
    }
    CHECK(g_count.balance == 0);
}

/* -- Initialization data --------------------------------------------------- */

static uint32_t ref_crc(const uint8_t *d, size_t n)
{
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) {
        c ^= (uint32_t)d[i] << 24;
        for (int b = 0; b < 8; b++)
            c = (c & 0x80000000u) ? (c << 1) ^ 0x04C11DB7u : (c << 1);
    }
    return c;
}

/* One PAT packet (the draft's example) and one PMT packet. */
static void psi_packets(uint8_t out[376])
{
    static const uint8_t pat[] = {
        0x47, 0x40, 0x00, 0x10, 0x00, 0x00, 0xB0, 0x0D, 0x00, 0x01, 0xC1,
        0x00, 0x00, 0x00, 0x01, 0xE1, 0x00, 0xE8, 0xF9, 0x5E, 0x7D,
    };
    memset(out, 0xFF, 376);
    memcpy(out, pat, sizeof(pat));
    uint8_t *p = out + 188;
    p[0] = 0x47; p[1] = 0x41; p[2] = 0x00; p[3] = 0x10; p[4] = 0x00;
    uint8_t *s = p + 5;
    const uint8_t pmt[] = {
        0x02, 0xB0, 0x17, 0x00, 0x01, 0xC1, 0x00, 0x00, 0xE1, 0x01, 0xF0, 0x00,
        0x1B, 0xE1, 0x01, 0xF0, 0x00, 0x0F, 0xE1, 0x02, 0xF0, 0x00,
    };
    memcpy(s, pmt, sizeof(pmt));
    uint32_t crc = ref_crc(s, sizeof(pmt));
    s[22] = (uint8_t)(crc >> 24); s[23] = (uint8_t)(crc >> 16);
    s[24] = (uint8_t)(crc >> 8);  s[25] = (uint8_t)crc;
}

static moq_result_t init_data_of(const moq_msf_catalog_t *c, moq_rcbuf_t **pk,
                                 moq_msf_mpeg2ts_error_t *r)
{
    return moq_msf_track_mpeg2ts_init_data(&g_alloc, c, &c->tracks[0], pk, r);
}

/* Strict encode of a parsed catalog. */
static moq_result_t reencode(const moq_msf_catalog_t *c)
{
    moq_rcbuf_t *out = NULL;
    moq_result_t rc = moq_msf_catalog_encode(&g_alloc, c, &out);
    if (out) moq_rcbuf_decref(out);
    return rc;
}

static char *catalog_with_init(const char *b64, const char *type, int ps)
{
    static char buf[4096];
    snprintf(buf, sizeof(buf),
             "{\"version\":\"1\",\"tracks\":[{\"name\":\"p\","
             "\"packaging\":\"mpeg2ts\",\"isLive\":true,"
             "\"initRef\":\"psi\",\"mpeg2tsMode\":\"per-program\","
             "\"mpeg2tsPacketSize\":%d}],"
             "\"initDataList\":[{\"id\":\"psi\",\"type\":\"%s\","
             "\"data\":\"%s\"}]}", ps, type, b64);
    return buf;
}

static void test_init_data(void)
{
    uint8_t pk[376];
    psi_packets(pk);
    moq_rcbuf_t *b64 = NULL;
    CHECK(moq_msf_encode_init_data(&g_alloc, (moq_bytes_t){ pk, 376 }, &b64) == MOQ_OK);
    char enc[600];
    snprintf(enc, sizeof(enc), "%.*s", (int)moq_rcbuf_len(b64),
             (const char *)moq_rcbuf_data(b64));
    moq_rcbuf_decref(b64);
    /* The draft's example data starts with the same PAT packet. */
    CHECK(strncmp(enc, "R0AAEAAAsA0AAcEAAAAB4QDo+V59////", 32) == 0);

    moq_msf_catalog_t c;
    moq_rcbuf_t *pkts = NULL;
    moq_msf_mpeg2ts_error_t r;

    CHECK(parse(catalog_with_init(enc, "inline", 188), &c) == MOQ_OK);
    CHECK(init_data_of(&c, &pkts, &r) == MOQ_OK);
    CHECK(pkts && moq_rcbuf_len(pkts) == 376);
    CHECK(pkts && memcmp(moq_rcbuf_data(pkts), pk, 376) == 0);
    moq_rcbuf_decref(pkts);
    /* NULL out_packets: check only. */
    CHECK(init_data_of(&c, NULL, &r) == MOQ_OK);
    /* Strict encode accepts the catalog it came from. */
    CHECK(reencode(&c) == MOQ_OK);
    moq_msf_catalog_cleanup(&g_alloc, &c);

    /* type MUST be "inline". */
    CHECK(parse(catalog_with_init(enc, "url", 188), &c) == MOQ_OK);
    CHECK(init_data_of(&c, &pkts, &r) ==
          MOQ_ERR_PROTO);
    CHECK(r == MOQ_MSF_MPEG2TS_ERR_INIT_DATA_TYPE);
    CHECK(reencode(&c) == MOQ_ERR_INVAL);
    moq_msf_catalog_cleanup(&g_alloc, &c);

    /* Whole packets of the declared size: 376 octets are not 192-octet
     * packets. */
    CHECK(parse(catalog_with_init(enc, "inline", 192), &c) == MOQ_OK);
    CHECK(init_data_of(&c, &pkts, &r) ==
          MOQ_ERR_PROTO);
    CHECK(r == MOQ_MSF_MPEG2TS_ERR_INIT_DATA_PACKETS);
    CHECK(reencode(&c) == MOQ_ERR_INVAL);
    moq_msf_catalog_cleanup(&g_alloc, &c);

    /* A partial packet. */
    CHECK(moq_msf_encode_init_data(&g_alloc, (moq_bytes_t){ pk, 300 }, &b64) == MOQ_OK);
    char enc2[600];
    snprintf(enc2, sizeof(enc2), "%.*s", (int)moq_rcbuf_len(b64),
             (const char *)moq_rcbuf_data(b64));
    moq_rcbuf_decref(b64);
    CHECK(parse(catalog_with_init(enc2, "inline", 188), &c) == MOQ_OK);
    CHECK(init_data_of(&c, &pkts, &r) ==
          MOQ_ERR_PROTO);
    CHECK(r == MOQ_MSF_MPEG2TS_ERR_INIT_DATA_PACKETS);
    CHECK(reencode(&c) == MOQ_ERR_INVAL);
    moq_msf_catalog_cleanup(&g_alloc, &c);

    /* A packet without the sync byte. */
    pk[188] = 0x00;
    CHECK(moq_msf_encode_init_data(&g_alloc, (moq_bytes_t){ pk, 376 }, &b64) == MOQ_OK);
    snprintf(enc2, sizeof(enc2), "%.*s", (int)moq_rcbuf_len(b64),
             (const char *)moq_rcbuf_data(b64));
    moq_rcbuf_decref(b64);
    CHECK(parse(catalog_with_init(enc2, "inline", 188), &c) == MOQ_OK);
    CHECK(init_data_of(&c, &pkts, &r) ==
          MOQ_ERR_PROTO);
    CHECK(r == MOQ_MSF_MPEG2TS_ERR_INIT_DATA_PACKETS);
    CHECK(reencode(&c) == MOQ_ERR_INVAL);
    moq_msf_catalog_cleanup(&g_alloc, &c);

    /* Not Base64: the draft's example, which shortens its data with "...". */
    size_t len;
    char *j = read_fixture("mpeg2ts_init_data.json", &len);
    CHECK(moq_msf_catalog_parse(&g_alloc, (moq_bytes_t){ (uint8_t *)j, len }, &c) == MOQ_OK);
    CHECK(check_reason(&c.tracks[0]) == MOQ_MSF_MPEG2TS_OK);
    CHECK(init_data_of(&c, &pkts, &r) ==
          MOQ_ERR_PROTO);
    CHECK(r == MOQ_MSF_MPEG2TS_ERR_INIT_DATA_BASE64);
    CHECK(reencode(&c) == MOQ_ERR_INVAL);
    moq_msf_catalog_cleanup(&g_alloc, &c);
    free(j);

    /* initRef that resolves to nothing. */
    CHECK(parse("{\"version\":\"1\",\"tracks\":[{\"name\":\"p\","
                "\"packaging\":\"mpeg2ts\",\"isLive\":true,\"initRef\":\"zz\","
                "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188}]}",
                &c) == MOQ_OK);
    CHECK(init_data_of(&c, &pkts, &r) ==
          MOQ_ERR_PROTO);
    CHECK(r == MOQ_MSF_MPEG2TS_ERR_INIT_REF_UNRESOLVED);
    CHECK(reencode(&c) == MOQ_ERR_INVAL);
    moq_msf_catalog_cleanup(&g_alloc, &c);

    /* One octet of Base64 data (a lone sync byte) is not a packet. */
    CHECK(parse(catalog_with_init("Rw==", "inline", 188), &c) == MOQ_OK);
    CHECK(init_data_of(&c, &pkts, &r) == MOQ_ERR_PROTO);
    CHECK(r == MOQ_MSF_MPEG2TS_ERR_INIT_DATA_PACKETS);
    CHECK(reencode(&c) == MOQ_ERR_INVAL);
    moq_msf_catalog_cleanup(&g_alloc, &c);
    /* Neither is an empty entry. */
    CHECK(parse(catalog_with_init("", "inline", 188), &c) == MOQ_OK);
    CHECK(reencode(&c) == MOQ_ERR_INVAL);
    moq_msf_catalog_cleanup(&g_alloc, &c);

    /* No initRef, or an invalid track. */
    CHECK(parse(one_track("\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188",
                          NULL), &c) == MOQ_OK);
    CHECK(init_data_of(&c, &pkts, &r) ==
          MOQ_ERR_INVAL);
    moq_msf_catalog_cleanup(&g_alloc, &c);
    CHECK(parse(one_track("\"mpeg2tsMode\":\"x\",\"initRef\":\"psi\"", NULL), &c) == MOQ_OK);
    CHECK(init_data_of(&c, &pkts, &r) ==
          MOQ_ERR_PROTO);
    CHECK(r == MOQ_MSF_MPEG2TS_ERR_MODE_UNRECOGNIZED);
    moq_msf_catalog_cleanup(&g_alloc, &c);
    CHECK(g_count.balance == 0);
}

/* The reason codes are public diagnostic values: each is pinned. */
static void test_error_values(void)
{
    static const struct { moq_msf_mpeg2ts_error_t e; int v; } pins[] = {
        { MOQ_MSF_MPEG2TS_OK, 0 },
        { MOQ_MSF_MPEG2TS_ERR_NOT_MPEG2TS, 1 },
        { MOQ_MSF_MPEG2TS_ERR_MALFORMED_FIELD, 2 },
        { MOQ_MSF_MPEG2TS_ERR_MODE_MISSING, 3 },
        { MOQ_MSF_MPEG2TS_ERR_MODE_UNRECOGNIZED, 4 },
        { MOQ_MSF_MPEG2TS_ERR_PACKET_SIZE_MISSING, 5 },
        { MOQ_MSF_MPEG2TS_ERR_PACKET_SIZE, 6 },
        { MOQ_MSF_MPEG2TS_ERR_TIMESTAMP_MODE_188, 7 },
        { MOQ_MSF_MPEG2TS_ERR_ES_PID_MISSING, 8 },
        { MOQ_MSF_MPEG2TS_ERR_ES_PID_FORBIDDEN, 9 },
        { MOQ_MSF_MPEG2TS_ERR_PID_RANGE, 10 },
        { MOQ_MSF_MPEG2TS_ERR_PROGRAM_NUMBER_RANGE, 11 },
        { MOQ_MSF_MPEG2TS_ERR_MUX_RATE_RANGE, 12 },
        { MOQ_MSF_MPEG2TS_ERR_MUX_RATE_FORBIDDEN, 13 },
        { MOQ_MSF_MPEG2TS_ERR_SI_PIDS_FORBIDDEN, 14 },
        { MOQ_MSF_MPEG2TS_ERR_SI_PIDS_DUPLICATE, 15 },
        { MOQ_MSF_MPEG2TS_ERR_SCTE35_PID_FORBIDDEN, 16 },
        { MOQ_MSF_MPEG2TS_ERR_RANDOM_ACCESS_FORBIDDEN, 17 },
        { MOQ_MSF_MPEG2TS_ERR_ALT_GROUP_FORBIDDEN, 18 },
        { MOQ_MSF_MPEG2TS_ERR_INIT_REF_UNRESOLVED, 19 },
        { MOQ_MSF_MPEG2TS_ERR_INIT_DATA_TYPE, 20 },
        { MOQ_MSF_MPEG2TS_ERR_INIT_DATA_BASE64, 21 },
        { MOQ_MSF_MPEG2TS_ERR_INIT_DATA_PACKETS, 22 },
    };
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++)
        CHECK((int)pins[i].e == pins[i].v);
    /* The extension kind is an open uint32_t set. */
    CHECK(sizeof(moq_msf_ext_kind_t) == sizeof(uint32_t));
    CHECK(MOQ_MSF_EXT_NONE == 0u && MOQ_MSF_EXT_MPEG2TS == 1u);
}

static void test_names(void)
{
    CHECK(strcmp(moq_msf_mpeg2ts_mode_name(MOQ_MSF_MPEG2TS_MODE_ES_PACKETS),
                 "es-packets") == 0);
    CHECK(strcmp(moq_msf_mpeg2ts_mode_name(MOQ_MSF_MPEG2TS_MODE_UNRECOGNIZED), "") == 0);
    CHECK(strcmp(moq_msf_mpeg2ts_mode_name((moq_msf_mpeg2ts_mode_t)99), "") == 0);
    CHECK(strcmp(moq_msf_mpeg2ts_timestamp_mode_name(
                     MOQ_MSF_MPEG2TS_TIMESTAMP_ARRIVAL_TIME), "arrival-time") == 0);
}

/* A clone merges, kind by kind, into the parent's entries: a parent without
 * the record gains the clone's, and a clone of a clone in the same delta
 * merges into the first clone's merged record. */
static void test_clone_extension_merge(void)
{
    moq_msf_catalog_t base, delta, eff;
    CHECK(parse("{\"version\":\"1\",\"tracks\":["
                "{\"name\":\"v\",\"packaging\":\"loc\",\"isLive\":true},"
                "{\"name\":\"p1\",\"packaging\":\"mpeg2ts\",\"isLive\":true,"
                "\"mpeg2tsMode\":\"per-program\",\"mpeg2tsPacketSize\":188,"
                "\"mpeg2tsSiPids\":[17]}]}", &base) == MOQ_OK);
    CHECK(parse("{\"deltaUpdate\":["
                "{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"v\",\"name\":\"w\","
                "\"mpeg2tsPcrPid\":300}]},"
                "{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p1\",\"name\":\"p2\","
                "\"mpeg2tsProgramNumber\":2}]},"
                "{\"op\":\"clone\",\"tracks\":[{\"parentName\":\"p2\",\"name\":\"p3\","
                "\"mpeg2tsPcrPid\":257}]}]}", &delta) == MOQ_OK);
    CHECK(moq_msf_catalog_apply_delta(&g_alloc, &base, &delta, &eff) == MOQ_OK);
    CHECK(eff.track_count == 5);
    const moq_msf_mpeg2ts_t *w = moq_msf_track_mpeg2ts(&eff.tracks[2]);
    CHECK(w && !w->has_mode && w->has_pcr_pid && w->pcr_pid == 300);
    const moq_msf_mpeg2ts_t *p3 = moq_msf_track_mpeg2ts(&eff.tracks[4]);
    CHECK(bv_eq(eff.tracks[4].name, "p3"));
    CHECK(p3 && p3->program_number == 2 && p3->pcr_pid == 257 &&
          p3->packet_size == 188 && p3->si_pid_count == 1 && p3->si_pids[0] == 17);
    CHECK(check_reason(&eff.tracks[4]) == MOQ_MSF_MPEG2TS_OK);
    CHECK(moq_msf_track_mpeg2ts(&eff.tracks[0]) == NULL);
    moq_msf_catalog_cleanup(&g_alloc, &eff);
    moq_msf_catalog_cleanup(&g_alloc, &delta);
    moq_msf_catalog_cleanup(&g_alloc, &base);
    CHECK(g_count.balance == 0);
}

int main(void)
{
    test_record_init_sized();
    test_examples_round_trip();
    test_examples_values();
    test_golden_encode();
    test_track_check_negatives();
    test_track_check_positives();
    test_strict_encode();
    test_track_extensions();
    test_clone_extension_merge();
    test_delta();
    test_delta_authoring();
    test_init_data();
    test_error_values();
    test_names();
    if (failures) {
        fprintf(stderr, "%d failure(s)\n", failures);
        return 1;
    }
    printf("msf_mpeg2ts: all tests passed\n");
    return 0;
}
