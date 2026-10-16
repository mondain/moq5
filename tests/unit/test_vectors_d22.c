/*
 * Draft-22 control-plane vectors authored from the specification text alone
 * (tests/vectors/d22). Every vector must decode to the fields its derivation
 * names and re-encode byte for byte; the vector encoding the refused reading
 * of FILL_PARAMETERS (a nested value without its parameter count) must be
 * refused. A disagreement here is a reading the codec and the vector author
 * resolved differently, and is settled against the text (and, where the text
 * is ambiguous, the selected interpretation the README records), never by
 * editing the vector.
 *
 * Usage: test_vectors_d22 <vector_dir>
 */
#include <moq/moq.h>
#include "test_support.h"
#include "../../core/src/wire/messages_d22_internal.h"
#include <stdio.h>
#include <string.h>
#include <inttypes.h>

#define UNCHECKED_MAX (-2)   /* the field is 2^64-1 */

typedef struct vec {
    const char *file;
    uint64_t    type;
    uint64_t    ctx_request_type;
    uint64_t    ctx_updated_request_type;
    bool        ctx_update_from_subscriber;
    bool        expect_refused;
    int64_t request_id, track_alias, params_count, props_len, ns_count, ns_bytes;
    const char *track_name;
    int64_t code, retry; const char *reason, *redirect_uri, *redirect_track;
    int64_t status_code, stream_count, end_of_track, end_group, end_object;
    int64_t forward, priority, filter_type, sg, so, egd, eo, largest_g, largest_o, expires, include_props;
    int64_t fill_timeout, fill_filter_type, fill_sg;
    const char *uri; int64_t timeout_ms; const char *path, *authority, *impl;
    int64_t max_req_upd, max_filter_ranges, max_cache, options_count;
    bool    expect_fill;     /* FILL_PARAMETERS present with fill_params inside */
    int64_t fill_params;
} vec_t;

static const vec_t vectors[] = {
    { "setup_empty.bin", MOQ_D22_SETUP, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=0 },
    { "setup_max_request_updates.bin", MOQ_D22_SETUP, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=1 },
    { "setup_max_filter_ranges_zero.bin", MOQ_D22_SETUP, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=0,
      .max_cache=-1, .options_count=1 },
    { "setup_path_authority.bin", MOQ_D22_SETUP, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path="/moq", .authority="ex.net",
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=2 },
    { "setup_server_options.bin", MOQ_D22_SETUP, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl="lib", .max_req_upd=1, .max_filter_ranges=-1,
      .max_cache=1024, .options_count=3 },
    { "goaway_empty_uri.bin", MOQ_D22_GOAWAY, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri="", .timeout_ms=0, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "goaway_uri_timeout.bin", MOQ_D22_GOAWAY, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri="x://y", .timeout_ms=5000, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "request_ok_empty.bin", MOQ_D22_REQUEST_OK, MOQ_D22_SUBSCRIBE_NAMESPACE, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=0,
      .props_len=0, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "request_ok_expires.bin", MOQ_D22_REQUEST_OK, MOQ_D22_REQUEST_UPDATE, MOQ_D22_SUBSCRIBE, true, false,
      .request_id=-1, .track_alias=-1, .params_count=1,
      .props_len=0, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=30000, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "request_error_invalid_filter.bin", MOQ_D22_REQUEST_ERROR, MOQ_D22_SUBSCRIBE, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=54, .retry=0, .reason="bad",
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "request_error_redirect.bin", MOQ_D22_REQUEST_ERROR, MOQ_D22_SUBSCRIBE, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=52, .retry=1, .reason="",
      .redirect_uri="", .redirect_track="video",
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "request_update_forward_zero.bin", MOQ_D22_REQUEST_UPDATE, MOQ_D22_SUBSCRIBE, 0, true, false,
      .request_id=4, .track_alias=-1, .params_count=1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=0, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "subscribe_minimal.bin", MOQ_D22_SUBSCRIBE, 0, 0, false, false,
      .request_id=4, .track_alias=-1, .params_count=0,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "subscribe_filter_type_00.bin", MOQ_D22_SUBSCRIBE, 0, 0, false, false,
      .request_id=4, .track_alias=-1, .params_count=3,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=1, .priority=128, .filter_type=0,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "subscribe_filter_type_01.bin", MOQ_D22_SUBSCRIBE, 0, 0, false, false,
      .request_id=4, .track_alias=-1, .params_count=3,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=1, .priority=128, .filter_type=1,
      .sg=1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "subscribe_filter_type_02.bin", MOQ_D22_SUBSCRIBE, 0, 0, false, false,
      .request_id=4, .track_alias=-1, .params_count=3,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=1, .priority=128, .filter_type=2,
      .sg=3, .so=4, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "subscribe_filter_type_03.bin", MOQ_D22_SUBSCRIBE, 0, 0, false, false,
      .request_id=4, .track_alias=-1, .params_count=3,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=1, .priority=128, .filter_type=3,
      .sg=3, .so=4, .egd=2, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "subscribe_filter_type_04.bin", MOQ_D22_SUBSCRIBE, 0, 0, false, false,
      .request_id=4, .track_alias=-1, .params_count=3,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=1, .priority=128, .filter_type=4,
      .sg=3, .so=4, .egd=2, .eo=9,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "subscribe_filter_type_05.bin", MOQ_D22_SUBSCRIBE, 0, 0, false, false,
      .request_id=4, .track_alias=-1, .params_count=3,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=1, .priority=128, .filter_type=5,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "subscribe_fill_parameters.bin", MOQ_D22_SUBSCRIBE, 0, 0, false, false,
      .request_id=4, .track_alias=-1, .params_count=2,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=5,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=250, .fill_filter_type=1, .fill_sg=1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1, .expect_fill=true, .fill_params=2 },
    { "subscribe_fill_parameters_empty.bin", MOQ_D22_SUBSCRIBE, 0, 0, false, false,
      .request_id=4, .track_alias=-1, .params_count=2,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=5,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1, .expect_fill=true, .fill_params=0 },
    { "subscribe_fill_parameters_alt_uncounted.bin", MOQ_D22_SUBSCRIBE, 0, 0, false, true,
      .request_id=4, .track_alias=-1, .params_count=2,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=5,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=250, .fill_filter_type=1, .fill_sg=1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "subscribe_ok_largest_object.bin", MOQ_D22_SUBSCRIBE_OK, 0, 0, false, false,
      .request_id=-1, .track_alias=7, .params_count=1,
      .props_len=0, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=2, .largest_o=5, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "publish_minimal.bin", MOQ_D22_PUBLISH, 0, 0, false, false,
      .request_id=1, .track_alias=7, .params_count=0,
      .props_len=0, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "publish_largest_object_props.bin", MOQ_D22_PUBLISH, 0, 0, false, false,
      .request_id=1, .track_alias=7, .params_count=2,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=2, .largest_o=5, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "publish_done_stream_count_max.bin", MOQ_D22_PUBLISH_DONE, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason="ended",
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=2, .stream_count=UNCHECKED_MAX,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "publish_state_notify_largest_object.bin", MOQ_D22_PUBLISH_STATE_NOTIFY, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=10, .largest_o=20, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "publish_state_notify_paused.bin", MOQ_D22_PUBLISH_STATE_NOTIFY, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=2,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=0, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=10, .largest_o=20, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "fetch_filter_type_04.bin", MOQ_D22_FETCH, 0, 0, false, false,
      .request_id=6, .track_alias=-1, .params_count=1,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=4,
      .sg=3, .so=4, .egd=2, .eo=9,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "fetch_ok_end_location.bin", MOQ_D22_FETCH_OK, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=0,
      .props_len=0, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=0, .end_group=5, .end_object=9,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "fetch_ok_end_of_track_props.bin", MOQ_D22_FETCH_OK, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=0,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=1, .end_group=5, .end_object=9,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "track_status.bin", MOQ_D22_TRACK_STATUS, 0, 0, false, false,
      .request_id=8, .track_alias=-1, .params_count=1,
      .props_len=-1, .ns_count=2, .ns_bytes=5,
      .track_name="video", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "track_status_ok.bin", MOQ_D22_REQUEST_OK, MOQ_D22_TRACK_STATUS, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=1,
      .props_len=-1, .ns_count=-1, .ns_bytes=-1,
      .track_name=NULL, .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=10, .largest_o=20, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
    { "publish_skipped.bin", MOQ_D22_PUBLISH_SKIPPED, 0, 0, false, false,
      .request_id=-1, .track_alias=-1, .params_count=-1,
      .props_len=-1, .ns_count=1, .ns_bytes=1,
      .track_name="x", .code=-1, .retry=-1, .reason=NULL,
      .redirect_uri=NULL, .redirect_track=NULL,
      .status_code=-1, .stream_count=-1,
      .end_of_track=-1, .end_group=-1, .end_object=-1,
      .forward=-1, .priority=-1, .filter_type=-1,
      .sg=-1, .so=-1, .egd=-1, .eo=-1,
      .largest_g=-1, .largest_o=-1, .expires=-1, .include_props=-1,
      .fill_timeout=-1, .fill_filter_type=-1, .fill_sg=-1,
      .uri=NULL, .timeout_ms=-1, .path=NULL, .authority=NULL,
      .impl=NULL, .max_req_upd=-1, .max_filter_ranges=-1,
      .max_cache=-1, .options_count=-1 },
};

static int read_file(const char *dir, const char *name, uint8_t *buf, size_t cap, size_t *len)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/%s", dir, name);
    FILE *f = fopen(path, "rb");
    if (!f) { fprintf(stderr, "cannot open %s\n", path); return -1; }
    *len = fread(buf, 1, cap, f);
    fclose(f);
    return 0;
}

static bool want(int64_t v) { return v != -1; }
static uint64_t as_u64(int64_t v) { return v == UNCHECKED_MAX ? UINT64_MAX : (uint64_t)v; }
#define EQ_FIELD(name, actual) do { if (want(v->name)) { \
    if ((uint64_t)(actual) != as_u64(v->name)) { \
        fprintf(stderr, "FAIL %s: " #name " = %" PRIu64 ", vector says %" PRIu64 "\n", v->file, (uint64_t)(actual), as_u64(v->name)); failures++; } } } while (0)
#define EQ_STR(name, span) do { if (v->name) { \
    if ((span).len != strlen(v->name) || memcmp((span).data, v->name, (span).len) != 0) { \
        fprintf(stderr, "FAIL %s: " #name " mismatch\n", v->file); failures++; } } } while (0)

/* Walk a decoded parameter sequence into an array (for re-encoding) and
 * check the scalar/location/filter values the vector names. */
static int check_params(const vec_t *v, const moq_d22_params_t *ps, moq_d22_param_t *arr, size_t cap, size_t *n_out)
{
    int failures = 0;
    moq_buf_reader_t r = { ps->encoded.data, ps->encoded.len, 0 };
    uint64_t prev = 0; size_t n = 0;
    bool saw_filter = false, saw_fill = false;
    while (moq_buf_reader_remaining(&r) > 0) {
        moq_d22_param_t p; memset(&p, 0, sizeof(p));
        moq_result_t rc = moq_d22_param_next_scanned(&r, &prev, &p);
        if (rc != MOQ_OK) { fprintf(stderr, "FAIL %s: param walk rc=%d\n", v->file, (int)rc); return failures + 1; }
        if (n < cap) arr[n] = p;
        n++;
        switch (p.type) {
        case 0x10: EQ_FIELD(forward, p.value.integer); break;
        case 0x20: EQ_FIELD(priority, p.value.integer); break;
        case 0x08: EQ_FIELD(expires, p.value.integer); break;
        case 0x35: EQ_FIELD(include_props, p.value.integer); break;
        case 0x09: EQ_FIELD(largest_g, p.value.location.group); EQ_FIELD(largest_o, p.value.location.object); break;
        case 0x21:
            saw_filter = true;
            EQ_FIELD(filter_type, p.value.filter.type);
            EQ_FIELD(sg, p.value.filter.start_group); EQ_FIELD(so, p.value.filter.start_object);
            EQ_FIELD(egd, p.value.filter.end_group_delta); EQ_FIELD(eo, p.value.filter.end_object);
            break;
        case 0x23: {
            saw_fill = true;
            moq_d22_params_t inner;
            moq_result_t frc = moq_d22_decode_fill(p.value.fill, 0, &inner);
            if (frc != MOQ_OK) { fprintf(stderr, "FAIL %s: fill decode rc=%d\n", v->file, (int)frc); failures++; break; }
            if (v->expect_fill && inner.count != (uint64_t)v->fill_params) {
                fprintf(stderr, "FAIL %s: fill holds %" PRIu64 " parameters, vector says %" PRId64 "\n",
                        v->file, inner.count, v->fill_params);
                failures++;
            }
            moq_buf_reader_t ir = { inner.encoded.data, inner.encoded.len, 0 };
            uint64_t iprev = 0;
            while (moq_buf_reader_remaining(&ir) > 0) {
                moq_d22_param_t ip; memset(&ip, 0, sizeof(ip));
                if (moq_d22_param_next_scanned(&ir, &iprev, &ip) != MOQ_OK) { failures++; break; }
                if (ip.type == 0x0a) EQ_FIELD(fill_timeout, ip.value.integer);
                if (ip.type == 0x21) { EQ_FIELD(fill_filter_type, ip.value.filter.type); EQ_FIELD(fill_sg, ip.value.filter.start_group); }
            }
            break;
        }
        default: break;
        }
    }
    if (want(v->filter_type) && !saw_filter && !want(v->fill_filter_type)) { fprintf(stderr, "FAIL %s: no location filter decoded\n", v->file); failures++; }
    if ((want(v->fill_timeout) || v->expect_fill) && !saw_fill) { fprintf(stderr, "FAIL %s: no fill parameters decoded\n", v->file); failures++; }
    *n_out = n;
    return failures;
}

static int run_vector(const char *dir, const vec_t *v)
{
    int failures = 0;
    uint8_t bytes[4096]; size_t len = 0;
    if (read_file(dir, v->file, bytes, sizeof(bytes), &len) < 0) return 1;

    moq_buf_reader_t r = { bytes, len, 0 };
    moq_control_envelope_t env;
    moq_result_t rc = moq_d22_decode_envelope(&r, &env);
    if (rc != MOQ_OK) { fprintf(stderr, "FAIL %s: envelope rc=%d\n", v->file, (int)rc); return 1; }
    if (env.msg_type != v->type) { fprintf(stderr, "FAIL %s: type 0x%" PRIx64 "\n", v->file, env.msg_type); failures++; }
    if (moq_buf_reader_offset(&r) != len) { fprintf(stderr, "FAIL %s: envelope consumed %zu of %zu\n", v->file, moq_buf_reader_offset(&r), len); failures++; }

    /* Every strict prefix of the whole message is incomplete, never malformed. */
    for (size_t cut = 0; cut < len; cut++) {
        moq_buf_reader_t pr = { bytes, cut, 0 };
        moq_control_envelope_t penv;
        if (moq_d22_decode_envelope(&pr, &penv) != MOQ_ERR_BUFFER || pr.pos != 0) {
            fprintf(stderr, "FAIL %s: prefix %zu not reported incomplete\n", v->file, cut); failures++; break;
        }
    }

    uint8_t out[4096]; moq_buf_writer_t w; moq_buf_writer_init(&w, out, sizeof(out));

    if (v->type == MOQ_D22_SETUP) {
        moq_d22_setup_options_t so;
        rc = moq_d22_decode_setup_options(env.payload, env.payload_len, &so);
        if (rc != MOQ_OK) { fprintf(stderr, "FAIL %s: setup rc=%d\n", v->file, (int)rc); return failures + 1; }
        if (want(v->max_req_upd)) { if (!so.has_max_request_updates) failures++; EQ_FIELD(max_req_upd, so.max_request_updates); }
        if (want(v->max_filter_ranges)) { if (!so.has_max_filter_ranges) failures++; EQ_FIELD(max_filter_ranges, so.max_filter_ranges); }
        if (want(v->max_cache)) { if (!so.has_max_auth_token_cache_size) failures++; EQ_FIELD(max_cache, so.max_auth_token_cache_size); }
        if (v->path) { if (!so.has_path) failures++; EQ_STR(path, so.path); }
        if (v->authority) { if (!so.has_authority) failures++; EQ_STR(authority, so.authority); }
        if (v->impl) { if (!so.has_implementation) failures++; EQ_STR(impl, so.implementation); }
        /* Re-encode from the validated option list. */
        moq_d22_kvp_t kv[16]; size_t n = 0; uint64_t prev = 0;
        moq_buf_reader_t kr = { so.encoded.data, so.encoded.len, 0 };
        while (moq_buf_reader_remaining(&kr) > 0 && n < 16) {
            if (moq_d22_decode_kvp(&kr, &prev, &kv[n]) != MOQ_OK) { failures++; break; }
            n++;
        }
        EQ_FIELD(options_count, n);
        if (moq_d22_encode_setup(&w, kv, n) != MOQ_OK) { fprintf(stderr, "FAIL %s: setup encode\n", v->file); return failures + 1; }
    } else if (v->type == MOQ_D22_GOAWAY) {
        moq_d22_goaway_t ga;
        rc = moq_d22_decode_goaway(env.payload, env.payload_len, &ga);
        if (rc != MOQ_OK) { fprintf(stderr, "FAIL %s: goaway rc=%d\n", v->file, (int)rc); return failures + 1; }
        EQ_STR(uri, ga.uri); EQ_FIELD(timeout_ms, ga.timeout_ms);
        if (moq_d22_encode_goaway(&w, &ga) != MOQ_OK) { fprintf(stderr, "FAIL %s: goaway encode\n", v->file); return failures + 1; }
    } else {
        moq_d22_message_context_t ctx = {
            .request_type = v->ctx_request_type,
            .updated_request_type = v->ctx_updated_request_type,
            .update_from_subscriber = v->ctx_update_from_subscriber,
            .max_filter_ranges = 0,
        };
        moq_d22_message_t msg; memset(&msg, 0, sizeof(msg));
        rc = moq_d22_decode_message(env.msg_type, env.payload, env.payload_len, &ctx, &msg);
        if (v->expect_refused) {
            if (rc != MOQ_ERR_PROTO) { fprintf(stderr, "FAIL %s: refused reading gave rc=%d, not PROTOCOL_VIOLATION\n", v->file, (int)rc); failures++; }
            return failures;
        }
        if (rc != MOQ_OK) { fprintf(stderr, "FAIL %s: message rc=%d\n", v->file, (int)rc); return failures + 1; }
        EQ_FIELD(request_id, msg.request_id);
        EQ_FIELD(track_alias, msg.track_alias);
        EQ_FIELD(params_count, msg.params.count);
        EQ_FIELD(props_len, msg.properties.len);
        EQ_FIELD(ns_count, msg.ns.count);
        EQ_FIELD(ns_bytes, msg.ns.value_bytes);
        EQ_STR(track_name, msg.track_name);
        EQ_FIELD(code, msg.code); EQ_FIELD(retry, msg.retry_interval); EQ_STR(reason, msg.reason);
        EQ_STR(redirect_uri, msg.redirect_uri);
        EQ_FIELD(status_code, msg.code); EQ_FIELD(stream_count, msg.stream_count);
        EQ_FIELD(end_of_track, msg.end_of_track);
        EQ_FIELD(end_group, msg.end_location.group); EQ_FIELD(end_object, msg.end_location.object);
        moq_d22_param_t arr[16]; size_t n = 0;
        failures += check_params(v, &msg.params, arr, 16, &n);
        if (moq_d22_encode_message(&w, &msg, arr, n, &ctx) != MOQ_OK) {
            fprintf(stderr, "FAIL %s: re-encode refused\n", v->file); return failures + 1;
        }
    }
    size_t wl = moq_buf_writer_offset(&w);
    if (wl != len || memcmp(out, bytes, len) != 0) {
        fprintf(stderr, "FAIL %s: re-encode differs (%zu vs %zu bytes)\n", v->file, wl, len);
        fprintf(stderr, "  want:"); for (size_t i = 0; i < len; i++) fprintf(stderr, " %02x", bytes[i]); fprintf(stderr, "\n");
        fprintf(stderr, "  got: "); for (size_t i = 0; i < wl; i++) fprintf(stderr, " %02x", out[i]); fprintf(stderr, "\n");
        failures++;
    }
    return failures;
}

int main(int argc, char **argv)
{
    const char *dir = argc > 1 ? argv[1] : "tests/vectors/d22";
    int failures = 0;
    size_t n = sizeof(vectors) / sizeof(vectors[0]);
    for (size_t i = 0; i < n; i++) failures += run_vector(dir, &vectors[i]);
    if (failures == 0) printf("PASS: vectors_d22 (%zu vectors)\n", n);
    else printf("FAIL: vectors_d22 (%d failures)\n", failures);
    return failures != 0;
}
