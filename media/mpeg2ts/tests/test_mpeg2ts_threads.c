/*
 * Concurrent first use of moq-mpeg2ts in a fresh process: independent
 * threads parsing their own PATs, each checked against its CRC_32, share no
 * writable data. Run under ThreadSanitizer to check the absence of data races; the
 * results are checked in every build.
 */

#include <moq/mpeg2ts.h>

#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "ts_builders.h"

#define THREADS 8

static bool     pat_ok[THREADS];
static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  cv = PTHREAD_COND_INITIALIZER;
static int             ready, go;

/* Hold every thread until all have started, then release them together. */
static void start_together(void)
{
    pthread_mutex_lock(&mu);
    ready++;
    pthread_cond_broadcast(&cv);
    while (!go) pthread_cond_wait(&cv, &mu);
    pthread_mutex_unlock(&mu);
}

static void *worker(void *arg)
{
    size_t i = (size_t)arg;
    uint8_t sec[64];
    tb_prog_t tp[] = { { (uint16_t)(i + 1), (uint16_t)(0x100 + i) } };
    size_t n = tb_pat(sec, 1, (uint8_t)i, tp, 1);
    start_together();
    moq_mpeg2ts_program_t progs[1];
    moq_mpeg2ts_pat_t pat;
    moq_mpeg2ts_pat_init(&pat, progs, 1);
    moq_bytes_t b = { sec, n };
    pat_ok[i] = moq_mpeg2ts_parse_pat(b, &pat) == MOQ_OK &&
                progs[0].pmt_pid == 0x100 + i;
    return NULL;
}

int main(void)
{
    pthread_t t[THREADS];
    for (size_t i = 0; i < THREADS; i++)
        pthread_create(&t[i], NULL, worker, (void *)i);
    pthread_mutex_lock(&mu);
    while (ready < THREADS) pthread_cond_wait(&cv, &mu);
    go = 1;
    pthread_cond_broadcast(&cv);
    pthread_mutex_unlock(&mu);
    for (size_t i = 0; i < THREADS; i++) pthread_join(t[i], NULL);
    int bad = 0;
    for (size_t i = 0; i < THREADS; i++) {
        if (!pat_ok[i]) {
            fprintf(stderr, "FAIL: thread %zu\n", i);
            bad++;
        }
    }
    if (bad) return 1;
    printf("mpeg2ts threads: all tests passed\n");
    return 0;
}
