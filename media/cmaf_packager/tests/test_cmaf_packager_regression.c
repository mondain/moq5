#include <moq/cmaf_packager.h>
#include <stdio.h>
#include <string.h>

static int failures;
#define CHECK(x) do { if (!(x)) { \
    fprintf(stderr, "FAIL:%d: %s\n", __LINE__, #x); failures++; } } while (0)

static const uint8_t avcc[] = {
    1, 0x64, 0, 0x1f, 0xfc, 0xe1, 0, 4, 0x67, 0x64, 0, 0x1f,
    1, 0, 2, 0x68, 0xee
};
static moq_cmaf_packager_cfg_t video(void)
{
    moq_cmaf_packager_cfg_t c;
    moq_cmaf_packager_cfg_init(&c);
#ifdef MOQ_PACKAGER_ORIGINAL_REVIEW
    c.codec_kind = MOQ_CMAF_CODEC_AVC;
    c.default_sample_duration = 3000;
#else
    c.sample_entry = MOQ_CMAF_ENTRY_AVC1;
    c.sample_format = MOQ_CMAF_SAMPLE_NAL_ANNEXB;
#endif
    c.codec_config = (moq_bytes_t){ avcc, sizeof(avcc) };
    c.width = 640; c.height = 480; c.timescale = 90000;
    return c;
}
static void regressions(void)
{
    moq_cmaf_packager_cfg_t c = video();
    moq_cmaf_packager_t *p = NULL;
    CHECK(moq_cmaf_packager_create(NULL, &c, &p) == MOQ_OK);
    if (!p) return;
    uint8_t nal[260];
    memset(nal, 0x65, sizeof(nal));
    nal[0] = nal[1] = nal[2] = 0; nal[3] = 1;
    moq_cmaf_packager_sample_t s = {
        .data = { nal, sizeof(nal) }, .duration = 3000, .keyframe = true
    };
    moq_bytes_t out = { NULL, 0 };
    CHECK(moq_cmaf_packager_write(p, &s, 1, &out) == MOQ_ERR_UNSUPPORTED);
    CHECK(out.data == NULL && out.len == 0);
    moq_cmaf_packager_destroy(p);

    uint8_t invalid = 0x81;
    c = video();
#ifdef MOQ_PACKAGER_ORIGINAL_REVIEW
    c.codec_kind = MOQ_CMAF_CODEC_AV1;
#else
    c.sample_entry = MOQ_CMAF_ENTRY_AV01;
    c.sample_format = MOQ_CMAF_SAMPLE_AV1_OBU;
#endif
    c.codec_config = (moq_bytes_t){ &invalid, 1 };
    CHECK(moq_cmaf_packager_create(NULL, &c, &p) == MOQ_ERR_PROTO);
    if (p) moq_cmaf_packager_destroy(p);

    c = video();
    p = NULL;
    CHECK(moq_cmaf_packager_create(NULL, &c, &p) == MOQ_OK);
    if (!p) return;
    const uint8_t small[] = { 0, 0, 1, 0x65 };
    moq_cmaf_packager_sample_t pair[] = {
        { .data = {small, sizeof(small)}, .duration = 3000, .keyframe = true },
        { .data = {small, sizeof(small)}, .pts = 6000, .dts = 6000,
          .duration = 3000, .keyframe = true }
    };
    CHECK(moq_cmaf_packager_write(p, pair, 2, &out) == MOQ_ERR_INVAL);
    CHECK(out.data == NULL && out.len == 0);
    pair[0].pts = pair[0].dts = 3000;
    pair[1].pts = pair[1].dts = 0;
    CHECK(moq_cmaf_packager_write(p, pair, 2, &out) == MOQ_ERR_INVAL);
    out = (moq_bytes_t){ small, sizeof(small) };
    CHECK(moq_cmaf_packager_write(NULL, pair, 2, &out) == MOQ_ERR_INVAL);
    CHECK(out.data == NULL && out.len == 0);
    moq_cmaf_packager_destroy(p);
}
int main(void)
{
    regressions();
    printf("packager regression failures=%d\n", failures);
    return failures ? 1 : 0;
}
