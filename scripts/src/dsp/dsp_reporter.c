#define _GNU_SOURCE
#include <stdio.h>
#include <math.h>
#include <unistd.h>
#include <stdatomic.h>
#include "include/dsp_reporter.h"
#include "include/engine_globals.h"
#include "include/engine_constants.h"

/* ~6Hz (167ms) 주기로 레벨 + GR 리포트 */
void *reporter_thread(void *arg)
{
    (void)arg;
    int buf_tick = 0;
    while (g_reporter_running) {
        usleep(167000);
        if (g_lvl_report) {
            for (int i = 0; i < g_n_in; i++) {
                float pk = atomic_exchange_explicit(&g_in_level[i], 0.0f, memory_order_relaxed);
                printf("lvl in %d %.1f\n", i+1, pk > 1e-7f ? 20.0f*log10f(pk) : -120.0f);
            }
            for (int i = 0; i < g_n_out; i++) {
                float pk = atomic_exchange_explicit(&g_out_level[i], 0.0f, memory_order_relaxed);
                printf("lvl out %d %.1f\n", i+1, pk > 1e-7f ? 20.0f*log10f(pk) : -120.0f);
            }
        }
        if (g_gr_report) {
            for (int i = 0; i < g_n_in; i++) {
                float gate = atomic_exchange_explicit(&g_in_gr_gate[i], 0.0f, memory_order_relaxed);
                float comp = atomic_exchange_explicit(&g_in_gr_comp[i], 0.0f, memory_order_relaxed);
                printf("gr in %d gate %.1f comp %.1f\n", i+1, gate, comp);
            }
            for (int i = 0; i < g_n_out; i++) {
                float gate = atomic_exchange_explicit(&g_out_gr_gate[i], 0.0f, memory_order_relaxed);
                float comp = atomic_exchange_explicit(&g_out_gr_comp[i], 0.0f, memory_order_relaxed);
                float lim  = atomic_exchange_explicit(&g_out_gr_lim[i],  0.0f, memory_order_relaxed);
                printf("gr out %d gate %.1f comp %.1f lim %.1f\n", i+1, gate, comp, lim);
            }
        }
        /* 2초마다 rtp_in 버퍼 상태 보고 (167ms * 12 ≈ 2s) */
        if (++buf_tick >= 12) {
            buf_tick = 0;
            for (int i = 0; i < g_n_rtp_in; i++) {
                RtpStream *r = &g_rtp_in[i];
                if (!r->enabled || !r->ring.buf) continue;
                int fill = rb_avail(&r->ring);
                int fill_ms   = fill * 1000 / SAMPLE_RATE;
                int target_ms = r->fill_target > 0 ? r->fill_target * 1000 / SAMPLE_RATE : 0;
                int pct = r->fill_target > 0 ? fill * 100 / r->fill_target : 0;
                printf("rtp_buf %s fillMs=%d targetMs=%d pct=%d underrun=%d underrunTotal=%ld overrunTotal=%ld\n",
                       r->name, fill_ms, target_ms, pct, r->rtp_underrun,
                       r->underrun_total, r->overrun_total);
            }
        }
        fflush(stdout);
    }
    return NULL;
}
