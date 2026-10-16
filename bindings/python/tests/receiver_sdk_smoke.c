/* Public SDK authority, never linked into the fake provider. No endpoint or
 * transport is created. Compile against the same SDK as the Python module. */
#include <moq/media_receiver.h>
#include <assert.h>
#include <stdio.h>
#include <string.h>

int main(void)
{
    moq_media_receiver_cfg_t receiver;
    memset(&receiver, 0xa5, sizeof(receiver));
    moq_media_receiver_cfg_init(&receiver);
    assert(receiver.struct_size == offsetof(moq_media_receiver_cfg_t, companion_profile));
    assert(receiver.companion_profile == UINT32_C(0xa5a5a5a5));
    moq_media_receiver_cfg_init_sized(&receiver, sizeof(receiver));
    assert(receiver.struct_size == sizeof(receiver));
    assert(receiver.companion_profile == MOQ_MEDIA_COMPANION_AUTO);
    assert(receiver.time_mode == MOQ_MEDIA_TIME_RAW);
    assert(receiver.overflow.policy == MOQ_MEDIA_OVERFLOW_UNSET);
    puts("receiver sized/default/old-prefix: PASS");

    moq_media_receiver_track_subscribe_cfg_t subscribe;
    moq_media_receiver_track_subscribe_cfg_init_sized(&subscribe, sizeof(subscribe));
    assert(subscribe.struct_size == sizeof(subscribe));
    assert(subscribe.start == MOQ_MEDIA_START_CURRENT && !subscribe.has_priority);
    assert(subscribe.location == NULL && subscribe.fill == NULL);
    moq_location_filter_cfg_t location;
    moq_fill_cfg_t fill;
    moq_location_filter_cfg_init_sized(&location, sizeof(location));
    moq_fill_cfg_init_sized(&fill, sizeof(fill));
    assert(location.struct_size == sizeof(location) && location.kind == MOQ_LOCATION_FILTER_NONE);
    assert(fill.struct_size == sizeof(fill) && !fill.has_subscriber_priority &&
           !fill.has_group_order && !fill.has_fill_timeout && fill.location == NULL);
    puts("subscribe/location/fill sized defaults: PASS");

    assert(moq_media_receiver_cancel_fill(NULL, NULL, 0) == MOQ_ERR_INVAL);
    assert(moq_media_receiver_cancel_fill(NULL, NULL, UINT64_MAX) == MOQ_ERR_INVAL);
    assert(moq_media_receiver_subscribe_track(NULL, NULL, &subscribe) == MOQ_ERR_INVAL);
    puts("service-only commands linked, exact null refusal: PASS");

    moq_media_object_t object;
    memset(&object, 0, sizeof(object));
    object.struct_size = sizeof(object);
    moq_rcbuf_t *kept;
    assert(moq_rcbuf_create(moq_alloc_default(), (const uint8_t *)"track", 5, &kept) == MOQ_OK);
    object.track_properties_ref = moq_rcbuf_incref(kept);
    assert(moq_rcbuf_refcount(kept) == 2);
    moq_media_object_cleanup(&object);
    assert(moq_rcbuf_refcount(kept) == 1);
    assert(object.struct_size == 0 && object.track_properties_ref == NULL);
    moq_media_object_cleanup(&object);
    assert(moq_rcbuf_refcount(kept) == 1);
    moq_rcbuf_decref(kept);
    puts("real stamped track-properties cleanup/idempotence: PASS");
    return 0;
}
