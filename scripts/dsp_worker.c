#define _GNU_SOURCE
#include <pthread.h>
#include <string.h>
#include <sched.h>
#include "include/dsp_worker.h"
#include "include/engine_globals.h"
#include "include/dsp_neon.h"
#include "include/dsp_channel.h"

/* ── 병렬 DSP 워커 전역 ─────────────────────────────────────────── */
WorkerArg         g_worker_arg[DSP_WORKER_COUNT];
pthread_t         g_worker_tid[DSP_WORKER_COUNT];
pthread_barrier_t g_barrier_work_start;
pthread_barrier_t g_barrier_input_done;
pthread_barrier_t g_barrier_routing_done;
pthread_barrier_t g_barrier_work_done;
volatile int      g_worker_quit = 0;

/* ── 공유 DSP 처리 함수 (aoip_engine.c에 선언, 여기서 extern 참조) */
extern void process_channel_dsp_in (int ch_start, int ch_count);
extern void process_channel_dsp_out(int ch_start, int ch_count);
extern void process_routing         (int out_start, int out_count, int n_in, int n_out);

void *dsp_worker_thread(void *arg)
{
    WorkerArg *w = (WorkerArg *)arg;

    struct sched_param sp = { .sched_priority = g_prio_dsp };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    pin_to_cpu(2);

    volatile char stack_touch[4096];
    memset((void *)stack_touch, 0, sizeof(stack_touch));

    while (1) {
        pthread_barrier_wait(&g_barrier_work_start);
        if (g_worker_quit) {
            pthread_barrier_wait(&g_barrier_input_done);
            pthread_barrier_wait(&g_barrier_routing_done);
            pthread_barrier_wait(&g_barrier_work_done);
            break;
        }
        process_channel_dsp_in(w->ch_start, w->ch_count);
        pthread_barrier_wait(&g_barrier_input_done);
        process_routing(w->ch_start, w->ch_count, w->n_in, w->n_out);
        pthread_barrier_wait(&g_barrier_routing_done);
        process_channel_dsp_out(w->ch_start, w->ch_count);
        pthread_barrier_wait(&g_barrier_work_done);
    }
    return NULL;
}

void dsp_run_parallel(void)
{
    for (int w = 0; w < DSP_WORKER_COUNT; w++) {
        g_worker_arg[w].n_in  = g_n_in;
        g_worker_arg[w].n_out = g_n_out;
    }
    pthread_barrier_wait(&g_barrier_work_start);
    process_channel_dsp_in(0, DSP_WORKER_CH);
    pthread_barrier_wait(&g_barrier_input_done);
    process_routing(0, DSP_WORKER_CH, g_n_in, g_n_out);
    pthread_barrier_wait(&g_barrier_routing_done);
    process_channel_dsp_out(0, DSP_WORKER_CH);
    pthread_barrier_wait(&g_barrier_work_done);
}
