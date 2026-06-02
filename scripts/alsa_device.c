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
#include <stdbool.h>
#include <stdatomic.h>
#include <math.h>
#include <poll.h>
#include <sched.h>
#include <sys/eventfd.h>
#include <time.h>
#include "include/alsa_device.h"
#include "include/clk2.h"

/* PTP 재잠금/시작 후 언뮤트까지 쌓아야 할 최소 프레임 수 (3 DSP 주기) */
#define RAVENNA_LOCK_PREBUF (g_period_frames * 3)

/* aoip_engine.c 가 소유하는 전역 플래그 */
extern _Atomic int  g_quit;
extern int          g_period_frames;

/* RAVENNA 클럭 마스터용 eventfd — aoip_engine.c 에서 생성, 여기서 신호 */
extern int          g_dsp_clock_fd;

/* hw:aoip ↔ hw:RAVENNA 클럭 비교 — clk2.c 소유 */
extern _Atomic int64_t  g_aoip_frames;
extern _Atomic int64_t  g_aoip_hts_ns;
extern _Atomic uint32_t g_aoip_seq;
extern _Atomic int64_t  g_ravenna_frames;
extern _Atomic int64_t  g_ravenna_hts_ns;
extern _Atomic uint32_t g_ravenna_seq;

/* RAVENNA ALSA 드라이버 고정 hw 주기 (AES67 1ms 패킷 = 48 프레임) */
#define RAVENNA_HW_PERIOD 48

/* ── 변환 헬퍼 (-march=armv8-a+simd -ftree-vectorize 로 NEON 자동 벡터화) ── */
#define SLEEP_INTERRUPTIBLE(ms, quit_flag) \
    do { for (int _s = 0; _s < (ms)/100 && !(quit_flag) && !g_quit; _s++) usleep(100000); } while(0)

static inline void i32_to_f32_block(const int32_t *src, float *dst, int n)
{
    for (int i = 0; i < n; i++)
        dst[i] = (float)src[i] * (1.0f / 2147483648.0f);
}

static inline void f32_clamp_to_i32_block(const float *src, int32_t *dst, int n)
{
    for (int i = 0; i < n; i++) {
        float v = src[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        dst[i] = (int32_t)(v * 2147483647.0f);
    }
}

/* frames 증가와 htstamp 갱신을 seqlock으로 묶어 reader 측 torn read 방지.
 * reader는 seq를 acquire-load → 짝수 확인 → 두 값 load → seq 재확인 한다. */
static inline void clk2_writer_commit(snd_pcm_t *pcm,
                                      _Atomic uint32_t *seq,
                                      _Atomic int64_t  *frames,
                                      _Atomic int64_t  *hts_ns,
                                      int64_t add_frames)
{
    snd_pcm_uframes_t avail;
    struct timespec   hts;
    int64_t new_hts_ns = 0;
    if (snd_pcm_htimestamp(pcm, &avail, &hts) == 0 && hts.tv_sec > 0)
        new_hts_ns = (int64_t)hts.tv_sec * 1000000000LL + hts.tv_nsec;

    /* seq: even → odd (mutation in progress) */
    atomic_fetch_add_explicit(seq, 1, memory_order_release);
    atomic_fetch_add_explicit(frames, add_frames, memory_order_relaxed);
    if (new_hts_ns)
        atomic_store_explicit(hts_ns, new_hts_ns, memory_order_relaxed);
    /* seq: odd → even (commit) */
    atomic_fetch_add_explicit(seq, 1, memory_order_release);
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
    if ((err = snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED)) < 0) {
        fprintf(stderr, "[aoip_engine] alsa_open %s: RW_INTERLEAVED not supported: %s\n",
                dev, snd_strerror(err));
        snd_pcm_close(pcm); return NULL;
    }
    if ((err = snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE)) < 0) {
        fprintf(stderr, "[aoip_engine] alsa_open %s: S32_LE not supported: %s\n",
                dev, snd_strerror(err));
        snd_pcm_close(pcm); return NULL;
    }
    if ((err = snd_pcm_hw_params_set_channels(pcm, hw, (unsigned)ch)) < 0) {
        fprintf(stderr, "[aoip_engine] alsa_open %s: %dch not supported: %s\n",
                dev, ch, snd_strerror(err));
        snd_pcm_close(pcm); return NULL;
    }
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
    if ((err = snd_pcm_sw_params(pcm, sw)) < 0)
        fprintf(stderr, "[aoip_engine] alsa_open %s: sw_params failed: %s\n",
                dev, snd_strerror(err));

    snd_pcm_prepare(pcm);
    return pcm;
}


/* ── ALSA 캡처 스레드 ────────────────────────────────────────────── */
static void *alsa_capture_thread(void *arg)
{
    Device *d = (Device *)arg;

    struct sched_param sp = { .sched_priority = d->thread_priority };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    pin_to_cpu(2);

    snd_pcm_t *pcm = NULL;
    while (!d->quit_cap && !g_quit) {
        pcm = alsa_open(d->dev, SND_PCM_STREAM_CAPTURE,
                        d->rate, d->period, d->nperiods, d->channels);
        if (pcm) break;
        fprintf(stderr, "[aoip_engine] cap %s: open failed, retry in 2s\n", d->name);
        SLEEP_INTERRUPTIBLE(2000, d->quit_cap);
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
        /* RAVENNA stale drain: 이전 인스턴스가 SIGKILL 등으로 비정상 종료되었을 때
         * driver ring에 누적된 데이터를 따라잡지 않으면 시작 직후 EPIPE(overrun) 폭풍 →
         * snd_pcm_recover()가 read pointer를 frame 경계가 아닌 곳으로 점프시킬 수 있어
         * 채널/샘플 정렬이 깨짐(외계인 소리). avail > period 면 미리 readi+discard. */
        snd_pcm_sframes_t avail = snd_pcm_avail(pcm);
        if (avail > (snd_pcm_sframes_t)d->period) {
            fprintf(stderr, "[aoip_engine] cap %s: stale ring=%ldfr, draining...\n",
                    d->name, (long)avail);
            int32_t *junk = malloc((size_t)(d->period * d->channels) * sizeof(int32_t));
            int drained = 0, guard = 64;
            while (junk && guard-- > 0) {
                avail = snd_pcm_avail(pcm);
                if (avail <= (snd_pcm_sframes_t)d->period) break;
                snd_pcm_sframes_t r = snd_pcm_readi(pcm, junk, (snd_pcm_uframes_t)d->period);
                if (r < 0) {
                    snd_pcm_recover(pcm, (int)r, 1);
                    snd_pcm_start(pcm);
                    break;
                }
                drained += (int)r;
            }
            free(junk);
            fprintf(stderr, "[aoip_engine] cap %s: drained %dfr, remaining=%ldfr\n",
                    d->name, drained, (long)snd_pcm_avail(pcm));
        }
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
                    /* 타임아웃: 소스 없음 또는 PTP 미잠금.
                     * ptp_locked=1 이면 홀드오버: 50ms 분량 무음을 in_ring에 공급하여
                     * DSP 버퍼를 유지한다. PTP가 RAVENNA_HOLDOVER_FRAMES 안에 복귀하면
                     * 뮤트/리셋 없이 오디오가 재개된다. */
                    if (atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_relaxed) &&
                        d->ravenna_holdover_frames < RAVENNA_HOLDOVER_FRAMES) {
                        if (d->ravenna_holdover_frames == 0)
                            fprintf(stderr, "[aoip_engine] cap %s: poll timeout, holdover start (%dms max)\n",
                                    d->name, RAVENNA_HOLDOVER_FRAMES * 1000 / SAMPLE_RATE);
                        int feed = SAMPLE_RATE / 20; /* 50ms */
                        if (d->ravenna_holdover_frames + feed > RAVENNA_HOLDOVER_FRAMES)
                            feed = RAVENNA_HOLDOVER_FRAMES - d->ravenna_holdover_frames;
                        float sil[RAVENNA_HW_PERIOD * MAX_CH];
                        memset(sil, 0, sizeof(float) * RAVENNA_HW_PERIOD * d->channels);
                        for (int _i = 0; _i < feed / RAVENNA_HW_PERIOD; _i++)
                            rb_write(&d->in_ring, sil, RAVENNA_HW_PERIOD);
                        d->ravenna_holdover_frames += feed;
                        if (d->ravenna_holdover_frames >= RAVENNA_HOLDOVER_FRAMES) {
                            fprintf(stderr, "[aoip_engine] cap %s: holdover expired, muting\n", d->name);
                            atomic_store_explicit(&d->ravenna_ptp_locked, 0, memory_order_release);
                            atomic_store_explicit(&d->ravenna_flush, 1, memory_order_release);
                            rb_reset(&d->in_ring);
                            d->ravenna_prebuf_count = 0;
                            d->ravenna_phase2_printed = 0;
                        }
                    }
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
            /* RT 스레드 내 fprintf 지연 포함 전체 복구 시간 측정 */
            struct timespec _t0, _t1;
            clock_gettime(CLOCK_MONOTONIC, &_t0);
            if (d->is_ravenna && cap_err_count++ < 10)
                fprintf(stderr, "[aoip_engine] cap %s: xrun (overrun), recovering\n", d->name);
            snd_pcm_recover(pcm, n, 1);
            if (d->is_ravenna) snd_pcm_start(pcm);
            clock_gettime(CLOCK_MONOTONIC, &_t1);
            long _us = (long)((_t1.tv_sec  - _t0.tv_sec)  * 1000000L
                            + (_t1.tv_nsec - _t0.tv_nsec) / 1000L);
            if (_us > 500)
                fprintf(stderr, "[aoip_engine] cap %s: xrun recovery (log+IOCTL) %ldus\n",
                        d->name, _us);
            if (d->is_i2s) {
                /* xrun 후 partial-fill 슬롯 폐기: 미리셋 안 하면 샘플 정렬 깨짐 */
                d->i2s_cap_ptrs[0] = NULL;
                d->i2s_cap_fill    = 0;
            }
            if (d->is_ravenna) {
                d->ravenna_accum = 0;
                d->ravenna_prebuf_count = 0;
                d->ravenna_holdover_frames = 0;
                /* 입력 링버퍼·SRC·PI 전부 리셋: xrun으로 데이터 불연속 발생 */
                rb_reset(&d->in_ring);
                if (d->cap_src) {
                    src_reset(d->cap_src);
                    /* xrun 후 PI를 실측 ratio hint로 재초기화 — ratio=1.0 튐 방지 */
                    double hint = atomic_load_explicit(&g_ravenna_ratio_hint, memory_order_relaxed);
                    d->cap_pi.ratio = hint;
                    d->cap_pi.integ = 0.0;
                    d->cap_pi.smooth = 0.0;
                }
                /* ptp_locked=0 → prebuffer 경로 재진입, DSP 입력 뮤트 */
                atomic_store_explicit(&d->ravenna_ptp_locked, 0, memory_order_release);
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
                 * ptp_locked=1 이면 홀드오버: RAVENNA_HW_PERIOD 무음을 in_ring에 공급.
                 * 1ms 간격으로 호출되어 홀드오버 기간 동안 DSP 버퍼를 유지한다.
                 * 홀드오버 만료 또는 이미 뮤트 상태면 기존 방식으로 처리. */
                if (atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_relaxed) &&
                    d->ravenna_holdover_frames < RAVENNA_HOLDOVER_FRAMES) {
                    if (d->ravenna_holdover_frames == 0)
                        fprintf(stderr, "[aoip_engine] cap %s: EIO holdover start (%dms max)\n",
                                d->name, RAVENNA_HOLDOVER_FRAMES * 1000 / SAMPLE_RATE);
                    float sil[RAVENNA_HW_PERIOD * MAX_CH];
                    memset(sil, 0, sizeof(float) * RAVENNA_HW_PERIOD * d->channels);
                    rb_write(&d->in_ring, sil, RAVENNA_HW_PERIOD);
                    d->ravenna_holdover_frames += RAVENNA_HW_PERIOD;
                    usleep(1000); /* 1ms: RAVENNA_HW_PERIOD(48) 프레임 주기 시뮬레이션 */
                } else {
                    /* 홀드오버 만료 or 이미 ptp_locked=0 */
                    if (atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_relaxed)) {
                        /* 홀드오버 만료: 최초 1회만 처리 */
                        fprintf(stderr, "[aoip_engine] cap %s: holdover expired (EIO), muting\n", d->name);
                        atomic_store_explicit(&d->ravenna_ptp_locked, 0, memory_order_release);
                        atomic_store_explicit(&d->ravenna_flush, 1, memory_order_release);
                        rb_reset(&d->in_ring);
                        d->ravenna_prebuf_count = 0;
                        d->ravenna_phase2_printed = 0;
                        cap_err_count = 1;
                    } else if (cap_err_count++ == 0) {
                        fprintf(stderr, "[aoip_engine] cap %s: EIO (PTP not locked), muting\n", d->name);
                    }
                    d->ravenna_accum = 0;
                    usleep(50000);
                }
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
                SLEEP_INTERRUPTIBLE(500, d->quit_cap);
                if (d->quit_cap || g_quit) break;
                pcm = alsa_open(d->dev, SND_PCM_STREAM_CAPTURE,
                                d->rate, d->period, d->nperiods, d->channels);
                if (pcm) { cap_err_count = 0; break; }
            }
            continue;
        }
        /* PTP 잠금/재잠금:
         * Phase 1 — RAVENNA_LOCK_PREBUF 프레임: 캡처 클럭 안정화 확인
         * Phase 2 — SAMPLE_RATE*3 프레임(3초): PTP servo 수렴 대기
         *   (LAN 재연결 시 ptp4l이 재시작되므로 servo 수렴에 충분한 시간 필요)
         * 두 단계 모두 in_ring에 쓰지 않음. 완료 후 ring 초기화 + 언뮤트.
         * ravenna_ptp_locked=1 전환 시 DSP cap_prebuf_ready=0 상태이므로
         * DSP-level prefill이 자동으로 재시작됨. */
        if (d->is_ravenna && (int)n > 0 &&
            !atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_relaxed)) {
            d->ravenna_prebuf_count += (int)n;
            if (d->ravenna_prebuf_count < RAVENNA_LOCK_PREBUF) {
                /* Phase 1: 캡처 prebuffer 중 — in_ring에 쓰지 않고 대기 */
                cap_err_count = 0;
                continue;
            }
            /* Phase 2: 3초 벽시계 대기 (PTP servo 완전 수렴 — EIO/EPIPE 무관) */
            if (!d->ravenna_phase2_printed) {
                struct timespec _ts;
                clock_gettime(CLOCK_MONOTONIC, &_ts);
                d->ravenna_phase2_start_ns = (int64_t)_ts.tv_sec * 1000000000LL + _ts.tv_nsec;
                fprintf(stderr, "[aoip_engine] cap %s: PTP clk stable, waiting 3s before unmute\n", d->name);
                d->ravenna_phase2_printed = 1;
            }
            {
                struct timespec _ts;
                clock_gettime(CLOCK_MONOTONIC, &_ts);
                int64_t now_ns = (int64_t)_ts.tv_sec * 1000000000LL + _ts.tv_nsec;
                if (now_ns - d->ravenna_phase2_start_ns < 3000000000LL) {
                    cap_err_count = 0;
                    continue;
                }
            }
            /* Phase 2 완료: in_ring 초기화 후 언뮤트 → DSP prefill 재시작 */
            rb_reset(&d->in_ring);
            atomic_store_explicit(&d->ravenna_ptp_locked, 1, memory_order_release);
            d->ravenna_prebuf_count = 0;
            fprintf(stderr, "[aoip_engine] cap %s: PTP locked (3s stable), unmuting → DSP prefill\n", d->name);
        }
        cap_err_count = 0;
        if (d->is_ravenna && d->ravenna_holdover_frames > 0) {
            fprintf(stderr, "[aoip_engine] cap %s: PTP recovered, holdover cleared (%dms)\n",
                    d->name, d->ravenna_holdover_frames * 1000 / SAMPLE_RATE);
            d->ravenna_holdover_frames = 0;
        }

        int written;
        if (d->is_i2s) {
            /* I2S zero-copy: SlotRing에 직접 int32→float + deinterleave */
            bool i2s_do_fill = true;
            if (!d->i2s_cap_ptrs[0]) {
                if (slot_ring_acquire_write(&d->i2s_in_ring, d->i2s_cap_ptrs)) {
                    d->i2s_cap_fill = 0;
                } else {
                    /* 링 풀: DSP가 너무 느림 — 이 ALSA period 드롭 */
                    i2s_do_fill = false;
                }
            }
            if (i2s_do_fill) {
                int frames = (int)n;
                if (d->i2s_cap_fill + frames > g_period_frames)
                    frames = g_period_frames - d->i2s_cap_fill;
                for (int f = 0; f < frames; f++)
                    for (int c = 0; c < d->channels; c++)
                        d->i2s_cap_ptrs[c][d->i2s_cap_fill + f] =
                            (float)ibuf[f * d->channels + c] * (1.0f / 2147483648.0f);
                d->i2s_cap_fill += frames;
                if (d->i2s_cap_fill >= g_period_frames) {
                    slot_ring_commit_write(&d->i2s_in_ring);
                    d->i2s_cap_ptrs[0] = NULL;
                    d->i2s_cap_fill = 0;
                }
            }
            written = (int)n;
        } else {
            i32_to_f32_block(ibuf, fbuf, (int)n * d->channels);
            /* RAVENNA/기타: 기존 RingBuf에 기록 */
            written = rb_write(&d->in_ring, fbuf, (int)n);
        }

        /* RAVENNA: htstamp 갱신 (DSP 클럭 신호 없음 — hw:aoip가 DSP 마스터) */
        if (d->is_ravenna) {
            clk2_writer_commit(pcm, &g_ravenna_seq,
                               &g_ravenna_frames, &g_ravenna_hts_ns,
                               (int64_t)written);
        }

        /* hw:aoip (is_i2s=1): DSP 틱 신호 + htstamp 갱신 */
        if (d->is_i2s) {
            d->ravenna_accum += written;
            if (d->ravenna_accum >= g_period_frames && g_dsp_clock_fd >= 0) {
                d->ravenna_accum -= g_period_frames;
                if (d->ravenna_prebuf_count < RAVENNA_LOCK_PREBUF) {
                    /* 시작 prebuffer 중 — eventfd 지연, 버퍼 누적 */
                    d->ravenna_prebuf_count += g_period_frames;
                    if (d->ravenna_prebuf_count >= RAVENNA_LOCK_PREBUF) {
                        rb_reset(&d->in_ring);
                        fprintf(stderr, "[aoip_engine] cap %s: startup prebuffer done, starting DSP clock\n",
                                d->name);
                    }
                } else {
                    uint64_t val = 1;
                    (void)write(g_dsp_clock_fd, &val, sizeof(val));
                }
            }
            clk2_writer_commit(pcm, &g_aoip_seq,
                               &g_aoip_frames, &g_aoip_hts_ns,
                               (int64_t)written);
        }
    }

    free(ibuf); free(fbuf);
    if (pcm) { snd_pcm_drop(pcm); snd_pcm_close(pcm); }
    return NULL;
}

/* ── ALSA 재생 스레드 ────────────────────────────────────────────── */
static void *alsa_playback_thread(void *arg)
{
    Device *d = (Device *)arg;

    struct sched_param sp = { .sched_priority = d->thread_priority };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    pin_to_cpu(2);

    /* RAVENNA는 hw 주기 48 고정; 일반 장치는 설정값 그대로 */
    int hw_period = d->is_ravenna ? RAVENNA_HW_PERIOD : d->period;

    snd_pcm_t *pcm = NULL;
    while (!d->quit_play && !g_quit) {
        pcm = alsa_open(d->dev, SND_PCM_STREAM_PLAYBACK,
                        d->rate, hw_period, d->nperiods, d->channels);
        if (pcm) break;
        fprintf(stderr, "[aoip_engine] play %s: open failed, retry in 2s\n", d->name);
        SLEEP_INTERRUPTIBLE(2000, d->quit_play);
    }
    if (!pcm) return NULL;

    float   *fbuf = malloc((size_t)(d->period * d->channels) * sizeof(float));
    int32_t *ibuf = malloc((size_t)(d->period * d->channels) * sizeof(int32_t));
    int play_err_count = 0;

    /* RAVENNA: 3 DSP period(≈6ms) — PTP 도메인이 동일하므로 과도한 버퍼 불필요.
     * I2S: 마스터 클럭 — 클럭 도메인 교차 없으므로 prebuffer 불필요(0).
     * USB 등 기타: PREBUF_FRAMES(42ms) — SRC 워밍업 + 클럭 도메인 지터 흡수. */
    const int prebuf = d->is_ravenna ? (g_period_frames * 3)
                     : d->is_i2s    ? 0
                     :                PREBUF_FRAMES;

    if (prebuf > 0) {
        if (d->is_i2s) {
            while (!d->quit_play && !g_quit && slot_ring_avail(&d->i2s_out_ring) < prebuf)
                usleep(1000);
        } else {
            while (!d->quit_play && !g_quit && rb_avail(&d->out_ring) < prebuf)
                usleep(1000);
        }
    }

    while (!d->quit_play && !g_quit) {
        /* PTP 언락 신호: out_ring 플러시 → 즉시 무음 출력 */
        if (d->is_ravenna &&
            atomic_load_explicit(&d->ravenna_flush, memory_order_acquire)) {
            atomic_store_explicit(&d->ravenna_flush, 0, memory_order_relaxed);
            rb_reset(&d->out_ring);
            atomic_store_explicit(&d->play_src_reset, 1, memory_order_release);
            fprintf(stderr, "[aoip_engine] play %s: PTP unlock, flushing output buffer\n",
                    d->name);
        }

        /* I2S: SlotRing 슬롯(g_period_frames)을 hw_period 청크로 나누어 write.
         * RAVENNA: d->period(=g_period_frames) 분량을 hw_period 단위로 분할 write.
         * 일반 장치: 한 번에 write. */
        snd_pcm_sframes_t n;
        if (d->is_i2s) {
            float *rptrs[MAX_CH];
            int total = g_period_frames;
            n = 0;
            if (slot_ring_acquire_read(&d->i2s_out_ring, rptrs)) {
                for (int off = 0; off < total && n >= 0; off += hw_period) {
                    for (int f = 0; f < hw_period; f++)
                        for (int c = 0; c < d->channels; c++) {
                            float v = rptrs[c][off + f];
                            if (v >  1.0f) v =  1.0f;
                            if (v < -1.0f) v = -1.0f;
                            ibuf[f * d->channels + c] = (int32_t)(v * 2147483647.0f);
                        }
                    snd_pcm_sframes_t r = snd_pcm_writei(pcm, ibuf, (snd_pcm_uframes_t)hw_period);
                    if (r < 0) { n = r; break; }
                    n += r;
                }
                slot_ring_consume_read(&d->i2s_out_ring);
            } else {
                memset(ibuf, 0, (size_t)(hw_period * d->channels) * sizeof(int32_t));
                n = snd_pcm_writei(pcm, ibuf, (snd_pcm_uframes_t)hw_period);
            }
        } else if (d->is_ravenna) {
            if (!rb_read(&d->out_ring, fbuf, d->period)) {
                memset(fbuf, 0, (size_t)(d->period * d->channels) * sizeof(float));
            }
            f32_clamp_to_i32_block(fbuf, ibuf, d->period * d->channels);
            n = 0;
            for (int off = 0; off < d->period && n >= 0; off += hw_period) {
                snd_pcm_sframes_t r = snd_pcm_writei(
                    pcm, ibuf + off * d->channels,
                    (snd_pcm_uframes_t)hw_period);
                if (r < 0) { n = r; break; }
                n += r;
            }
        } else {
            if (!rb_read(&d->out_ring, fbuf, d->period)) {
                memset(fbuf, 0, (size_t)(d->period * d->channels) * sizeof(float));
            }
            f32_clamp_to_i32_block(fbuf, ibuf, d->period * d->channels);
            n = snd_pcm_writei(pcm, ibuf, (snd_pcm_uframes_t)d->period);
        }

        if (n == -EPIPE) {
            fprintf(stderr, "[aoip_engine] play %s: xrun (underrun)\n", d->name);
            snd_pcm_recover(pcm, (int)n, 1);
            /* out_ring 데이터는 유효 — 리셋 없이 즉시 재공급하여 재xrun 방지 */
        } else if (n == -ESTRPIPE) {
            while (!g_quit && snd_pcm_resume(pcm) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(pcm);
        } else if (n == -EIO) {
            rb_reset(&d->out_ring);
            atomic_store_explicit(&d->play_src_reset, 1, memory_order_release);
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
                            atomic_store_explicit(&d->play_src_reset, 1, memory_order_release);
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
                SLEEP_INTERRUPTIBLE(500, d->quit_play);
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
    d->quit_cap = d->quit_play = 0;

    if (d->mode != 2) {  /* capture */
        if (d->is_i2s) {
            slot_ring_init(&d->i2s_in_ring, SLOT_COUNT, MAX_PERIOD_FRAMES, d->channels);
            d->i2s_cap_ptrs[0] = NULL;
            d->i2s_cap_fill    = 0;
            d->i2s_in_acquired = 0;
        } else {
            rb_init(&d->in_ring, RING_FRAMES, d->channels);
        }
        d->ravenna_accum           = 0;
        d->ravenna_prebuf_count    = 0;
        d->ravenna_holdover_frames = 0;
        d->ravenna_phase2_start_ns = 0;
        d->ravenna_phase2_printed  = 0;
        pthread_create(&d->cap_tid, NULL, alsa_capture_thread, d);
    }
    if (d->mode != 1) {  /* playback */
        if (d->is_i2s) {
            slot_ring_init(&d->i2s_out_ring, SLOT_COUNT, MAX_PERIOD_FRAMES, d->channels);
        } else {
            rb_init(&d->out_ring, RING_FRAMES, d->channels);
        }
        pthread_create(&d->play_tid, NULL, alsa_playback_thread, d);
    }
    /* 링/슬롯 초기화 및 스레드 시작 완료 후에 enabled=1 설정
     * DSP 스레드가 미초기화 링에 접근하는 레이스를 방지 */
    atomic_thread_fence(memory_order_release);
    d->enabled = 1;
    printf("bridge:%s:ready\n", d->name);
    fflush(stdout);
}

/* ── device_stop ─────────────────────────────────────────────────── */
void device_stop(Device *d)
{
    /* DSP 스레드가 이 장치를 건너뛰도록 먼저 비활성화.
     * DSP 루프 최대 1주기(~11ms)가 끝날 때까지 대기 후 메모리 해제. */
    d->enabled = 0;
    atomic_thread_fence(memory_order_seq_cst);
    usleep(25000);  /* ≥2 DSP 주기 (~21ms) */

    if (d->mode != 2) {
        d->quit_cap = 1;
        pthread_join(d->cap_tid, NULL);
        if (d->is_i2s) {
            slot_ring_destroy(&d->i2s_in_ring);
        } else {
            if (d->in_ring.buf) { free(d->in_ring.buf); d->in_ring.buf = NULL; }
        }
    }
    if (d->mode != 1) {
        d->quit_play = 1;
        pthread_join(d->play_tid, NULL);
        if (d->is_i2s) {
            slot_ring_destroy(&d->i2s_out_ring);
        } else {
            if (d->out_ring.buf) { free(d->out_ring.buf); d->out_ring.buf = NULL; }
        }
    }
    printf("bridge:%s:stopped\n", d->name);
    fflush(stdout);
}
