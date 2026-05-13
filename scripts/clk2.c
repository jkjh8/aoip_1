#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <time.h>

#include "include/engine_constants.h"
#include "include/clk2.h"

_Atomic int64_t g_aoip_frames    = 0;
_Atomic int64_t g_aoip_hts_ns    = 0;
_Atomic int64_t g_ravenna_frames = 0;
_Atomic int64_t g_ravenna_hts_ns = 0;

volatile int g_clk2_report = 1;

extern int g_period_frames;

/* hw:aoip ↔ RAVENNA 클럭 drift 보고 (30s 주기) */
void clk2_report(int64_t *pa_fr,  int64_t *pa_hts,
                 int64_t *pr_fr,  int64_t *pr_hts,
                 int64_t *next_ns)
{
    struct timespec _now;
    clock_gettime(CLOCK_MONOTONIC, &_now);
    int64_t now_ns = (int64_t)_now.tv_sec * 1000000000LL + _now.tv_nsec;

    if (!g_clk2_report) {
        *next_ns = now_ns + 30LL * 1000000000LL;
        return;
    }

    if (now_ns < *next_ns) return;

    int64_t a_fr  = atomic_load_explicit(&g_aoip_frames,    memory_order_acquire);
    int64_t a_hts = atomic_load_explicit(&g_aoip_hts_ns,    memory_order_acquire);
    int64_t r_fr  = atomic_load_explicit(&g_ravenna_frames,  memory_order_acquire);
    int64_t r_hts = atomic_load_explicit(&g_ravenna_hts_ns,  memory_order_acquire);

    if (*pa_hts > 0 && *pr_hts > 0 &&
        a_hts > *pa_hts && r_hts > *pr_hts) {

        int64_t da_fr  = a_fr  - *pa_fr;
        int64_t da_hts = a_hts - *pa_hts;
        int64_t dr_fr  = r_fr  - *pr_fr;
        int64_t dr_hts = r_hts - *pr_hts;

        double aoip_rate    = (double)da_fr * 1e9 / (double)da_hts;
        double ravenna_rate = (double)dr_fr * 1e9 / (double)dr_hts;
        double drift_ppm    = (aoip_rate - ravenna_rate) / SAMPLE_RATE * 1e6;
        double elapsed_s    = (double)da_hts / 1e9;
        double dsp_tick_ms  = (double)g_period_frames / aoip_rate * 1000.0;

        printf("clk2 aoip_rate=%.3f ravenna_rate=%.3f drift_ppm=%+.3f elapsed=%.0f"
               " dsp_period=%d dsp_tick_ms=%.3f\n",
               aoip_rate, ravenna_rate, drift_ppm, elapsed_s,
               g_period_frames, dsp_tick_ms);
        fflush(stdout);
    }

    *pa_fr  = a_fr;  *pa_hts = a_hts;
    *pr_fr  = r_fr;  *pr_hts = r_hts;
    *next_ns = now_ns + 30LL * 1000000000LL;
}
