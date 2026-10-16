/*
 * What an endpoint test expects the default backend to carry, decided from
 * the provider itself rather than from the resolved offer.
 *
 * A moqt:// or https:// URL with the AUTO backend selects the picoquic
 * family. That backend carries draft-22 only when its picoquic reports exact
 * stream terminal outcomes (PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION >= 1);
 * a build without the picoquic facades has no such provider. Core draft
 * availability is a separate fact: draft-22 stays a supported, declared
 * profile either way.
 */
#ifndef MOQ_SERVICE_TEST_ENDPOINT_OFFER_EXPECT_H
#define MOQ_SERVICE_TEST_ENDPOINT_OFFER_EXPECT_H

#if defined(MOQ_SERVICE_HAVE_PQ_THREADED) || \
    defined(MOQ_SERVICE_HAVE_PICO_WT_MANAGED)
#include <picoquic.h>
#endif

#if defined(PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION) && \
    PICOQUIC_STREAM_TERMINAL_CONTRACT_VERSION >= 1
#define TEST_DEFAULT_BACKEND_HOSTS_D22 1
#else
#define TEST_DEFAULT_BACKEND_HOSTS_D22 0
#endif

#endif
