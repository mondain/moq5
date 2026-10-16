/*
 * object_relay -- the expected Object sequence and its verifier for the
 * interop client's `object-relay` case: opaque object conformance through a
 * relay. The payload bytes are opaque; this checks transport delivery of a
 * deterministic sequence, not media content.
 *
 * The sequence is fixed and derived only from (group, object): ORL_GROUPS
 * groups, each one subgroup (subgroup 0) of ORL_OBJECTS objects with IDs
 * 0..ORL_OBJECTS-1. Every object has a distinct nonzero payload length and
 * bytes that are a function of (group, object, offset), so a received value
 * can only match if it is the one that was sent.
 *
 * Pure: no I/O, no allocation, no clock. The case feeds it every relevant
 * subscriber event in arrival order and asks for a verdict at the end.
 */
#ifndef MOQ_INTEROP_OBJECT_RELAY_H
#define MOQ_INTEROP_OBJECT_RELAY_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#define ORL_GROUPS  3u
#define ORL_OBJECTS 4u
#define ORL_TOTAL   (ORL_GROUPS * ORL_OBJECTS)
/* The longest payload the sequence uses (orl_payload_len). */
#define ORL_MAX_PAYLOAD 1600u

/* PUBLISH_DONE status TRACK_ENDED: draft-18 Section 10.11 / 15.10.3 and
 * draft-22 Section 9.9 / 16.11.3 both assign it 0x2. */
#define ORL_DONE_TRACK_ENDED 0x2u

/* Payload length of object (group, object): distinct and nonzero for every
 * object of the sequence. */
size_t  orl_payload_len(uint64_t group, uint64_t object);
/* Byte `offset` of the payload of object (group, object). */
uint8_t orl_payload_byte(uint64_t group, uint64_t object, size_t offset);
/* Fill `out` (at least orl_payload_len bytes) with the expected payload. */
void    orl_payload_fill(uint64_t group, uint64_t object, uint8_t *out);

typedef struct orl_verifier {
    bool     seen[ORL_GROUPS][ORL_OBJECTS];
    uint64_t next_object[ORL_GROUPS];  /* in-order arrival within a subgroup */
    bool     finished[ORL_GROUPS];
    bool     done;
    uint64_t done_status;
    size_t   objects_ok;               /* objects matching the sequence */
    size_t   objects_received;         /* every object event, valid or not */
    uint64_t bytes_verified;
    unsigned errors;
    char     first_error[200];
} orl_verifier_t;

void orl_verifier_init(orl_verifier_t *v);

/* One received object. `own_track` is false when the event did not belong to
 * the case's own subscription. A non-normal status, an identity outside the
 * sequence, a duplicate, an out-of-order object within its subgroup, an
 * object after its subgroup's FIN or after the subscription's done, a wrong
 * length or any wrong byte is an error. */
void orl_verifier_object(orl_verifier_t *v, bool own_track, uint64_t group,
                         uint64_t subgroup, uint64_t object, uint64_t status,
                         const uint8_t *payload, size_t len);

/* The subgroup stream of `group` ended with a FIN. An error unless it is a
 * subgroup of the sequence, not yet finished, and every object of the group
 * was already received. */
void orl_verifier_finished(orl_verifier_t *v, bool own_track, uint64_t group,
                           uint64_t subgroup);

/* The subscription ended (PUBLISH_DONE). An error if it was already ended. */
void orl_verifier_done(orl_verifier_t *v, uint64_t status_code);

/* Any other observed deviation (a reset subgroup, a reset object, an
 * oversized object): recorded as an error. */
void orl_verifier_error(orl_verifier_t *v, const char *what);

/* Final verdict: true only with no error, every object of the sequence
 * received exactly once, every subgroup finished, and the subscription ended
 * with TRACK_ENDED after all of them. `msg` gets the reason on failure. */
bool orl_verifier_pass(const orl_verifier_t *v, char *msg, size_t msg_len);

#endif
