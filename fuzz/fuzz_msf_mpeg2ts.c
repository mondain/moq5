/*
 * Fuzz the MSF catalog parser on mpeg2ts tracks (draft-gregoire-moq-msfts):
 * parse, the subscriber's track check, initialization data, encode, and
 * applying a delta to a fixed base.
 *
 * Oracles: an encode that succeeds re-parses, and every track (independent
 * or in a delta operation) keeps its check verdict through the round trip;
 * every typed extension field survives for all tracks and delta operations;
 * a strictly encoded mpeg2ts track passes the check; cleanup returns every
 * byte.
 */

#include <moq/msf.h>
#include <moq/rcbuf.h>
#include "msf_extension_eq.h"

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

static int64_t g_balance;

static void *f_alloc(size_t n, void *ctx)
{
    (void)ctx;
    g_balance += (int64_t)n;
    return malloc(n ? n : 1);
}

static void *f_realloc(void *p, size_t o, size_t n, void *ctx)
{
    (void)ctx;
    g_balance += (int64_t)n - (int64_t)o;
    return realloc(p, n ? n : 1);
}

static void f_free(void *p, size_t n, void *ctx)
{
    (void)ctx;
    g_balance -= (int64_t)n;
    free(p);
}

static const moq_alloc_t g_alloc = {
    .ctx = NULL, .alloc = f_alloc, .realloc = f_realloc, .free = f_free,
};

static moq_msf_mpeg2ts_error_t verdict(const moq_msf_track_t *t)
{
    moq_msf_mpeg2ts_error_t why = MOQ_MSF_MPEG2TS_OK;
    moq_result_t rc = moq_msf_track_mpeg2ts_check(t, &why);
    if (rc == MOQ_OK && why != MOQ_MSF_MPEG2TS_OK) abort();
    if (rc == MOQ_ERR_PROTO && why == MOQ_MSF_MPEG2TS_OK) abort();
    return why;
}

/* Every track of a catalog, independent ones then delta-op ones. */
static size_t all_tracks(const moq_msf_catalog_t *c,
                         const moq_msf_track_t **out, size_t cap)
{
    size_t n = 0;
    for (size_t i = 0; i < c->track_count && n < cap; i++) out[n++] = &c->tracks[i];
    for (size_t i = 0; c->delta_update && i < c->delta_update_count; i++)
        for (size_t j = 0; j < c->delta_update[i].track_count && n < cap; j++)
            out[n++] = &c->delta_update[i].tracks[j];
    return n;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    g_balance = 0;
    moq_msf_catalog_t cat;
    if (moq_msf_catalog_parse(&g_alloc, (moq_bytes_t){ data, size }, &cat) != MOQ_OK) {
        if (g_balance != 0) abort();
        return 0;
    }

    enum { MAXT = 32 };
    const moq_msf_track_t *tr[MAXT];
    size_t nt = all_tracks(&cat, tr, MAXT);
    moq_msf_mpeg2ts_error_t before[MAXT];
    for (size_t i = 0; i < nt; i++) {
        before[i] = verdict(tr[i]);
        if (before[i] == MOQ_MSF_MPEG2TS_OK && tr[i]->has_init_ref) {
            moq_rcbuf_t *pk = NULL;
            moq_msf_mpeg2ts_error_t why;
            if (moq_msf_track_mpeg2ts_init_data(&g_alloc, &cat, tr[i], &pk,
                                                &why) == MOQ_OK) {
                size_t ps = moq_msf_track_mpeg2ts(tr[i])->packet_size;
                if (!pk || moq_rcbuf_len(pk) == 0 || moq_rcbuf_len(pk) % ps != 0)
                    abort();
                moq_rcbuf_decref(pk);
            }
        }
    }

    moq_rcbuf_t *json = NULL;
    if (moq_msf_catalog_encode(&g_alloc, &cat, &json) == MOQ_OK) {
        moq_msf_catalog_t back;
        if (moq_msf_catalog_parse(&g_alloc,
                (moq_bytes_t){ moq_rcbuf_data(json), moq_rcbuf_len(json) },
                &back) != MOQ_OK)
            abort();
        if (back.track_count != cat.track_count ||
            back.delta_update_count != cat.delta_update_count ||
            (back.delta_update != NULL) != (cat.delta_update != NULL)) abort();
        /* Compare every extension, not only the bounded verdict sample. */
        for (size_t i = 0; i < cat.track_count; i++)
            if (!fuzz_msf_extensions_eq(&cat.tracks[i], &back.tracks[i])) abort();
        for (size_t i = 0; i < cat.delta_update_count; i++) {
            const moq_msf_delta_op_t *a = &cat.delta_update[i];
            const moq_msf_delta_op_t *b = &back.delta_update[i];
            if (a->track_count != b->track_count) abort();
            for (size_t j = 0; j < a->track_count; j++)
                if (!fuzz_msf_extensions_eq(&a->tracks[j], &b->tracks[j])) abort();
        }
        const moq_msf_track_t *btr[MAXT];
        if (all_tracks(&back, btr, MAXT) != nt) abort();
        for (size_t i = 0; i < nt; i++) {
            moq_msf_mpeg2ts_error_t after = verdict(btr[i]);
            if (after != before[i]) abort();
            /* Authoring let it through: an independent mpeg2ts track is
             * valid. */
            if (!back.delta_update &&
                btr[i]->packaging.len == 7 &&
                memcmp(btr[i]->packaging.data, "mpeg2ts", 7) == 0 &&
                after != MOQ_MSF_MPEG2TS_OK)
                abort();
        }
        moq_msf_catalog_cleanup(&g_alloc, &back);
        moq_rcbuf_decref(json);
    }

    /* A delta applied to a fixed base with an mpeg2ts parent "p". */
    if (cat.delta_update) {
        static const char base_json[] =
            "{\"version\":\"1\",\"tracks\":[{\"name\":\"p\",\"packaging\":"
            "\"mpeg2ts\",\"isLive\":true,\"mpeg2tsMode\":\"per-program\","
            "\"mpeg2tsPacketSize\":188,\"mpeg2tsSiPids\":[17]}]}";
        moq_msf_catalog_t base, eff;
        if (moq_msf_catalog_parse(&g_alloc,
                (moq_bytes_t){ (const uint8_t *)base_json, sizeof(base_json) - 1 },
                &base) != MOQ_OK)
            abort();
        if (moq_msf_catalog_apply_delta(&g_alloc, &base, &cat, &eff) == MOQ_OK) {
            for (size_t i = 0; i < eff.track_count; i++)
                (void)verdict(&eff.tracks[i]);
            moq_msf_catalog_cleanup(&g_alloc, &eff);
        }
        moq_msf_catalog_cleanup(&g_alloc, &base);
    }

    moq_msf_catalog_cleanup(&g_alloc, &cat);
    if (g_balance != 0) abort();
    return 0;
}
