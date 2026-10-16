/* Core's configuration path must not require the optional log-conversion
 * library. In particular picoquic_set_qlog belongs to the core link closure. */
#include <picoquic.h>
#include <picoquic_config.h>
#include <stdio.h>

int main(void)
{
    picoquic_quic_config_t cfg;
    picoquic_config_init(&cfg);
    cfg.nb_connections = 1;
    uint64_t now = 0;
    picoquic_quic_t *quic = picoquic_create_and_configure(
        &cfg, NULL, NULL, now, &now);
    if (quic == NULL) {
        picoquic_config_clear(&cfg);
        fputs("core-only configuration failed\n", stderr);
        return 1;
    }
    picoquic_free(quic);
    picoquic_config_clear(&cfg);
    puts("PASS: picoquic core-only configuration");
    return 0;
}
