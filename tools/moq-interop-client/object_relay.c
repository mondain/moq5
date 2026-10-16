#include "object_relay.h"

#include <inttypes.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

size_t orl_payload_len(uint64_t group, uint64_t object)
{
    return 1u + (size_t)(group * ORL_OBJECTS + object) * 131u;
}

uint8_t orl_payload_byte(uint64_t group, uint64_t object, size_t offset)
{
    return (uint8_t)(0xA5u ^ (unsigned)(group * 0x45u) ^
                     (unsigned)(object * 0x1Bu) ^ (unsigned)(offset * 0x3Du) ^
                     (unsigned)(offset >> 8));
}

void orl_payload_fill(uint64_t group, uint64_t object, uint8_t *out)
{
    size_t n = orl_payload_len(group, object);
    for (size_t i = 0; i < n; i++) out[i] = orl_payload_byte(group, object, i);
}

void orl_verifier_init(orl_verifier_t *v)
{
    memset(v, 0, sizeof(*v));
}

static void fail(orl_verifier_t *v, const char *fmt, ...)
{
    if (v->errors++ == 0) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(v->first_error, sizeof(v->first_error), fmt, ap);
        va_end(ap);
    }
}

void orl_verifier_object(orl_verifier_t *v, bool own_track, uint64_t group,
                         uint64_t subgroup, uint64_t object, uint64_t status,
                         const uint8_t *payload, size_t len)
{
    v->objects_received++;
    if (!own_track) {
        fail(v, "object g=%" PRIu64 " o=%" PRIu64 " on another track",
             group, object);
        return;
    }
    if (group >= ORL_GROUPS || subgroup != 0 || object >= ORL_OBJECTS) {
        fail(v, "unexpected object g=%" PRIu64 " sg=%" PRIu64 " o=%" PRIu64,
             group, subgroup, object);
        return;
    }
    if (status != 0) {
        fail(v, "object g=%" PRIu64 " o=%" PRIu64 " status %" PRIu64,
             group, object, status);
        return;
    }
    if (v->done) {
        fail(v, "object g=%" PRIu64 " o=%" PRIu64 " after the done",
             group, object);
        return;
    }
    if (v->seen[group][object]) {
        fail(v, "duplicate object g=%" PRIu64 " o=%" PRIu64, group, object);
        return;
    }
    v->seen[group][object] = true;
    if (v->finished[group]) {
        fail(v, "object g=%" PRIu64 " o=%" PRIu64 " after its subgroup FIN",
             group, object);
        return;
    }
    if (object != v->next_object[group]) {
        fail(v, "object g=%" PRIu64 " o=%" PRIu64 " out of order (expected o=%"
             PRIu64 ")", group, object, v->next_object[group]);
        return;
    }
    v->next_object[group] = object + 1;
    size_t want = orl_payload_len(group, object);
    if (len != want) {
        fail(v, "object g=%" PRIu64 " o=%" PRIu64 " length %zu, expected %zu",
             group, object, len, want);
        return;
    }
    for (size_t i = 0; i < len; i++) {
        if (!payload || payload[i] != orl_payload_byte(group, object, i)) {
            fail(v, "object g=%" PRIu64 " o=%" PRIu64 " byte %zu differs",
                 group, object, i);
            return;
        }
    }
    v->objects_ok++;
    v->bytes_verified += len;
}

void orl_verifier_finished(orl_verifier_t *v, bool own_track, uint64_t group,
                           uint64_t subgroup)
{
    if (!own_track) {
        fail(v, "subgroup FIN g=%" PRIu64 " on another track", group);
        return;
    }
    if (group >= ORL_GROUPS || subgroup != 0) {
        fail(v, "unexpected subgroup FIN g=%" PRIu64 " sg=%" PRIu64,
             group, subgroup);
        return;
    }
    if (v->finished[group]) {
        fail(v, "second FIN for g=%" PRIu64, group);
        return;
    }
    v->finished[group] = true;
    if (v->next_object[group] != ORL_OBJECTS)
        fail(v, "subgroup FIN g=%" PRIu64 " after %" PRIu64 " of %u objects",
             group, v->next_object[group], ORL_OBJECTS);
}

void orl_verifier_error(orl_verifier_t *v, const char *what)
{
    fail(v, "%s", what);
}

void orl_verifier_done(orl_verifier_t *v, uint64_t status_code)
{
    if (v->done) {
        fail(v, "second done");
        return;
    }
    v->done = true;
    v->done_status = status_code;
}

bool orl_verifier_pass(const orl_verifier_t *v, char *msg, size_t msg_len)
{
    if (v->errors) {
        snprintf(msg, msg_len, "%s (%u error(s))", v->first_error, v->errors);
        return false;
    }
    for (unsigned g = 0; g < ORL_GROUPS; g++)
        for (unsigned o = 0; o < ORL_OBJECTS; o++)
            if (!v->seen[g][o]) {
                snprintf(msg, msg_len, "missing object g=%u o=%u", g, o);
                return false;
            }
    for (unsigned g = 0; g < ORL_GROUPS; g++)
        if (!v->finished[g]) {
            snprintf(msg, msg_len, "no subgroup FIN for g=%u", g);
            return false;
        }
    if (!v->done) {
        snprintf(msg, msg_len, "no PUBLISH_DONE after the tail");
        return false;
    }
    if (v->done_status != ORL_DONE_TRACK_ENDED) {
        snprintf(msg, msg_len, "PUBLISH_DONE status 0x%" PRIx64
                 ", expected TRACK_ENDED 0x2", v->done_status);
        return false;
    }
    if (v->objects_ok != ORL_TOTAL || v->objects_received != ORL_TOTAL) {
        snprintf(msg, msg_len, "%zu objects received, %zu valid, expected %u",
                 v->objects_received, v->objects_ok, ORL_TOTAL);
        return false;
    }
    snprintf(msg, msg_len, "%u groups x %u objects, %" PRIu64
             " payload bytes verified, %u FINs, PUBLISH_DONE TRACK_ENDED",
             ORL_GROUPS, ORL_OBJECTS, v->bytes_verified, ORL_GROUPS);
    return true;
}
