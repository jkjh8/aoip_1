#define _GNU_SOURCE
#include <stdio.h>
#include <stdint.h>
#include <stdatomic.h>
#include <math.h>
#include <time.h>

#include "include/engine_constants.h"
#include "include/alsa_device.h"
#include "include/clk2.h"

_Atomic int64_t  g_aoip_frames    = 0;
_Atomic int64_t  g_aoip_hts_ns    = 0;
_Atomic uint32_t g_aoip_seq       = 0;
_Atomic int64_t  g_ravenna_frames = 0;
_Atomic int64_t  g_ravenna_hts_ns = 0;
_Atomic uint32_t g_ravenna_seq    = 0;

volatile int    g_clk2_report       = 1;
_Atomic double  g_ravenna_ratio_hint = 1.0;

/* seqlock reader: writer의 (frames, hts_ns) 페어를 일관성 있게 스냅샷 */
static inline void clk2_reader_snapshot(_Atomic uint32_t *seq,
                                        _Atomic int64_t  *frames,
                                        _Atomic int64_t  *hts_ns,
                                        int64_t *out_fr, int64_t *out_hts)
{
    uint32_t s1, s2;
    for (int spin = 0; spin < 1024; spin++) {
        s1 = atomic_load_explicit(seq, memory_order_acquire);
        if (s1 & 1u) continue;                       /* writer mid-update */
        *out_fr  = atomic_load_explicit(frames, memory_order_relaxed);
        *out_hts = atomic_load_explicit(hts_ns, memory_order_relaxed);
        atomic_thread_fence(memory_order_acquire);
        s2 = atomic_load_explicit(seq, memory_order_acquire);
        if (s1 == s2) return;                        /* consistent pair */
    }
    /* spin 한계 초과 — 마지막 값 그대로 반환 (다음 주기에 재시도) */
}

extern int g_period_frames;

/* 안정화 판정: 연속 STABLE_COUNT회 drift 변화 < STABLE_PPM → 30s 주기로 전환 */
#define STABLE_PPM   2.0
#define STABLE_COUNT 5
#define FAST_INTERVAL_S 30LL
#define SLOW_INTERVAL_S 30LL

/* 5-sample median 필터 — 측정창에 끼는 일시적 이상치 제거용.
 * seqlock으로 torn read는 막았지만, ALSA htstamp 자체가 한 period 만큼
 * 늦게 갱신되는 경우가 있어 표시값/제어값을 한 번 더 부드럽게 한다. */
#define MED_N 5
static double clk2_median5(const double *src)
{
    double a[MED_N];
    for (int i = 0; i < MED_N; i++) a[i] = src[i];
    for (int i = 0; i < MED_N; i++)
        for (int j = i + 1; j < MED_N; j++)
            if (a[j] < a[i]) { double t = a[i]; a[i] = a[j]; a[j] = t; }
    return a[MED_N / 2];
}

/* hw:aoip ↔ RAVENNA 클럭 drift 보고
 * 초기: 1s 주기로 빠르게 측정 → 안정화 후 30s 주기로 전환 */
void clk2_report(int64_t *pa_fr,  int64_t *pa_hts,
                 int64_t *pr_fr,  int64_t *pr_hts,
                 int64_t *next_ns)
{
    static int    stable_cnt  = 0;
    static int    stabilized  = 0;
    static double prev_ppm    = 0.0;

    static double med_ppm[MED_N]   = {0};
    static double med_ratio[MED_N] = {0};
    static int    med_idx          = 0;
    static int    med_filled       = 0;

    struct timespec _now;
    clock_gettime(CLOCK_MONOTONIC, &_now);
    int64_t now_ns = (int64_t)_now.tv_sec * 1000000000LL + _now.tv_nsec;

    if (!g_clk2_report) {
        *next_ns = now_ns + SLOW_INTERVAL_S * 1000000000LL;
        return;
    }

    if (now_ns < *next_ns) return;

    int64_t a_fr = 0, a_hts = 0, r_fr = 0, r_hts = 0;
    clk2_reader_snapshot(&g_aoip_seq,    &g_aoip_frames,    &g_aoip_hts_ns,    &a_fr, &a_hts);
    clk2_reader_snapshot(&g_ravenna_seq, &g_ravenna_frames, &g_ravenna_hts_ns, &r_fr, &r_hts);

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
        double ratio        = ravenna_rate / aoip_rate;

        /* 5-sample median 필터 — 이상치 억제 */
        med_ppm[med_idx]   = drift_ppm;
        med_ratio[med_idx] = ratio;
        med_idx = (med_idx + 1) % MED_N;
        if (med_filled < MED_N) med_filled++;

        double drift_ppm_med = (med_filled == MED_N) ? clk2_median5(med_ppm)   : drift_ppm;
        double ratio_med     = (med_filled == MED_N) ? clk2_median5(med_ratio) : ratio;

        /* ratio_hint는 median 값으로 갱신 — 일시적 이상치가 DSP 보간 비율에 새지 않도록 */
        if (ratio_med > RATIO_MIN && ratio_med < RATIO_MAX)
            atomic_store_explicit(&g_ravenna_ratio_hint, ratio_med, memory_order_relaxed);

        /* 안정화 판정도 median 기준으로 (초기 빠른 수렴 구간에서만) */
        if (!stabilized) {
            if (fabs(drift_ppm_med - prev_ppm) < STABLE_PPM)
                stable_cnt++;
            else
                stable_cnt = 0;
            prev_ppm = drift_ppm_med;
            if (stable_cnt >= STABLE_COUNT) {
                stabilized = 1;
                fprintf(stderr, "[aoip_engine] clk2: stabilized at drift_ppm=%+.3f"
                        " ratio=%.7f → switching to %llds interval\n",
                        drift_ppm_med, ratio_med, (long long)SLOW_INTERVAL_S);
            }
        }

        printf("clk2 aoip_rate=%.3f ravenna_rate=%.3f drift_ppm=%+.3f elapsed=%.0f"
               " dsp_period=%d dsp_tick_ms=%.3f ratio_hint=%.7f%s\n",
               aoip_rate, ravenna_rate, drift_ppm_med, elapsed_s,
               g_period_frames, dsp_tick_ms, ratio_med,
               stabilized ? "" : " [fast]");
        fflush(stdout);
    }

    *pa_fr  = a_fr;  *pa_hts = a_hts;
    *pr_fr  = r_fr;  *pr_hts = r_hts;
    *next_ns = now_ns + interval_s * 1000000000LL;
}
