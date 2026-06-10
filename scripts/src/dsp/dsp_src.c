#define _GNU_SOURCE
#include <stdio.h>
#include <string.h>
#include <math.h>
#include <stdint.h>
#include <samplerate.h>

#include "include/engine_constants.h"
#include "include/alsa_device.h"
#include "include/dsp_src.h"
#include "include/clk2.h"

extern int    g_period_frames;
extern float *g_in_ptr[MAX_CH];
extern float *g_out_ptr[MAX_CH];

/* DEBUG_SRC=1 이면 stderr에 SRC 상세 로그 출력 (빌드 시 -DDEBUG_SRC=1) */
#ifndef DEBUG_SRC
#define DEBUG_SRC 0
#endif

/* PI 로그는 매 N번 호출마다 한 번만 출력 (hot-path 부하 최소화) */
#define SRC_LOG_INTERVAL 500

#if DEBUG_SRC
#define SRC_DBG(fmt, ...) fprintf(stderr, "[SRC] " fmt "\n", ##__VA_ARGS__)
#else
#define SRC_DBG(fmt, ...) (void)0
#endif

/* ── PI 드리프트 보정 ────────────────────────────────────────────── */
#define RATIO_INIT_OFFSET  0.0

void pi_reset(PiState *p) {
    double ki = p->ki > 0.0 ? p->ki : RATIO_KI;
    p->smooth = 0.0;
    p->integ  = RATIO_INIT_OFFSET / ki;
    p->ratio  = 1.0 + RATIO_INIT_OFFSET;
    SRC_DBG("pi_reset → ratio=%.6f integ=%.6f", p->ratio, p->integ);
}

void pi_update(PiState *p, int avail, int target) {
    static uint64_t _pi_tick = 0;
    double err  = ((double)target - avail) / (double)target;
    p->smooth  += 0.2 * (err - p->smooth);
    p->integ   += p->smooth;
    double prev_ratio = p->ratio;
    p->ratio    = 1.0 + p->smooth * p->kp + p->integ * p->ki;

    int clamped = 0;
    if (p->ratio < p->min) { p->ratio = p->min; p->integ = (p->min - 1.0 - p->smooth * p->kp) / p->ki; clamped = -1; }
    if (p->ratio > p->max) { p->ratio = p->max; p->integ = (p->max - 1.0 - p->smooth * p->kp) / p->ki; clamped =  1; }

    if (++_pi_tick % SRC_LOG_INTERVAL == 0) {
        SRC_DBG("pi_update [%6llu] avail=%d target=%d err=%+.4f smooth=%+.6f "
                "ratio=%+.7f (Δ%+.7f)%s",
                (unsigned long long)_pi_tick, avail, target, err, p->smooth,
                p->ratio, p->ratio - prev_ratio,
                clamped < 0 ? " [CLAMP MIN]" : clamped > 0 ? " [CLAMP MAX]" : "");
    }
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
        /* actual_need: SRC가 실제로 소비하는 입력량. need는 src_process에 전달할 상한(+2 여유).
         * 언더런 판정은 actual_need 기준 — +2를 포함하면 데이터가 충분해도 오판정. */
        int actual_need = (int)ceil((double)g_period_frames / pi->ratio);
        int need = actual_need + 2;
        if (actual_need > fill || need > DEV_TMP_FRAMES) {
            SRC_DBG("UNDERRUN ch_start=%d fill=%d actual_need=%d need=%d ratio=%.7f → zeroing %d frames",
                    ch_start, fill, actual_need, need, pi->ratio, g_period_frames);
            for (int c = 0; c < channels && (ch_start+c) < MAX_CH; c++)
                memset(g_in_ptr[ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
            return 0;  /* src/pi 리셋은 호출자(지속 언더런 감지 후)가 결정 */
        }
        int input_size = (fill < need) ? fill : need;
        SRC_DATA sd = {
            .data_in = tmp_in, .data_out = tmp_out,
            .input_frames = input_size, .output_frames = g_period_frames,
            .src_ratio = pi->ratio,
        };
        src_process(src, &sd);
        long gen = sd.output_frames_gen;
        SRC_DBG("cap  ch=%d+%d ratio=%.7f fill=%d need=%d input=%d used=%ld gen=%ld period=%d",
                ch_start, channels, pi->ratio,
                fill, need, input_size, sd.input_frames_used, gen, g_period_frames);
        if (gen < g_period_frames)
            SRC_DBG("cap  SHORT OUTPUT: gen=%ld < period=%d (zeroing %ld tail frames)",
                    gen, g_period_frames, (long)g_period_frames - gen);
        for (int c = 0; c < channels && (ch_start+c) < MAX_CH; c++) {
            float *dst = g_in_ptr[ch_start+c];
            for (long f = 0; f < gen; f++) dst[f] = tmp_out[f*channels+c];
            for (long f = gen; f < g_period_frames; f++) dst[f] = 0.0f;
        }
        return sd.input_frames_used;
    } else {
        long out_max = (long)ceil((double)g_period_frames * pi->ratio) + 4;
        if (out_max > DEV_TMP_FRAMES) out_max = DEV_TMP_FRAMES;
        if (ring_space < (int)out_max) {
            SRC_DBG("play SKIP ch=%d+%d ring_space=%d out_max=%ld ratio=%.7f",
                    ch_start, channels, ring_space, out_max, pi->ratio);
            return 0;
        }
        for (int f = 0; f < g_period_frames; f++)
            for (int c = 0; c < channels && (ch_start+c) < MAX_CH; c++)
                tmp_in[f*channels+c] = g_out_ptr[ch_start+c][f];
        SRC_DATA sd = {
            .data_in = tmp_in, .data_out = tmp_out,
            .input_frames = g_period_frames, .output_frames = out_max,
            .src_ratio = pi->ratio,
        };
        src_process(src, &sd);
        SRC_DBG("play ch=%d+%d ratio=%.7f period=%d out_max=%ld gen=%ld ring_space=%d",
                ch_start, channels, pi->ratio,
                g_period_frames, out_max, sd.output_frames_gen, ring_space);
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

    /* 오버플로우 감지: fill_target 2배 초과 시 오래된 데이터 스킵.
     * pi_reset 금지 — 리셋하면 ratio=1.0으로 돌아가 즉시 재오버플로우 루프 발생.
     * 대신 pi_update로 실제 overflow 크기를 PI에 반영하여 수렴 유도. */
    if (avail > fill_target * 2) {
        unsigned rp0 = atomic_load_explicit(&ring->rp, memory_order_relaxed);
        unsigned skip = (unsigned)(avail - fill_target);
        SRC_DBG("OVERFLOW ch=%d+%d avail=%d target=%d → skip=%u frames (rp %u→%u)",
                ch_start, channels, avail, fill_target, skip, rp0, rp0 + skip);
        atomic_store_explicit(&ring->rp, rp0 + skip, memory_order_release);
        pi_update(pi, avail, fill_target);
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
    /* DSP는 호스트 스트림 활성 여부와 무관하게 계속 out_ring을 채운다.
     * UAC2 비활성 동안 ring이 차서 rb_write가 잘려도 무해 — 재활성화 시점에
     * 재생 스레드가 rb_reset(rp=wp)으로 현재 write 위치로 점프해서 그 이후 신선한
     * 데이터부터 재생한다. */

    /* out_ring 리셋 후 SRC 내부 delay line의 이전 오디오 잔재를 제거.
     * 리셋 없이 재개하면 SRC 히스토리가 새 오디오와 섞여 '외계인 소리' 발생. */
    if (atomic_load_explicit(&d->play_src_reset, memory_order_acquire)) {
        src_reset(d->play_src);
        pi_reset(&d->play_pi);
        double hint = atomic_load_explicit(&g_ravenna_ratio_hint, memory_order_relaxed);
        if (hint > RATIO_MIN && hint < RATIO_MAX)
            d->play_pi.ratio = hint;
        atomic_store_explicit(&d->play_src_reset, 0, memory_order_release);
    }

    int play_avail = rb_avail(&d->out_ring);
    int play_free  = rb_free(&d->out_ring);
    long gen = src_convert(d->play_src, &d->play_pi, d->tmp_play_in, d->tmp_play_out,
                           play_avail, FILL_TARGET, play_free,
                           d->channels, d->ch_start, 0);
    SRC_DBG("alsa_play ch=%d+%d ring_avail=%d ring_free=%d gen=%ld ratio=%.7f",
            d->ch_start, d->channels, play_avail, play_free, gen, d->play_pi.ratio);
    if (gen > 0)
        rb_write(&d->out_ring, d->tmp_play_out, (int)gen);
}
