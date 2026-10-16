/*
 * Service tier control surfaces:
 *   - the session-wide GOAWAY's New Session URI is readable from the
 *     endpoint, not only the DRAINING state;
 *   - the endpoint forwards SETUP authorization tokens, validated at
 *     resolve, to the backends that carry them;
 *   - a media track's Track Properties are checked at add_track against the
 *     versions the endpoint may negotiate, not first at activation.
 */
#include <moq/endpoint.h>
#include <moq/media_sender.h>
#include "test_support.h"
#include "endpoint_internal.h"
#include <string.h>

moq_endpoint_t *moq_endpoint_test_make_bare(void);
void moq_endpoint_test_free_bare(moq_endpoint_t *ep);
void moq_endpoint_test_lock_mu(moq_endpoint_t *ep);
void moq_endpoint_test_unlock_mu(moq_endpoint_t *ep);
void moq_endpoint_test_set_offer(moq_endpoint_t *ep, const moq_version_t *v, size_t n);
moq_media_sender_t *moq_media_sender_test_new(void);
void moq_media_sender_test_free(moq_media_sender_t *s);
void moq_media_sender_test_set_ep(moq_media_sender_t *s, moq_endpoint_t *ep);

static int failures;

static void note(moq_endpoint_t *ep, const char *uri)
{
    moq_endpoint_test_lock_mu(ep);
    moq_endpoint_note_goaway_locked(ep, (moq_bytes_t){ (const uint8_t *)uri, strlen(uri) });
    moq_endpoint_test_unlock_mu(ep);
}

static void test_goaway_uri(void)
{
    moq_endpoint_t *ep = moq_endpoint_test_make_bare();
    uint8_t buf[64];
    size_t len = 99;
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_get_goaway_uri(ep, buf, sizeof(buf), &len), MOQ_DONE);
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_get_goaway_uri(NULL, buf, sizeof(buf), &len), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_get_goaway_uri(ep, buf, sizeof(buf), NULL), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_get_goaway_uri(ep, NULL, 4, &len), MOQ_ERR_INVAL);
    note(ep, "moqt://next.example/");
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_get_goaway_uri(ep, buf, 4, &len), MOQ_ERR_BUFFER);
    MOQ_TEST_CHECK_EQ_SIZE(len, strlen("moqt://next.example/"));
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_get_goaway_uri(ep, buf, sizeof(buf), &len), MOQ_OK);
    MOQ_TEST_CHECK(len == strlen("moqt://next.example/") &&
                   memcmp(buf, "moqt://next.example/", len) == 0);
    /* One session-wide GOAWAY: a later report does not replace it. */
    note(ep, "moqt://other.example/");
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_get_goaway_uri(ep, buf, sizeof(buf), &len), MOQ_OK);
    MOQ_TEST_CHECK(len == strlen("moqt://next.example/"));
    moq_endpoint_test_free_bare(ep);

    /* An empty URI: reconnect to the current one. */
    ep = moq_endpoint_test_make_bare();
    note(ep, "");
    len = 99;
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_get_goaway_uri(ep, NULL, 0, &len), MOQ_OK);
    MOQ_TEST_CHECK_EQ_SIZE(len, 0);
    moq_endpoint_test_free_bare(ep);
}

static void endpoint_cfg(moq_endpoint_cfg_t *c)
{
    moq_endpoint_cfg_init_sized(c, sizeof(*c));
    c->url = MOQ_BYTES_LITERAL("moqt://relay.example:4443/live");
}

static void test_setup_tokens(void)
{
    moq_endpoint_cfg_t c;
    moq_endpoint_resolved_t r;
    uint8_t v[] = { 1, 2, 3 };
    moq_auth_token_t tokens[17];
    memset(tokens, 0, sizeof(tokens));
    tokens[0] = (moq_auth_token_t){ 7, { v, sizeof(v) } };

    endpoint_cfg(&c);
    moq_result_t base = moq_endpoint_resolve_cfg(&c, &r);
    c.setup_auth_tokens = tokens;
    c.setup_auth_token_count = 1;
    moq_result_t with = moq_endpoint_resolve_cfg(&c, &r);
    MOQ_TEST_CHECK_EQ_INT((int)with, (int)base);
    if (with == MOQ_OK) {
        MOQ_TEST_CHECK(r.setup_auth_tokens == tokens && r.setup_auth_token_count == 1);
    }
    c.setup_auth_token_count = 17;
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), MOQ_ERR_INVAL);
    c.setup_auth_token_count = 1;
    c.setup_auth_tokens = NULL;
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), MOQ_ERR_INVAL);
    c.setup_auth_tokens = tokens;
    tokens[0].token_value = (moq_bytes_t){ NULL, 2 };
    MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), MOQ_ERR_INVAL);
    tokens[0].token_value = (moq_bytes_t){ v, sizeof(v) };

    /* The SETUP carrying them must fit the default 4096-byte session send
     * arena the backends use (and so a default peer's receive buffer): one
     * 4096-byte value does not; two values that each fit alone but not
     * together do not; the largest value that fits beside the URL does. */
    if (base == MOQ_OK) {
        static uint8_t big[4096];
        size_t largest = 4096 - 128 - c.url.len - 32;
        tokens[0].token_value = (moq_bytes_t){ big, sizeof(big) };
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), MOQ_ERR_INVAL);
        tokens[0].token_value = (moq_bytes_t){ big, largest + 1 };
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), MOQ_ERR_INVAL);
        tokens[0].token_value = (moq_bytes_t){ big, largest };
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), MOQ_OK);
        tokens[0].token_value = (moq_bytes_t){ big, 2000 };
        tokens[1] = (moq_auth_token_t){ 8, { big, 2000 } };
        c.setup_auth_token_count = 2;
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), MOQ_ERR_INVAL);
        c.setup_auth_token_count = 1;
        memset(&tokens[1], 0, sizeof(tokens[1]));
        tokens[0].token_value = (moq_bytes_t){ v, sizeof(v) };
    }

    /* A backend that cannot forward them refuses a non-empty list rather
     * than dropping it. */
    endpoint_cfg(&c);
    c.backend = MOQ_TRANSPORT_BACKEND_MSQUIC;
    base = moq_endpoint_resolve_cfg(&c, &r);
    c.setup_auth_tokens = tokens;
    c.setup_auth_token_count = 1;
    /* Without tokens the backend resolves (when built); with them it is
     * refused: the refusal is the tokens', not the backend's absence. */
    if (base == MOQ_OK)
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), MOQ_ERR_UNSUPPORTED);
    else
        MOQ_TEST_CHECK_EQ_INT((int)moq_endpoint_resolve_cfg(&c, &r), (int)base);

    /* A caller whose struct ends before the pair sends none. */
    endpoint_cfg(&c);
    c.struct_size = (uint32_t)offsetof(moq_endpoint_cfg_t, setup_auth_tokens);
    c.setup_auth_tokens = NULL;
    c.setup_auth_token_count = 5;
    if (moq_endpoint_resolve_cfg(&c, &r) == MOQ_OK)
        MOQ_TEST_CHECK_EQ_SIZE(r.setup_auth_token_count, 0);
}

static moq_result_t add(moq_media_sender_t *s, moq_bytes_t props, bool dynamic)
{
    moq_media_track_cfg_t tc;
    moq_media_track_cfg_init_sized(&tc, sizeof(tc));
    tc.name = MOQ_BYTES_LITERAL("video");
    tc.media_type = MOQ_MEDIA_TYPE_VIDEO;
    tc.packaging = MOQ_MEDIA_PACKAGING_RAW;
    tc.codec = MOQ_BYTES_LITERAL("av01");
    tc.bitrate = 1500000;
    tc.is_live = true;
    tc.track_properties = props;
    tc.dynamic_groups = dynamic;
    moq_media_track_t *t = NULL;
    return moq_media_sender_add_track(s, &tc, &t);
}

static void test_add_track_properties(void)
{
    /* An odd-type property whose length runs past the set does not parse
     * on any draft; an even-type property with a value does. */
    static const uint8_t bad[] = { 0x21, 0x05, 'a' };
    static const uint8_t good[] = { 0x20, 0x05 };
    static const uint8_t dyn2[] = { 0x30, 0x02 };
    /* The same inside IMMUTABLE_PROPERTIES (0x0b) where a draft has them. */
    static const uint8_t imm_dyn2[] = { 0x0b, 0x02, 0x30, 0x02 };
    static const uint8_t imm_dyn1[] = { 0x0b, 0x02, 0x30, 0x01 };
    moq_version_t versions[][2] = {
        { MOQ_VERSION_DRAFT_22, 0 },
        { MOQ_VERSION_DRAFT_18, 0 },
        { MOQ_VERSION_DRAFT_16, 0 },
        { MOQ_VERSION_DRAFT_22, MOQ_VERSION_DRAFT_16 },
    };
    size_t counts[] = { 1, 1, 1, 2 };
    for (size_t i = 0; i < 4; i++) {
        moq_endpoint_t *ep = moq_endpoint_test_make_bare();
        moq_endpoint_test_set_offer(ep, versions[i], counts[i]);
        moq_media_sender_t *s = moq_media_sender_test_new();
        moq_media_sender_test_set_ep(s, ep);
        MOQ_TEST_CHECK_EQ_INT((int)add(s, (moq_bytes_t){ bad, sizeof(bad) }, false), MOQ_ERR_INVAL);
        /* DYNAMIC_GROUPS (0x30) allows only 0 and 1 on every draft
         * (draft-16 §11.1.1.3, draft-18 §12.6, draft-22 §10.6): 2 is refused
         * here, not at activation or by the peer. */
        MOQ_TEST_CHECK_EQ_INT((int)add(s, (moq_bytes_t){ dyn2, sizeof(dyn2) }, false),
                              MOQ_ERR_INVAL);
        MOQ_TEST_CHECK_EQ_INT((int)add(s, (moq_bytes_t){ dyn2, sizeof(dyn2) }, true),
                              MOQ_ERR_INVAL);
        MOQ_TEST_CHECK_EQ_INT((int)add(s, (moq_bytes_t){ good, sizeof(good) }, false), MOQ_OK);
        moq_media_sender_test_set_ep(s, NULL);
        moq_media_sender_test_free(s);
        moq_endpoint_test_free_bare(ep);
    }
    /* The library entry the sender uses, on its own: per draft, the checks
     * its encoder and its peer apply. */
    moq_version_t each[] = { MOQ_VERSION_DRAFT_16, MOQ_VERSION_DRAFT_18, MOQ_VERSION_DRAFT_22 };
    for (size_t i = 0; i < 3; i++) {
        MOQ_TEST_CHECK_EQ_INT((int)moq_track_properties_validate(each[i], dyn2, sizeof(dyn2),
                              false), MOQ_ERR_INVAL);
        MOQ_TEST_CHECK_EQ_INT((int)moq_track_properties_validate(each[i], dyn2, 1 + 0, false),
                              MOQ_ERR_INVAL);
        if (each[i] == MOQ_VERSION_DRAFT_16) continue;   /* no Immutable Properties */
        MOQ_TEST_CHECK_EQ_INT((int)moq_track_properties_validate(each[i], imm_dyn2,
                              sizeof(imm_dyn2), false), MOQ_ERR_INVAL);
        MOQ_TEST_CHECK_EQ_INT((int)moq_track_properties_validate(each[i], imm_dyn1,
                              sizeof(imm_dyn1), false), MOQ_OK);
        MOQ_TEST_CHECK_EQ_INT((int)moq_track_properties_validate(each[i], imm_dyn1,
                              sizeof(imm_dyn1), true), MOQ_OK);
    }
    MOQ_TEST_CHECK_EQ_INT((int)moq_track_properties_validate(MOQ_VERSION_DRAFT_22, bad,
                          sizeof(bad), false), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)moq_track_properties_validate(MOQ_VERSION_DRAFT_22, good,
                          sizeof(good), true), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_track_properties_validate(MOQ_VERSION_DRAFT_22, NULL, 0,
                          false), MOQ_OK);
    MOQ_TEST_CHECK_EQ_INT((int)moq_track_properties_validate(MOQ_VERSION_DRAFT_22, NULL, 2,
                          false), MOQ_ERR_INVAL);
    MOQ_TEST_CHECK_EQ_INT((int)moq_track_properties_validate((moq_version_t)0x7f000001, good,
                          sizeof(good), false), MOQ_ERR_UNSUPPORTED);
}

int main(void)
{
    test_goaway_uri();
    test_setup_tokens();
    test_add_track_properties();
    if (failures) { fprintf(stderr, "FAIL: test_service_surfaces (%d)\n", failures); return 1; }
    printf("PASS: test_service_surfaces\n");
    return 0;
}
