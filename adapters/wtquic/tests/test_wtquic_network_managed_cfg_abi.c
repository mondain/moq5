/*
 * moq_wtquic_network_managed cfg initializer + short-config rejection test.
 *
 * A caller built against this header gets the full current struct from both
 * initializers. The pointer initializer's symbol, which a caller built before
 * `target` was appended calls, stamps only the layout through
 * app_deadline_ctx and never writes past it. The appended app_deadline
 * callback/ctx form ONE ABI block; its import in create() lies below the
 * unconditional backend-capability rejection and is unreachable here.
 *
 * The create() rows carry a malformed offer list and are rejected
 * (MOQ_ERR_INVAL) by offer validation, which precedes the capability
 * rejection; the exact-prefix row keeps those validation reads in bounds.
 * A real static callback is used, not an integer-cast pointer.
 */
#include <moq/wtquic_network_managed.h>

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static uint64_t deadline_cb(void *ctx) { (void)ctx; return UINT64_MAX; }
static int      pump_cb(moq_wtquic_network_managed_t *m,
                        moq_wtquic_network_managed_lane_t *lane,
                        uint64_t now, void *ctx)
{ (void)m; (void)lane; (void)now; (void)ctx; return 0; }

/* Fill the fields create() validates, plus an unrecognized wt_protocols so
 * offer validation rejects it (MOQ_ERR_INVAL) before allocation; the
 * optional-tail import is unreachable. All of these live at offsets below the
 * app_deadline block, so they fit inside a prefix that stops at
 * app_deadline_us. */
static void fill_common(moq_wtquic_network_managed_cfg_t *cfg)
{
    cfg->alloc = moq_alloc_default();
    cfg->perspective = MOQ_PERSPECTIVE_CLIENT;
    cfg->host = "127.0.0.1";
    cfg->port = 443;
    cfg->wt_protocols = "not-a-real-moq-wt-protocol-token";
    cfg->app_deadline_us = deadline_cb;
}

int main(void)
{
    /* Init contract: both initializers stamp the full current struct. */
    moq_wtquic_network_managed_cfg_t a;
    moq_wtquic_network_managed_cfg_init(&a);
    if (a.struct_size != (uint32_t)sizeof(a)) return 1;
    if (a.alloc || a.on_lane_pump || a.app_deadline_us || a.app_deadline_ctx)
        return 2;
    moq_wtquic_network_managed_cfg_init_sized(&a, sizeof(a));
    if (a.struct_size != (uint32_t)sizeof(a)) return 3;

    /* The symbol, called as an old binary calls it (parenthesized: not the
     * header's sized macro), on storage that ends at the layout through
     * app_deadline_ctx followed by bytes the caller owns. */
    {
        const size_t end = offsetof(moq_wtquic_network_managed_cfg_t, app_deadline_ctx) +
                           sizeof(void *);
        const size_t align = _Alignof(moq_wtquic_network_managed_cfg_t);
        const size_t old_size = (end + align - 1) / align * align;
        if (offsetof(moq_wtquic_network_managed_cfg_t, target) < old_size) return 7;
        union {
            moq_wtquic_network_managed_cfg_t cfg;
            unsigned char bytes[sizeof(moq_wtquic_network_managed_cfg_t) + 16];
        } old;
        memset(old.bytes, 0xa5, sizeof(old.bytes));
        (moq_wtquic_network_managed_cfg_init)(&old.cfg);
        uint32_t ss;
        memcpy(&ss, old.bytes, sizeof(ss));
        if (ss != (uint32_t)old_size) return 8;
        for (size_t i = old_size; i < sizeof(old.bytes); i++)
            if (old.bytes[i] != 0xa5) return 9;
    }

    /* Full-size malformed offer is INVAL, not masked by policy rejection. */
    {
        moq_wtquic_network_managed_cfg_t cfg;
        moq_wtquic_network_managed_cfg_init(&cfg);
        fill_common(&cfg);
        cfg.on_lane_pump = pump_cb;
        cfg.app_deadline_ctx = &cfg;    /* the full block is present */
        moq_wtquic_network_managed_t *m = NULL;
        moq_result_t rc = moq_wtquic_network_managed_create(&cfg, &m);
        if (rc != MOQ_ERR_INVAL || m != NULL) return 4;
    }

    /* Allocate exactly the prefix through app_deadline_us;
     * app_deadline_ctx is NOT part of the allocation. struct_size is
     * stamped to that prefix. create() rejects during validation, and
     * every field it reads on that path lies inside the allocation. */
    size_t prefix = offsetof(moq_wtquic_network_managed_cfg_t, app_deadline_us) +
                    sizeof(((moq_wtquic_network_managed_cfg_t *)0)->app_deadline_us);
    /* The fields are set on a full, properly typed local template and only
     * the prefix bytes are copied into the short block, so no member is
     * written through a struct lvalue larger than its storage. */
    moq_wtquic_network_managed_cfg_t tmpl;
    memset(&tmpl, 0, sizeof(tmpl));
    tmpl.struct_size = (uint32_t)prefix;
    fill_common(&tmpl);                  /* every set field is within the prefix */
    /* on_lane_pump/app_deadline_ctx intentionally left unset; app_deadline_ctx
     * is beyond the prefix: not copied, not in the block. */
    unsigned char *raw = (unsigned char *)malloc(prefix);
    if (!raw) return 5;
    memcpy(raw, &tmpl, prefix);
    moq_wtquic_network_managed_cfg_t *cfg = (moq_wtquic_network_managed_cfg_t *)raw;

    moq_wtquic_network_managed_t *m = NULL;
    moq_result_t rc = moq_wtquic_network_managed_create(cfg, &m);
    /* Offer validation rejects the bogus list before the capability
     * rejection. The return code and NULL handle are asserted; only an
     * executed, instrumented out-of-bounds read on this path is visible,
     * and only under a sanitizer. */
    if (rc != MOQ_ERR_INVAL || m != NULL) { free(raw); return 6; }
    free(raw);

    printf("PASS: wtquic_network_managed_cfg_abi\n");
    return 0;
}
