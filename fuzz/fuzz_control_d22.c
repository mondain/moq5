/*
 * Draft-22 control codec fuzz target: envelope, SETUP options, GOAWAY, and
 * every request-stream message under each response/update context.
 */
#include "../core/src/wire/messages_d22_internal.h"
#include <stdint.h>
#include <stddef.h>
#include <string.h>

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    moq_buf_reader_t r;
    moq_buf_reader_init(&r, data, size);
    moq_control_envelope_t env;
    if (moq_d22_decode_envelope(&r, &env) < 0)
        return 0;
    if (env.msg_type == MOQ_D22_SETUP) {
        moq_d22_setup_options_t so;
        if (moq_d22_decode_setup_options(env.payload, env.payload_len, &so) == MOQ_OK) {
            moq_buf_reader_t kr; moq_buf_reader_init(&kr, so.encoded.data, so.encoded.len);
            uint64_t prev = 0; moq_d22_kvp_t kv;
            while (moq_buf_reader_remaining(&kr) > 0 && moq_d22_decode_kvp(&kr, &prev, &kv) == MOQ_OK) {}
        }
        return 0;
    }
    if (env.msg_type == MOQ_D22_GOAWAY) {
        moq_d22_goaway_t ga;
        (void)moq_d22_decode_goaway(env.payload, env.payload_len, &ga);
        return 0;
    }
    static const uint64_t request_types[] = {
        0, MOQ_D22_SUBSCRIBE, MOQ_D22_PUBLISH, MOQ_D22_FETCH, MOQ_D22_TRACK_STATUS,
        MOQ_D22_PUBLISH_NAMESPACE, MOQ_D22_SUBSCRIBE_NAMESPACE, MOQ_D22_SUBSCRIBE_TRACKS,
        MOQ_D22_REQUEST_UPDATE,
    };
    for (size_t i = 0; i < sizeof(request_types) / sizeof(request_types[0]); i++) {
        for (int from_sub = 0; from_sub < 2; from_sub++) {
            moq_d22_message_context_t ctx = {
                .request_type = request_types[i],
                .updated_request_type = MOQ_D22_SUBSCRIBE,
                .update_from_subscriber = from_sub != 0,
                .max_filter_ranges = (size & 1) ? 4 : 0,
            };
            moq_d22_message_t msg;
            memset(&msg, 0, sizeof(msg));
            if (moq_d22_decode_message(env.msg_type, env.payload, env.payload_len, &ctx, &msg) != MOQ_OK)
                continue;
            /* Walk every parameter the decoder accepted. */
            moq_buf_reader_t pr; moq_buf_reader_init(&pr, msg.params.encoded.data, msg.params.encoded.len);
            uint64_t prev = 0; moq_d22_param_t p;
            while (moq_buf_reader_remaining(&pr) > 0 &&
                   moq_d22_param_next_scanned(&pr, &prev, &p) == MOQ_OK) {
                if (p.type == 0x23) {
                    moq_d22_params_t inner;
                    (void)moq_d22_decode_fill(p.value.fill, ctx.max_filter_ranges, &inner);
                }
            }
        }
    }
    return 0;
}
