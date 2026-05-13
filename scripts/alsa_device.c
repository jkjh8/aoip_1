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
#include <stdint.h>
#include <math.h>
#include <poll.h>
#include <sys/eventfd.h>
#include "include/alsa_device.h"

/* aoip_engine.c 가 소유하는 전역 플래그 */
extern volatile int g_quit;
extern int          g_period_frames;

/* RAVENNA 클럭 마스터용 eventfd — aoip_engine.c 에서 생성, 여기서 신호 */
extern int          g_dsp_clock_fd;

/* hw:aoip ↔ hw:RAVENNA 클럭 비교 — aoip_engine.c 소유 */
extern _Atomic int64_t g_aoip_frames;
extern _Atomic int64_t g_aoip_hts_ns;
extern _Atomic int64_t g_ravenna_frames;
extern _Atomic int64_t g_ravenna_hts_ns;

/* ── PI 드리프트 보정 ────────────────────────────────────────────── */
void pi_reset(PiState *p) {
    p->ratio = 1.0; p->integ = 0.0; p->smooth = 0.0;
}

void pi_update(PiState *p, int avail, int target) {
    double err  = ((double)target - avail) / (double)target;
    p->smooth  += 0.05 * (err - p->smooth);
    p->integ   += p->smooth;
    p->ratio    = 1.0 + p->smooth * p->kp + p->integ * p->ki;
    if (p->ratio < p->min) { p->ratio = p->min; p->integ = (p->min - 1.0 - p->smooth * p->kp) / p->ki; }
    if (p->ratio > p->max) { p->ratio = p->max; p->integ = (p->max - 1.0 - p->smooth * p->kp) / p->ki; }
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
    /* ALSA htstamp 활성화 — snd_pcm_htimestamp() 사용을 위해 필요 */
    snd_pcm_sw_params_t *sw;
    snd_pcm_sw_params_alloca(&sw);
    snd_pcm_sw_params_current(pcm, sw);
    snd_pcm_sw_params_set_tstamp_mode(pcm, sw, SND_PCM_TSTAMP_ENABLE);
    snd_pcm_sw_params_set_tstamp_type(pcm, sw, SND_PCM_TSTAMP_TYPE_MONOTONIC);
    snd_pcm_sw_params(pcm, sw);

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
        for (int _i = 0; _i < 20 && !d->quit_cap && !g_quit; _i++) usleep(100000);
    }
    if (!pcm) return NULL;

    int32_t *ibuf = malloc((size_t)(d->period * d->channels) * sizeof(int32_t));
    float   *fbuf = malloc((size_t)(d->period * d->channels) * sizeof(float));
    int cap_err_count = 0;

    /* RAVENNA: alsa_open 후 PREPARED 상태 — 명시적 start 필요.
     * poll-before-read 방식은 PREPARED 상태에서 POLLIN이 오지 않아 deadlock.
     * snd_pcm_start()로 RUNNING 상태로 전환 후 poll 사용. */
    if (d->is_ravenna) {
        if (snd_pcm_start(pcm) < 0)
            fprintf(stderr, "[aoip_engine] cap %s: snd_pcm_start failed, continuing\n", d->name);
    }

    while (!d->quit_cap && !g_quit) {
        /* RAVENNA: PTP 유실 또는 소스 없을 때 snd_pcm_readi 무한 블로킹 방지.
         * poll 타임아웃 50ms — 소스 없으면 그냥 skip (direct_cap은 0 유지).
         * drop+prepare+start는 절대 하지 않음 — 재시작 시 데이터 누적 → 즉시 EPIPE 유발. */
        if (d->is_ravenna) {
            struct pollfd pfds[4];
            int npfds = snd_pcm_poll_descriptors(pcm, pfds, 4);
            if (npfds > 0) {
                int ready = poll(pfds, (nfds_t)npfds, 50);
                if (d->quit_cap || g_quit) break;
                if (ready == 0) {
                    /* 타임아웃: 소스 없음 또는 PTP 미잠금 — 조용히 skip */
                    continue;
                }
                if (ready < 0) {
                    if (errno != EINTR) {
                        snd_pcm_recover(pcm, -EPIPE, 1);
                        snd_pcm_start(pcm);
                    }
                    continue;
                }
            }
        }

        snd_pcm_sframes_t n = snd_pcm_readi(pcm, ibuf, (snd_pcm_uframes_t)d->period);
        if (n == -EPIPE) {
            /* 오버런: recover 후 재시작.
             * RAVENNA: in_ring은 그대로 유지 — 캡처된 유효 데이터 손실 방지.
             * accum만 리셋하여 eventfd 타이밍 재동기화. */
            if (d->is_ravenna && cap_err_count++ < 3)
                fprintf(stderr, "[aoip_engine] cap %s: xrun (overrun), recovering\n", d->name);
            snd_pcm_recover(pcm, n, 1);
            if (d->is_ravenna) {
                snd_pcm_start(pcm);
                d->ravenna_accum = 0;
            }
            continue;
        }
        if (n == -ESTRPIPE) {
            while (!g_quit && snd_pcm_resume(pcm) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(pcm);
            if (d->is_ravenna) {
                snd_pcm_start(pcm);
                d->ravenna_accum = 0;
            }
            continue;
        }
        if (n == -EIO) {
            if (d->is_ravenna) {
                /* PTP 미잠금 또는 소스 없음.
                 * ptp_locked=0 → DSP가 이 장치를 뮤트.
                 * in_ring 리셋: EIO 중 쌓인 쓰레기 데이터 제거 (DSP가 읽지 않으므로 안전). */
                if (cap_err_count++ == 0) {
                    atomic_store_explicit(&d->ravenna_ptp_locked, 0, memory_order_release);
                    rb_reset(&d->in_ring);
                    fprintf(stderr, "[aoip_engine] cap %s: EIO (PTP not locked), muting\n", d->name);
                }
                d->ravenna_accum = 0;
                usleep(50000);
            } else {
                if (cap_err_count++ == 0)
                    fprintf(stderr, "[aoip_engine] cap %s: EIO, host stream not active\n", d->name);
                snd_pcm_prepare(pcm);
                usleep(100000);
            }
            continue;
        }
        if (n < 0) {
            if (cap_err_count++ == 0)
                fprintf(stderr, "[aoip_engine] cap %s: %s\n", d->name, snd_strerror((int)n));
            snd_pcm_close(pcm); pcm = NULL;
            rb_reset(&d->in_ring);
            if (d->is_ravenna) d->ravenna_accum = 0;
            while (!d->quit_cap && !g_quit) {
                for (int _i = 0; _i < 5 && !d->quit_cap && !g_quit; _i++) usleep(100000);
                if (d->quit_cap || g_quit) break;
                pcm = alsa_open(d->dev, SND_PCM_STREAM_CAPTURE,
                                d->rate, d->period, d->nperiods, d->channels);
                if (pcm) { cap_err_count = 0; break; }
            }
            continue;
        }
        /* 에러에서 복귀: in_ring 리셋 후 언뮤트 (PTP 재잠금 또는 소스 복귀) */
        if (d->is_ravenna && cap_err_count > 0 &&
            !atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_relaxed)) {
            rb_reset(&d->in_ring);
            atomic_store_explicit(&d->ravenna_ptp_locked, 1, memory_order_release);
            fprintf(stderr, "[aoip_engine] cap %s: PTP locked, unmuting capture\n", d->name);
        }
        cap_err_count = 0;
        for (int i = 0; i < (int)n * d->channels; i++)
            fbuf[i] = (float)ibuf[i] * (1.0f / 2147483648.0f);

        /* 모든 장치: in_ring에 기록 */
        int written = rb_write(&d->in_ring, fbuf, (int)n);

        /* RAVENNA: htstamp 갱신 (DSP 클럭 신호 없음 — hw:aoip가 DSP 마스터) */
        if (d->is_ravenna) {
            atomic_fetch_add_explicit(&g_ravenna_frames, (int64_t)written, memory_order_relaxed);
            {
                snd_pcm_uframes_t _avail;
                struct timespec   _hts;
                if (snd_pcm_htimestamp(pcm, &_avail, &_hts) == 0 && _hts.tv_sec > 0) {
                    int64_t _ns = (int64_t)_hts.tv_sec * 1000000000LL + _hts.tv_nsec;
                    atomic_store_explicit(&g_ravenna_hts_ns, _ns, memory_order_release);
                }
            }
        }

        /* hw:aoip (is_i2s=1): DSP 틱 신호 + htstamp 갱신 */
        if (d->is_i2s) {
            d->ravenna_accum += written;
            if (d->ravenna_accum >= g_period_frames && g_dsp_clock_fd >= 0) {
                d->ravenna_accum -= g_period_frames;
                uint64_t val = 1;
                (void)write(g_dsp_clock_fd, &val, sizeof(val));
            }
            atomic_fetch_add_explicit(&g_aoip_frames, (int64_t)written, memory_order_relaxed);
            {
                snd_pcm_uframes_t _avail;
                struct timespec   _hts;
                if (snd_pcm_htimestamp(pcm, &_avail, &_hts) == 0 && _hts.tv_sec > 0) {
                    int64_t _ns = (int64_t)_hts.tv_sec * 1000000000LL + _hts.tv_nsec;
                    atomic_store_explicit(&g_aoip_hts_ns, _ns, memory_order_release);
                }
            }
        }
    }

    free(ibuf); free(fbuf);
    if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); }
    return NULL;
}

/* ── RAVENNA 하드웨어 고정 주기 ────────────────────────────────────
 * RAVENNA ALSA 드라이버는 AES67 1ms 패킷(a=ptime:1) 기준으로
 * 항상 period=48 을 강제한다.  DSP period(g_period_frames)가 달라도
 * ALSA open/write 는 반드시 48 프레임 단위로 수행해야 한다.        */
#define RAVENNA_HW_PERIOD 48

/* ── ALSA 재생 스레드 ────────────────────────────────────────────── */
static void *alsa_playback_thread(void *arg)
{
    Device *d = (Device *)arg;

    struct sched_param sp = { .sched_priority = d->thread_priority };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* RAVENNA는 hw 주기 48 고정; 일반 장치는 설정값 그대로 */
    int hw_period = d->is_ravenna ? RAVENNA_HW_PERIOD : d->period;

    snd_pcm_t *pcm = NULL;
    while (!d->quit_play && !g_quit) {
        pcm = alsa_open(d->dev, SND_PCM_STREAM_PLAYBACK,
                        d->rate, hw_period, d->nperiods, d->channels);
        if (pcm) break;
        fprintf(stderr, "[aoip_engine] play %s: open failed, retry in 2s\n", d->name);
        for (int _i = 0; _i < 20 && !d->quit_play && !g_quit; _i++) usleep(100000);
    }
    if (!pcm) return NULL;

    float   *fbuf = malloc((size_t)(d->period * d->channels) * sizeof(float));
    int32_t *ibuf = malloc((size_t)(d->period * d->channels) * sizeof(int32_t));
    int play_err_count = 0;

    /* RAVENNA: 3 DSP period(≈6ms) — PTP 도메인이 동일하므로 과도한 버퍼 불필요.
     * I2S/USB: PREBUF_FRAMES(42ms) — SRC 워밍업 + 클럭 도메인 지터 흡수. */
    const int prebuf = d->is_ravenna ? (g_period_frames * 3) : PREBUF_FRAMES;

    while (!d->quit_play && !g_quit && rb_avail(&d->out_ring) < prebuf)
        usleep(1000);

    while (!d->quit_play && !g_quit) {
        if (!rb_read(&d->out_ring, fbuf, d->period)) {
            memset(fbuf, 0, (size_t)(d->period * d->channels) * sizeof(float));
        }
        for (int i = 0; i < d->period * d->channels; i++) {
            float v = fbuf[i];
            if (v >  1.0f) v =  1.0f;
            if (v < -1.0f) v = -1.0f;
            ibuf[i] = (int32_t)(v * 2147483647.0f);
        }

        /* RAVENNA: DSP period(예: 96) 분량을 hw_period(48) 단위로 분할 write.
         * 일반 장치: 한 번에 write. */
        snd_pcm_sframes_t n;
        if (d->is_ravenna) {
            n = 0;
            for (int off = 0; off < d->period && n >= 0; off += hw_period) {
                snd_pcm_sframes_t r = snd_pcm_writei(
                    pcm, ibuf + off * d->channels,
                    (snd_pcm_uframes_t)hw_period);
                if (r < 0) { n = r; break; }
                n += r;
            }
        } else {
            n = snd_pcm_writei(pcm, ibuf, (snd_pcm_uframes_t)d->period);
        }

        if (n == -EPIPE) {
            fprintf(stderr, "[aoip_engine] play %s: xrun (underrun)\n", d->name);
            rb_reset(&d->out_ring);
            snd_pcm_recover(pcm, (int)n, 1);
        } else if (n == -ESTRPIPE) {
            rb_reset(&d->out_ring);
            while (!g_quit && snd_pcm_resume(pcm) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(pcm);
        } else if (n == -EIO) {
            rb_reset(&d->out_ring);
            if (d->is_ravenna) {
                /* PTP 미잠금: PCM close → 재오픈 주기로 잠금 확인 */
                if (play_err_count++ == 0)
                    fprintf(stderr, "[aoip_engine] play %s: EIO (PTP not locked), pausing\n",
                            d->name);
                snd_pcm_close(pcm); pcm = NULL;
                /* PTP 잠금 대기: 5초마다 재오픈 시도 */
                while (!d->quit_play && !g_quit) {
                    for (int _i = 0; _i < 50 && !d->quit_play && !g_quit; _i++) usleep(100000);
                    if (d->quit_play || g_quit) break;
                    pcm = alsa_open(d->dev, SND_PCM_STREAM_PLAYBACK,
                                    d->rate, hw_period, d->nperiods, d->channels);
                    if (pcm) {
                        /* 1 frame write 시도 → EIO면 PTP 아직 미잠금 */
                        static const int32_t probe_buf[MAX_CH] = {0};
                        snd_pcm_sframes_t probe = snd_pcm_writei(pcm, probe_buf, 1);
                        if (probe >= 0 || probe == -EAGAIN) {
                            /* PTP 잠금 확인.
                             * probe write가 PCM을 RUNNING으로 전환했으므로
                             * prepare로 리셋 후 prebuf 대기 — 즉시 xrun 방지 */
                            fprintf(stderr, "[aoip_engine] play %s: PTP locked, resuming\n",
                                    d->name);
                            play_err_count = 0;
                            snd_pcm_prepare(pcm);
                            rb_reset(&d->out_ring);
                            break;
                        } else {
                            snd_pcm_close(pcm); pcm = NULL;
                        }
                    }
                }
                if (!pcm && !d->quit_play && !g_quit) continue;
            } else {
                /* UAC2 가젯: 호스트 스트림 미활성 */
                if (play_err_count++ == 0)
                    fprintf(stderr, "[aoip_engine] play %s: EIO, host stream not active\n",
                            d->name);
                snd_pcm_prepare(pcm);
                usleep(100000);
            }
        } else if (n < 0) {
            play_err_count++;
            if (play_err_count == 1)
                fprintf(stderr, "[aoip_engine] play %s: %s (will suppress repeats)\n",
                        d->name, snd_strerror((int)n));
            snd_pcm_close(pcm); pcm = NULL;
            rb_reset(&d->out_ring);
            while (!d->quit_play && !g_quit) {
                for (int _i = 0; _i < 5 && !d->quit_play && !g_quit; _i++) usleep(100000);
                if (d->quit_play || g_quit) break;
                pcm = alsa_open(d->dev, SND_PCM_STREAM_PLAYBACK,
                                d->rate, hw_period, d->nperiods, d->channels);
                if (pcm) {
                    play_err_count = 0;
                    break;
                }
            }
        } else {
            play_err_count = 0;
        }
    }

    free(fbuf); free(ibuf);
    if (pcm) snd_pcm_close(pcm);
    return NULL;
}

/* ── device_start ────────────────────────────────────────────────── */
void device_start(Device *d)
{
    d->enabled = 1;
    d->quit_cap = d->quit_play = 0;

    if (d->mode != 2) {  /* capture */
        rb_init(&d->in_ring, RING_FRAMES, d->channels);
        d->ravenna_accum = 0;
        pthread_create(&d->cap_tid, NULL, alsa_capture_thread, d);
    }
    if (d->mode != 1) {  /* playback */
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
        if (d->in_ring.buf)  { free(d->in_ring.buf);  d->in_ring.buf  = NULL; }
    }
    if (d->mode != 1) {
        d->quit_play = 1;
        pthread_join(d->play_tid, NULL);
        if (d->out_ring.buf) { free(d->out_ring.buf); d->out_ring.buf = NULL; }
    }
    printf("bridge:%s:stopped\n", d->name);
    fflush(stdout);
}
