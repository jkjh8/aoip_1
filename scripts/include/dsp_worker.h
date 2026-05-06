#pragma once
#include <pthread.h>
#include "engine_constants.h"

typedef struct {
    int id;
    int ch_start;
    int ch_count;
    int n_in;
    int n_out;
} WorkerArg;

extern WorkerArg         g_worker_arg[DSP_WORKER_COUNT];
extern pthread_t         g_worker_tid[DSP_WORKER_COUNT];
extern pthread_barrier_t g_barrier_work_start;
extern pthread_barrier_t g_barrier_input_done;
extern pthread_barrier_t g_barrier_routing_done;
extern pthread_barrier_t g_barrier_work_done;
extern volatile int      g_worker_quit;

void *dsp_worker_thread(void *arg);
void  dsp_run_parallel(void);
