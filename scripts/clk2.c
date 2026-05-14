#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>

#include "include/engine_constants.h"
#include "include/alsa_device.h"
#include "include/clk2.h"

_Atomic int64_t g_aoip_frames    = 0;
_Atomic int64_t g_aoip_hts_ns    = 0;
_Atomic int64_t g_ravenna_frames = 0;
_Atomic int64_t g_ravenna_hts_ns = 0;

volatile int    g_clk2_report       = 1;
_Atomic double  g_ravenna_ratio_hint = 1.0;

extern int g_period_frames;

/* 안정화 판정: 연속 STABLE_COUNT회 drift 변화 < STABLE_PPM → 30s 주기로 전환 */
#define STABLE_PPM   2.0
#define STABLE_COUNT 5
#define FAST_INTERVAL_S 30LL
#define SLOW_INTERVAL_S 30LL

/* hw:aoip ↔ RAVENNA 클럭 drift 보고
 * 초기: 1s 주기로 빠르게 측정 → 안정화 후 30s 주기로 전환 */
void clk2_report(int64_t *pa_fr,  int64_t *pa_hts,
                 int64_t *pr_fr,  int64_t *pr_hts,
                 int64_t *next_ns)
{
    static int    stable_cnt  = 0;
    static int    stabilized  = 0;
    static double prev_ppm    = 0.0;

    struct timespec _now;
    clock_gettime(CLOCK_MONOTONIC, &_now);
    int64_t now_ns = (int64_t)_now.tv_sec * 1000000000LL + _now.tv_nsec;

    if (!g_clk2_report) {
        *next_ns = now_ns + SLOW_INTERVAL_S * 1000000000LL;
        return;
    }

    if (now_ns < *next_ns) return;

    int64_t a_fr  = atomic_load_explicit(&g_aoip_frames,    memory_order_acquire);
    int64_t a_hts = atomic_load_explicit(&g_aoip_hts_ns,    memory_order_acquire);
    int64_t r_fr  = atomic_load_explicit(&g_ravenna_frames,  memory_order_acquire);
    int64_t r_hts = atomic_load_explicit(&g_ravenna_hts_ns,  memory_order_acquire);

    int64_t interval_s = stabilized ? SLOW_INTERVAL_S : FAST_INTERVAL_S;

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

        double ratio = ravenna_rate / aoip_rate;
        if (ratio > RATIO_MIN && ratio < RATIO_MAX)
            atomic_store_explicit(&g_ravenna_ratio_hint, ratio, memory_order_relaxed);

        /* 안정화 판정 (초기 빠른 수렴 구간에서만) */
        if (!stabilized) {
            if (fabs(drift_ppm - prev_ppm) < STABLE_PPM)
                stable_cnt++;
            else
                stable_cnt = 0;
            prev_ppm = drift_ppm;
            if (stable_cnt >= STABLE_COUNT) {
                stabilized = 1;
                fprintf(stderr, "[aoip_engine] clk2: stabilized at drift_ppm=%+.3f"
                        " ratio=%.7f → switching to %llds interval\n",
                        drift_ppm, ratio, (long long)SLOW_INTERVAL_S);
            }
        }

        printf("clk2 aoip_rate=%.3f ravenna_rate=%.3f drift_ppm=%+.3f elapsed=%.0f"
               " dsp_period=%d dsp_tick_ms=%.3f ratio_hint=%.7f%s\n",
               aoip_rate, ravenna_rate, drift_ppm, elapsed_s,
               g_period_frames, dsp_tick_ms, ratio,
               stabilized ? "" : " [fast]");
        fflush(stdout);
    }

    *pa_fr  = a_fr;  *pa_hts = a_hts;
    *pr_fr  = r_fr;  *pr_hts = r_hts;
    *next_ns = now_ns + interval_s * 1000000000LL;
}
