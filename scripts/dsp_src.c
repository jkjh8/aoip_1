#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <samplerate.h>

#include "include/engine_constants.h"
#include "include/alsa_device.h"
#include "include/dsp_src.h"

extern int    g_period_frames;
extern float *g_in_ptr[MAX_CH];
extern float *g_out_ptr[MAX_CH];

/* ── PI 드리프트 보정 ────────────────────────────────────────────── */
/* hw:aoip 크리스탈 실측 drift 보정 초기값 (-10ppm).
 * integ를 미리 세팅하여 수렴 시간 없이 즉시 보정 적용. */
#define RATIO_INIT_OFFSET  (-10e-6)

void pi_reset(PiState *p) {
    double ki = p->ki > 0.0 ? p->ki : RATIO_KI;
    p->smooth = 0.0;
    p->integ  = RATIO_INIT_OFFSET / ki;
    p->ratio  = 1.0 + RATIO_INIT_OFFSET;
}

void pi_update(PiState *p, int avail, int target) {
    double err  = ((double)target - avail) / (double)target;
    p->smooth  += 0.2 * (err - p->smooth);
    p->integ   += p->smooth;
    p->ratio    = 1.0 + p->smooth * p->kp + p->integ * p->ki;
    if (p->ratio < p->min) { p->ratio = p->min; p->integ = (p->min - 1.0 - p->smooth * p->kp) / p->ki; }
    if (p->ratio > p->max) { p->ratio = p->max; p->integ = (p->max - 1.0 - p->smooth * p->kp) / p->ki; }
}

/*
 * src_convert — PI 보정 SRC 통합 함수
 *
 * 캡처 (is_capture=1):
 *   tmp_in  : 인터리브 입력 (호출자가 fill 프레임만큼 사전 채움)
 *   → SRC → g_in_buf[ch_start..] 디인터리브
 *   반환: 소비된 입력 프레임 수 (0 = 언더런, g_in_buf → zeros)
 *
 * 재생 (is_capture=0):
 *   g_out_buf[ch_start..] → 인터리브 → SRC → tmp_out
 *   ring_space: rb_free (출력 링 여유 공간 체크)
 *   반환: 생성된 출력 프레임 수 (0 = 링 공간 부족, skip)
 */
long src_convert(SRC_STATE *src, PiState *pi,
                 float *tmp_in, float *tmp_out,
                 int fill, int target, int ring_space,
                 int channels, int ch_start,
                 int is_capture)
{
    pi_update(pi, fill, target);

    if (is_capture) {
        int need = (int)ceil((double)g_period_frames / pi->ratio) + 2;
        if (need > fill || need > DEV_TMP_FRAMES) {
            src_reset(src); pi_reset(pi);
            for (int c = 0; c < channels && (ch_start+c) < MAX_CH; c++)
                memset(g_in_ptr[ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
            return 0;
        }
        SRC_DATA sd = {
            .data_in = tmp_in, .data_out = tmp_out,
            .input_frames = need, .output_frames = g_period_frames,
            .src_ratio = pi->ratio,
        };
        src_process(src, &sd);
        long gen = sd.output_frames_gen;
        for (int c = 0; c < channels && (ch_start+c) < MAX_CH; c++) {
            float *dst = g_in_ptr[ch_start+c];
            for (long f = 0; f < gen; f++) dst[f] = tmp_out[f*channels+c];
            for (long f = gen; f < g_period_frames; f++) dst[f] = 0.0f;
        }
        return sd.input_frames_used;
    } else {
        long out_max = (long)ceil((double)g_period_frames * pi->ratio) + 4;
        if (out_max > DEV_TMP_FRAMES) out_max = DEV_TMP_FRAMES;
        if (ring_space < (int)out_max) return 0;
        for (int f = 0; f < g_period_frames; f++)
            for (int c = 0; c < channels && (ch_start+c) < MAX_CH; c++)
                tmp_in[f*channels+c] = g_out_ptr[ch_start+c][f];
        SRC_DATA sd = {
            .data_in = tmp_in, .data_out = tmp_out,
            .input_frames = g_period_frames, .output_frames = out_max,
            .src_ratio = pi->ratio,
        };
        src_process(src, &sd);
        return sd.output_frames_gen;
    }
}

/* ── 캡처 SRC (ALSA RAVENNA / RTP 공용) ─────────────────────────── */
int ring_capture_src(SRC_STATE *src, PiState *pi,
                     RingBuf *ring,
                     float *tmp_in, float *tmp_out,
                     int fill_target, int channels, int ch_start)
{
    int avail = rb_avail(ring);

    /* 오버플로우 감지: fill_target 2배 초과 시 오래된 데이터 스킵 (SRC 상태 유지) */
    if (avail > fill_target * 2) {
        unsigned rp0 = atomic_load_explicit(&ring->rp, memory_order_relaxed);
        unsigned skip = (unsigned)(avail - fill_target);
        atomic_store_explicit(&ring->rp, rp0 + skip, memory_order_release);
        pi_reset(pi);
        avail = fill_target;
    }

    int n = avail < DEV_TMP_FRAMES ? avail : DEV_TMP_FRAMES;
    unsigned rp = atomic_load_explicit(&ring->rp, memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        unsigned idx = (rp + (unsigned)i) % (unsigned)ring->ring_frames;
        memcpy(&tmp_in[i * channels],
               &ring->buf[idx * ring->channels],
               (size_t)channels * sizeof(float));
    }
    long used = src_convert(src, pi, tmp_in, tmp_out,
                            avail, fill_target, 0, channels, ch_start, 1);
    if (used > 0)
        atomic_store_explicit(&ring->rp, rp + (unsigned)used, memory_order_release);
    return (int)used;
}

/* ── RAVENNA 재생 SRC ────────────────────────────────────────────── */
void alsa_playback_src(Device *d)
{
    long gen = src_convert(d->play_src, &d->play_pi, d->tmp_play_in, d->tmp_play_out,
                           rb_avail(&d->out_ring), FILL_TARGET, rb_free(&d->out_ring),
                           d->channels, d->ch_start, 0);
    if (gen > 0)
        rb_write(&d->out_ring, d->tmp_play_out, (int)gen);
}
