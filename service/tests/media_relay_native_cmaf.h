#ifndef MOQ_TEST_MEDIA_RELAY_NATIVE_CMAF_H
#define MOQ_TEST_MEDIA_RELAY_NATIVE_CMAF_H

#include "../../media/cmaf/tests/ffmpeg_aac_stereo_init.h"

/* Reuse the independent FFmpeg init bytes above. This two-sample vector uses
 * the layout exercised by test_media_receiver_origin.c and test_cmaf.c, with
 * the mfhd required by strict sender validation, an explicit moof-relative
 * data offset, and duration/size/flags/CTO for every sample. The sample bytes
 * are synthetic container-test payloads, not an AAC decoder conformance test.
 * No production encoder or parser constructs the expected values. */
#define NATIVE_CMAF_FRAGMENT_SIZE 132u
#define NATIVE_CMAF_MDAT_OFFSET 128u

static const uint8_t native_cmaf_fragment_template[] = {
    0,0,0,120,'m','o','o','f',
    0,0,0,16,'m','f','h','d', 0,0,0,0, 0,0,0,1,
    0,0,0,96,'t','r','a','f',
    0,0,0,16,'t','f','h','d', 0,2,0,0, 0,0,0,1,
    0,0,0,20,'t','f','d','t', 1,0,0,0, 0,0,0,0, 0,0,0xbb,0x80,
    0,0,0,52,'t','r','u','n', 1,0,0x0f,1, 0,0,0,2, 0,0,0,128,
    0,0,4,0, 0,0,0,2, 2,0,0,0, 0,0,0,0,
    0,0,4,0, 0,0,0,2, 2,0,0,0, 0,0,0,0,
    0,0,0,12,'m','d','a','t', 'A','B','C','D'
};

/* Track 1, timescale 48000; four consecutive two-sample fragments, each
 * sample 1024 ticks. Integer microsecond conversion truncates, not rounds. */
static const uint8_t native_cmaf_decode_words[4][4] = {
    {0, 0, 0xbb, 0x80}, /* 48000 */
    {0, 0, 0xc3, 0x80}, /* 50048 */
    {0, 0, 0xcb, 0x80}, /* 52096 */
    {0, 0, 0xd3, 0x80}  /* 54144 */
};
static const uint64_t native_cmaf_decode_us[4] = {
    UINT64_C(1000000), UINT64_C(1042666),
    UINT64_C(1085333), UINT64_C(1128000)
};
static const uint8_t native_cmaf_sample_bytes[4][4] = {
    {'A', 'B', 'C', 'D'}, {'E', 'F', 'G', 'H'},
    {'I', 'J', 'K', 'L'}, {'M', 'N', 'O', 'P'}
};
static const uint8_t native_cmaf_asc[] = {0x11, 0x90, 0x56, 0xe5, 0x00};

_Static_assert(sizeof(native_cmaf_fragment_template) == NATIVE_CMAF_FRAGMENT_SIZE,
               "native CMAF fragment extent");
_Static_assert(sizeof(k_ffmpeg_aac_stereo_init) == 733,
               "retained FFmpeg init extent");

#endif /* MOQ_TEST_MEDIA_RELAY_NATIVE_CMAF_H */
