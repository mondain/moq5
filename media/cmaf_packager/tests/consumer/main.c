#include <moq/cmaf_packager.h>
#include <string.h>

int main(void)
{
    const uint8_t asc[] = {0x11, 0x90};
    const uint8_t data[] = {0x11, 0x22};
    moq_cmaf_packager_cfg_t cfg;
    moq_cmaf_packager_cfg_init(&cfg);
    cfg.sample_entry = MOQ_CMAF_ENTRY_MP4A;
    cfg.sample_format = MOQ_CMAF_SAMPLE_AAC_RAW;
    cfg.codec_config.data = asc;
    cfg.codec_config.len = sizeof(asc);
    cfg.samplerate = 48000;
    cfg.channel_count = 2;
    moq_cmaf_packager_t *p = NULL;
    if (moq_cmaf_packager_create(NULL, &cfg, &p) != MOQ_OK) return 1;
    moq_cmaf_packager_sample_t s;
    memset(&s, 0, sizeof(s));
    s.data.data = data;
    s.data.len = sizeof(data);
    s.duration = 1024;
    s.keyframe = true;
    moq_bytes_t out;
    int rc = moq_cmaf_packager_write(p, &s, 1, &out);
    if (rc == MOQ_OK && (!out.data || !out.len)) rc = 1;
    moq_cmaf_packager_destroy(p);
    return rc == MOQ_OK ? 0 : 1;
}
