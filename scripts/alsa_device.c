/*
 * alsa_device.c — ALSA 브릿지 장치 관리
 *
 * - PI 드리프트 보정
 * - ALSA PCM 오픈/설정
 * - 캡처 스레드 (P80 SCHED_FIFO): ALSA → RingBuf
 * - 재생 스레드 (P80 SCHED_FIFO): RingBuf → ALSA
 * - device_start / device_stop
 */
#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <math.h>
#include <samplerate.h>
#include "include/alsa_device.h"

/* aoip_engine.c 가 소유하는 전역 quit 플래그 */
extern volatile int g_quit;

/* ── PI 드리프트 보정 ────────────────────────────────────────────── */
void pi_reset(PiState *p) {
    p->ratio = 1.0; p->integ = 0.0; p->smooth = 0.0; p->prebuf_done = 0;
}

void pi_update(PiState *p, int avail, int target) {
    double err  = ((double)avail - target) / (double)target;
    p->smooth  += 0.05 * (err - p->smooth);
    p->integ   += p->smooth;
    p->ratio    = 1.0 + p->smooth * RATIO_KP + p->integ * RATIO_KI;
    if (p->ratio < RATIO_MIN) {
        p->ratio = RATIO_MIN;
        p->integ = (RATIO_MIN - 1.0 - p->smooth * RATIO_KP) / RATIO_KI;
    }
    if (p->ratio > RATIO_MAX) {
        p->ratio = RATIO_MAX;
        p->integ = (RATIO_MAX - 1.0 - p->smooth * RATIO_KP) / RATIO_KI;
    }
}

/* ── ALSA 오픈 헬퍼 ──────────────────────────────────────────────── */
snd_pcm_t *alsa_open(const char *dev, int stream, int rate,
                     int period, int nperiods, int ch)
{
    snd_pcm_t *pcm = NULL;
    int err;
    if ((err = snd_pcm_open(&pcm, dev, stream, 0)) < 0) {
        fprintf(stderr, "[aoip_engine] alsa_open %s (%s): %s\n",
                dev, stream == SND_PCM_STREAM_CAPTURE ? "cap" : "play",
                snd_strerror(err));
        return NULL;
    }
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    if ((err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE)) < 0) {
        fprintf(stderr, "[aoip_engine] alsa_open %s: S32_LE not supported: %s\n",
                dev, snd_strerror(err));
        snd_pcm_close(pcm); return NULL;
    }
    snd_pcm_hw_params_set_channels(pcm, hw, (unsigned)ch);
    unsigned r = (unsigned)rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0);
    snd_pcm_uframes_t p = (snd_pcm_uframes_t)period;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &p, 0);
    snd_pcm_uframes_t buf = p * (snd_pcm_uframes_t)nperiods;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buf);
    if ((err = snd_pcm_hw_params(pcm, hw)) < 0) {
        fprintf(stderr, "[aoip_engine] alsa_open %s hw_params: %s\n",
                dev, snd_strerror(err));
        snd_pcm_close(pcm); return NULL;
    }
    if (r != (unsigned)rate)
        fprintf(stderr, "[aoip_engine] alsa_open %s: rate %d→%u\n", dev, rate, r);
    if (p != (snd_pcm_uframes_t)period)
        fprintf(stderr, "[aoip_engine] alsa_open %s: period %d→%lu\n",
                dev, period, (unsigned long)p);
    snd_pcm_prepare(pcm);
    return pcm;
}

/* ── ALSA 캡처 스레드 ────────────────────────────────────────────── */
static void *alsa_capture_thread(void *arg)
{
    Device *d = (Device *)arg;

    struct sched_param sp = { .sched_priority = d->thread_priority };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    snd_pcm_t *pcm = NULL;
    while (!d->quit_cap && !g_quit) {
        pcm = alsa_open(d->dev, SND_PCM_STREAM_CAPTURE,
                        d->rate, d->period, d->nperiods, d->channels);
        if (pcm) break;
        fprintf(stderr, "[aoip_engine] cap %s: open failed, retry in 2s\n", d->name);
        usleep(2000000);
    }
    if (!pcm) return NULL;

    int32_t *ibuf = malloc((size_t)(d->period * d->channels) * sizeof(int32_t));
    float   *fbuf = malloc((size_t)(d->period * d->channels) * sizeof(float));
    int cap_err_count = 0;

    while (!d->quit_cap && !g_quit) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, ibuf, (snd_pcm_uframes_t)d->period);
        if (n == -EPIPE) {
            fprintf(stderr, "[aoip_engine] cap %s: xrun (overrun)\n", d->name);
            snd_pcm_prepare(pcm); continue;
        }
        if (n == -ESTRPIPE) {
            while (!g_quit && snd_pcm_resume(pcm) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(pcm); continue;
        }
        if (n == -EIO) {
            /* UAC2 가젯: 호스트 스트림 미활성 */
            if (cap_err_count++ == 0)
                fprintf(stderr, "[aoip_engine] cap %s: EIO, host stream not active\n", d->name);
            snd_pcm_prepare(pcm);
            usleep(100000);
            continue;
        }
        if (n < 0) {
            if (cap_err_count++ == 0)
                fprintf(stderr, "[aoip_engine] cap %s: %s\n", d->name, snd_strerror((int)n));
            snd_pcm_close(pcm); pcm = NULL;
            rb_reset(&d->in_ring);
            while (!d->quit_cap && !g_quit) {
                usleep(500000);
                pcm = alsa_open(d->dev, SND_PCM_STREAM_CAPTURE,
                                d->rate, d->period, d->nperiods, d->channels);
                if (pcm) { cap_err_count = 0; break; }
            }
            continue;
        }
        cap_err_count = 0;
        for (int i = 0; i < (int)n * d->channels; i++)
            fbuf[i] = (float)ibuf[i] * (1.0f / 2147483648.0f);
        rb_write(&d->in_ring, fbuf, (int)n);
    }

    free(ibuf); free(fbuf);
    if (pcm) snd_pcm_close(pcm);
    return NULL;
}

/* ── ALSA 재생 스레드 ────────────────────────────────────────────── */
static void *alsa_playback_thread(void *arg)
{
    Device *d = (Device *)arg;

    struct sched_param sp = { .sched_priority = d->thread_priority };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    snd_pcm_t *pcm = NULL;
    while (!d->quit_play && !g_quit) {
        pcm = alsa_open(d->dev, SND_PCM_STREAM_PLAYBACK,
                        d->rate, d->period, d->nperiods, d->channels);
        if (pcm) break;
        fprintf(stderr, "[aoip_engine] play %s: open failed, retry in 2s\n", d->name);
        usleep(2000000);
    }
    if (!pcm) return NULL;

    float   *fbuf     = malloc((size_t)(d->period * d->channels) * sizeof(float));
    float   *fade_buf = calloc((size_t)(d->period * d->channels), sizeof(float));
    int32_t *ibuf     = malloc((size_t)(d->period * d->channels) * sizeof(int32_t));
    int      fade_cnt = 0;
    int play_err_count = 0;

    while (!d->quit_play && !g_quit && rb_avail(&d->out_ring) < PREBUF_FRAMES)
        usleep(1000);

    while (!d->quit_play && !g_quit) {
        if (!rb_read(&d->out_ring, fbuf, d->period)) {
            /* 언더런: 페이드아웃으로 클릭 방지 */
            float scale = (fade_cnt < UNDERRUN_FADE_PERIODS)
                          ? 1.0f - (float)(fade_cnt + 1) / (float)UNDERRUN_FADE_PERIODS
                          : 0.0f;
            int n = d->period * d->channels;
            for (int i = 0; i < n; i++) fbuf[i] = fade_buf[i] * scale;
            if (fade_cnt < UNDERRUN_FADE_PERIODS) fade_cnt++;
        } else {
            memcpy(fade_buf, fbuf, (size_t)(d->period * d->channels) * sizeof(float));
            fade_cnt = 0;
        }
        for (int i = 0; i < d->period * d->channels; i++) {
            float v = fbuf[i];
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            ibuf[i] = (int32_t)(v * 2147483647.0f);
        }

        snd_pcm_sframes_t n = snd_pcm_writei(pcm, ibuf, (snd_pcm_uframes_t)d->period);
        if (n == -EPIPE) {
            fprintf(stderr, "[aoip_engine] play %s: xrun (underrun)\n", d->name);
            rb_reset(&d->out_ring); snd_pcm_prepare(pcm);
            while (!d->quit_play && !g_quit && rb_avail(&d->out_ring) < PREBUF_FRAMES)
                usleep(1000);
        } else if (n == -ESTRPIPE) {
            rb_reset(&d->out_ring);
            while (!g_quit && snd_pcm_resume(pcm) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(pcm);
            while (!d->quit_play && !g_quit && rb_avail(&d->out_ring) < PREBUF_FRAMES)
                usleep(1000);
        } else if (n == -EIO) {
            /* UAC2 가젯: 호스트 스트림 미활성 */
            if (play_err_count++ == 0)
                fprintf(stderr, "[aoip_engine] play %s: EIO, host stream not active\n", d->name);
            rb_reset(&d->out_ring);
            snd_pcm_prepare(pcm);
            usleep(100000);
        } else if (n < 0) {
            play_err_count++;
            if (play_err_count == 1)
                fprintf(stderr, "[aoip_engine] play %s: %s (will suppress repeats)\n",
                        d->name, snd_strerror((int)n));
            snd_pcm_close(pcm); pcm = NULL;
            rb_reset(&d->out_ring);
            while (!d->quit_play && !g_quit) {
                usleep(500000);
                pcm = alsa_open(d->dev, SND_PCM_STREAM_PLAYBACK,
                                d->rate, d->period, d->nperiods, d->channels);
                if (pcm) {
                    play_err_count = 0;
                    while (!d->quit_play && !g_quit && rb_avail(&d->out_ring) < PREBUF_FRAMES)
                        usleep(1000);
                    break;
                }
            }
        } else {
            play_err_count = 0;
        }
    }

    free(fbuf); free(fade_buf); free(ibuf);
    if (pcm) snd_pcm_close(pcm);
    return NULL;
}

/* ── device_start ────────────────────────────────────────────────── */
void device_start(Device *d)
{
    d->enabled = 1;
    int err;
    d->quit_cap = d->quit_play = 0;

    if (d->mode != 2) {  /* capture */
        d->cap_src = src_new(SRC_SINC_FASTEST, d->channels, &err);
        pi_reset(&d->cap_pi);
        rb_init(&d->in_ring, RING_FRAMES, d->channels);
        d->cap_fade_buf = calloc((size_t)(PERIOD_FRAMES * d->channels), sizeof(float));
        d->cap_fade_cnt = 0;
        pthread_create(&d->cap_tid, NULL, alsa_capture_thread, d);
    }
    if (d->mode != 1) {  /* playback */
        d->play_src = src_new(SRC_SINC_FASTEST, d->channels, &err);
        pi_reset(&d->play_pi);
        rb_init(&d->out_ring, RING_FRAMES, d->channels);
        pthread_create(&d->play_tid, NULL, alsa_playback_thread, d);
    }
    printf("bridge:%s:ready\n", d->name);
    fflush(stdout);
}

/* ── device_stop ─────────────────────────────────────────────────── */
void device_stop(Device *d)
{
    /* DSP 스레드가 이 장치를 건너뛰도록 먼저 비활성화.
     * DSP 루프 최대 1주기(~11ms)가 끝날 때까지 대기 후 메모리 해제. */
    d->enabled = 0;
    __sync_synchronize();
    usleep(25000);  /* ≥2 DSP 주기 (~21ms) */

    if (d->mode != 2) {
        d->quit_cap = 1;
        pthread_join(d->cap_tid, NULL);
        if (d->cap_src)      { src_delete(d->cap_src); d->cap_src = NULL; }
        if (d->in_ring.buf)  { free(d->in_ring.buf);   d->in_ring.buf = NULL; }
        if (d->cap_fade_buf) { free(d->cap_fade_buf);  d->cap_fade_buf = NULL; }
    }
    if (d->mode != 1) {
        d->quit_play = 1;
        pthread_join(d->play_tid, NULL);
        if (d->play_src)     { src_delete(d->play_src); d->play_src = NULL; }
        if (d->out_ring.buf) { free(d->out_ring.buf);   d->out_ring.buf = NULL; }
    }
    printf("bridge:%s:stopped\n", d->name);
    fflush(stdout);
}
