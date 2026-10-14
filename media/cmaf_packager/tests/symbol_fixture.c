#define PUBLIC __attribute__((visibility("default")))
#define DEFINE(name) PUBLIC void name(void); PUBLIC void name(void) { }
DEFINE(moq_cmaf_packager_cfg_init_sized)
DEFINE(moq_cmaf_packager_create)
DEFINE(moq_cmaf_packager_destroy)
DEFINE(moq_cmaf_packager_init_segment)
DEFINE(moq_cmaf_packager_timescale)
#ifdef HIDE_WRITE
void moq_cmaf_packager_write(void);
void moq_cmaf_packager_write(void) { }
#else
DEFINE(moq_cmaf_packager_write)
#endif
#ifdef EXTRA_EXPORT
DEFINE(accidental_export)
#endif
