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
#include <fcntl.h>
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

/* hw:aoip(I2S) 시작 prebuffer — DSP 클럭 eventfd 신호 전 누적 프레임 수 (3 DSP 주기) */
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

/* 마지막 hw_ptr 갱신 시각(ns, CLOCK_MONOTONIC)과 그 시점 avail. 실패 시 0. */
static inline int64_t pcm_hts_ns(snd_pcm_t *pcm, snd_pcm_uframes_t *avail)
{
    struct timespec hts;
    if (snd_pcm_htimestamp(pcm, avail, &hts) == 0 && hts.tv_sec > 0)
        return (int64_t)hts.tv_sec * 1000000000LL + hts.tv_nsec;
    return 0;
}

static inline int64_t mono_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

/* frames 증가와 htstamp 갱신을 seqlock으로 묶어 reader 측 torn read 방지.
 * reader는 seq를 acquire-load → 짝수 확인 → 두 값 load → seq 재확인 한다. */
static inline void clk2_writer_commit(_Atomic uint32_t *seq,
                                      _Atomic int64_t  *frames,
                                      _Atomic int64_t  *hts_ns,
                                      int64_t add_frames,
                                      int64_t new_hts_ns)
{
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

    /* I2S precal 은 RAVENNA RTP 로딩(SRC ready) 직후 dsp_io 에서 1회 적용 — alsa_open
     * 시점은 clk_i2s mux 가 pll_audio 로 전환되기 전이라 PLL resolve 가 실패함. */

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


/* ── RAVENNA 언뮤트 게이트 ───────────────────────────────────────────
 *
 * 드라이버(ravenna-alsa-lkm audio_driver.c)의 캡처 읽기 위치 capture_buffer_pos 는
 * PCM prepare 시점에만 GlobalSAC mod 링길이 로 맞춰지고, 이후엔 TIC 인터럽트마다
 * +48 씩 증가만 한다. PTP 언락 동안 드라이버는 인터럽트를 멈추지만 GlobalSAC 는
 * 계속 흐르므로, 재락 후 읽기 위치가 RTP 쓰기 위치와 (점프량 mod 링길이) 만큼
 * 어긋난 채 굳는다 → 재부팅(=재prepare) 전까지 안 풀리던 비트 어긋남.
 * 그래서 언뮤트 직전엔 반드시 drop/prepare/start 로 읽기 위치를 재동기화한다.
 *
 *   MEASURE ─(연속 양호 RG_MEASURE_WIN + 구간 평균 기울기 양호)→ prepare 재동기화 → VERIFY
 *   VERIFY  ─(연속 양호 RG_VERIFY_WIN)→ LIVE(언뮤트)     (불량 → MEASURE)
 *   LIVE    ─(느슨한 임계 초과 / PTP 언락 / xrun)→ 뮤트 → MEASURE
 *
 * 측정: hw 위치(읽은 프레임+avail) 와 그 htstamp 로 위상 e = Δpos − Δt·Fs 를 쌓는다.
 * 인터럽트 지연은 e 를 낮추기만 하므로 1s 윈도우의 상위값(3번째)은 지터에 둔감하고,
 * TIC drop·재락·GM 스텝 같은 진짜 위상 변화는 그대로 드러난다.
 *   rate = 윈도우 간 위상 기울기 (RAVENNA 클럭 vs CLOCK_MONOTONIC, ppm) — GM 슬루
 *   jump = 기울기 변화량 (프레임)                                      — 스텝/급변
 * 실측(2026-09-17): 드라이버 TIC 타이머가 100µs(≈4.8fr) 단위로 흔들려 정상 상태에서도
 * 윈도우당 rate ±190ppm, jump ≤9.6fr 가 나온다. 그래서 윈도우 단위 rate 는 큰 이상만 거르고,
 * 클럭 기울기 판정은 연속 양호 구간 전체 평균(양자화 노이즈가 구간 길이로 나눠짐)으로 한다. */
#define RG_WIN_NS            1000000000LL
#define RG_MEASURE_WIN       10        /* 재동기화 전 연속 양호 윈도우 (≈10s) */
#define RG_VERIFY_WIN        3         /* 재동기화 후 연속 양호 윈도우 (≈3s) */
#define RG_WIN_PPM           300.0     /* 윈도우 1개 기울기 한계 — 양자화 노이즈 ≤200ppm 위, GM 슬루 차단 */
#define RG_GATE_JUMP_FR      12.0      /* 게이트 위상 급변 한계 — 양자화 노이즈 ≤9.6fr 위, TIC drop(48fr) 아래 */
#define RG_GATE_AVG_PPM      50.0      /* MEASURE 구간 평균 기울기 한계 */
#define RG_GATE_WANDER_FR    8.0       /* MEASURE 구간 위상의 추세선 대비 최대 이탈 — 양자화 ≈4.8fr,
                                        * 출렁이는 클럭(평균은 상쇄돼도) 은 수십 fr */
#define RG_GATE_RELAX_WIN    60        /* 이만큼 통과 못하면 게이트 임계 한 단계 완화 (최대 2단계) */
#define RG_LIVE_PPM          600.0     /* 언뮤트 중 윈도우 기울기 한계 (TIC drop 은 ≈1000ppm) */
#define RG_LIVE_JUMP_FR      24.0      /* 언뮤트 중 위상 급변 한계 — 반 TIC 프레임 (drop=48fr) */
#define RG_RESYNC_LATE_NS    500000LL  /* 마지막 TIC 후 이 시간 넘기면 다음 TIC 과 경합 → 재시도 */
#define RG_RESYNC_MAX_TRIES  20
#define RG_BAD_LOG_EVERY     30        /* 게이트 불량 로그 rate-limit (윈도우) */
#define RG_LIVE_REPORT_WIN   600       /* 언뮤트 중 클럭 통계 보고 주기 (≈10분) */

typedef struct {
    int     anchored;
    int     rate;
    int64_t t0_ns, p0;
    int64_t win_t0_ns;
    double  top[3];        /* 윈도우 내 e 상위 3개 (내림차순) */
    int     win_n;
    int     have_E, have_d;
    double  prev_E, prev_d;
} RavPhase;

typedef struct {
    double  ppm;
    double  jump_fr;
    double  dE_fr;         /* 직전 윈도우 대비 위상 변화 (프레임) */
    int     have_jump;     /* 0 = 기울기 기준 확보 중(워밍업) */
    int     reads;
    int64_t span_ns;
} RavWin;

typedef enum { RG_MEASURE = 0, RG_VERIFY, RG_LIVE } RgState;

typedef struct {
    RgState  st;
    int      good;          /* 연속 양호 윈도우 */
    int      windows;       /* 뮤트 이후 판정한 윈도우 수 (완화 단계 산출) */
    int      relax;         /* 게이트 임계 완화 단계 0..2 */
    int      bad_logged;
    int      want_resync;
    int      resync_tries;
    double   max_ppm, max_jump;
    /* 연속 양호 구간의 누적 위상(fr)·시간(s) — [0] 은 원점. 평균 기울기·위상 이탈 산출 */
    double   streak_P[RG_MEASURE_WIN + 1];
    double   streak_T[RG_MEASURE_WIN + 1];
    double   avg_ppm, wander_fr;
    int64_t  pos;           /* 마지막 prepare/start 이후 읽은 프레임 누적 */
    int      live_win;
    RavPhase ph;
} RavGate;

static void rav_phase_top_insert(RavPhase *ph, double e)
{
    if (e <= ph->top[2]) return;
    if (e > ph->top[0])      { ph->top[2] = ph->top[1]; ph->top[1] = ph->top[0]; ph->top[0] = e; }
    else if (e > ph->top[1]) { ph->top[2] = ph->top[1]; ph->top[1] = e; }
    else                     { ph->top[2] = e; }
}

/* 샘플 1개 투입. 윈도우가 끝나 판정할 값이 나오면 1 반환. */
static int rav_phase_feed(RavPhase *ph, int64_t pos, int64_t t_ns, int rate, RavWin *w)
{
    if (!ph->anchored) {
        memset(ph, 0, sizeof(*ph));
        ph->anchored  = 1;
        ph->rate      = rate;
        ph->t0_ns     = ph->win_t0_ns = t_ns;
        ph->p0        = pos;
        ph->top[0] = ph->top[1] = ph->top[2] = -1e300;
        return 0;
    }
    const double fs_per_ns = (double)ph->rate / 1e9;
    rav_phase_top_insert(ph, (double)(pos - ph->p0) - (double)(t_ns - ph->t0_ns) * fs_per_ns);
    ph->win_n++;

    int64_t span = t_ns - ph->win_t0_ns;
    if (span < RG_WIN_NS) return 0;

    double E = ph->win_n >= 3 ? ph->top[2] : ph->top[0];
    int got = 0;
    if (ph->have_E) {
        double dE    = E - ph->prev_E;
        w->ppm       = dE / ((double)span * fs_per_ns) * 1e6;
        w->have_jump = ph->have_d;
        w->jump_fr   = ph->have_d ? fabs(dE - ph->prev_d) : 0.0;
        w->dE_fr     = dE;
        w->reads     = ph->win_n;
        w->span_ns   = span;
        ph->prev_d = dE;
        ph->have_d = 1;
        got = 1;
    }
    ph->prev_E    = E;
    ph->have_E    = 1;
    ph->win_t0_ns = t_ns;
    ph->win_n     = 0;
    ph->top[0] = ph->top[1] = ph->top[2] = -1e300;
    return got;
}

static const char *rg_judge(const RavWin *w, double ppm_lim, double jump_lim,
                            int rate, int period)
{
    /* 기대 read 수의 절반 미만 = TIC 인터럽트 끊김 */
    if ((double)w->reads * period * 1e9 < (double)w->span_ns * rate * 0.5) return "gap";
    if (fabs(w->ppm) > ppm_lim)                                            return "rate";
    if (w->have_jump && w->jump_fr > jump_lim)                              return "jump";
    return NULL;
}

/* 연속 양호 구간 통계 초기화 */
static void rg_streak_reset(RavGate *g)
{
    g->good = 0;
    g->max_ppm = g->max_jump = 0.0;
    g->streak_P[0] = g->streak_T[0] = 0.0;
}

/* 뮤트 상태에서 측정을 처음부터. pos 는 hw 위치 누적이라 유지. */
static void rg_restart(RavGate *g)
{
    g->st = RG_MEASURE;
    g->windows = g->relax = g->bad_logged = 0;
    g->want_resync = g->resync_tries = 0;
    rg_streak_reset(g);
    g->live_win = 0;
    g->ph.anchored = 0;
}

/* 언뮤트 중이면 뮤트로 전환하고, 어떤 상태든 측정을 처음부터 다시 한다.
 * flush_play: PTP/클럭 원인일 때 AES67 출력 링도 비우도록 재생 스레드에 신호. */
static void rav_mute(Device *d, RavGate *g, const char *why, int flush_play)
{
    if (atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_relaxed)) {
        fprintf(stderr, "[aoip_engine] cap %s: %s → mute, re-measuring clock\n", d->name, why);
        atomic_store_explicit(&d->ravenna_ptp_locked, 0, memory_order_release);
        if (flush_play)
            atomic_store_explicit(&d->ravenna_flush, 1, memory_order_release);
        rb_reset(&d->in_ring);
    }
    rg_restart(g);
}

static void rg_on_window(Device *d, RavGate *g, const RavWin *w)
{
    double jump = w->have_jump ? w->jump_fr : 0.0;

    if (g->st == RG_LIVE) {
        const char *why = rg_judge(w, RG_LIVE_PPM, RG_LIVE_JUMP_FR, d->rate, d->period);
        if (why) {
            char msg[128];
            snprintf(msg, sizeof(msg), "clock %s (rate %+.1fppm, jump %.2ffr, reads %d)",
                     why, w->ppm, jump, w->reads);
            rav_mute(d, g, msg, 1);
            return;
        }
        if (fabs(w->ppm) > g->max_ppm) g->max_ppm = fabs(w->ppm);
        if (jump > g->max_jump)        g->max_jump = jump;
        if (++g->live_win >= RG_LIVE_REPORT_WIN) {
            fprintf(stderr, "[aoip_engine] cap %s: live clock %ds: rate %+.2fppm (max |%.2f|), max jump %.2ffr\n",
                    d->name, g->live_win, w->ppm, g->max_ppm, g->max_jump);
            g->live_win = 0;
            g->max_ppm = g->max_jump = 0.0;
        }
        return;
    }

    if (!w->have_jump) return;   /* 워밍업 */

    /* 오래 통과 못하면 임계를 단계적으로 완화 — 측정 노이즈 과소평가로 영구 뮤트되는 것 방지.
     * 2단계(평균 150ppm / 18fr)여도 LIVE 임계보다 엄격하고 TIC drop(48fr)·GM 슬루는 계속 걸러진다. */
    int relax = ++g->windows / RG_GATE_RELAX_WIN;
    if (relax > 2) relax = 2;
    double avg_lim    = RG_GATE_AVG_PPM * (1.0 + relax);
    double jump_lim   = RG_GATE_JUMP_FR * (1.0 + 0.25 * relax);
    double wander_lim = RG_GATE_WANDER_FR * (1.0 + 0.25 * relax);
    if (relax != g->relax) {
        g->relax = relax;
        fprintf(stderr, "[aoip_engine] cap %s: clock gate not passed for %ds, relaxing to avg %.0fppm / jump %.0ffr / wander %.0ffr\n",
                d->name, g->windows, avg_lim, jump_lim, wander_lim);
    }

    const char *why = rg_judge(w, RG_WIN_PPM, jump_lim, d->rate, d->period);
    if (why) {
        if (g->st == RG_VERIFY) {
            fprintf(stderr, "[aoip_engine] cap %s: resync verify failed: clock %s (rate %+.1fppm, jump %.2ffr) → re-measuring\n",
                    d->name, why, w->ppm, jump);
            g->st = RG_MEASURE;
        } else if (g->bad_logged++ % RG_BAD_LOG_EVERY == 0) {
            fprintf(stderr, "[aoip_engine] cap %s: clock unstable: %s (rate %+.1fppm, jump %.2ffr, reads %d), holding mute\n",
                    d->name, why, w->ppm, jump, w->reads);
        }
        rg_streak_reset(g);
        g->want_resync = g->resync_tries = 0;
        return;
    }

    g->good++;
    if (fabs(w->ppm) > g->max_ppm) g->max_ppm = fabs(w->ppm);
    if (jump > g->max_jump)        g->max_jump = jump;
    if (g->good <= RG_MEASURE_WIN) {
        g->streak_P[g->good] = g->streak_P[g->good - 1] + w->dE_fr;
        g->streak_T[g->good] = g->streak_T[g->good - 1] + (double)w->span_ns / 1e9;
    }

    if (g->st == RG_MEASURE && g->good >= RG_MEASURE_WIN) {
        /* 구간 끝점 추세선: 평균 기울기 + 각 윈도우 위상의 추세선 대비 최대 이탈 */
        const int n = RG_MEASURE_WIN;
        double slope = g->streak_P[n] / g->streak_T[n];          /* fr/s */
        g->avg_ppm   = slope / d->rate * 1e6;
        g->wander_fr = 0.0;
        for (int i = 1; i < n; i++) {
            double r = fabs(g->streak_P[i] - slope * g->streak_T[i]);
            if (r > g->wander_fr) g->wander_fr = r;
        }
        const char *bad = fabs(g->avg_ppm) > avg_lim   ? "drifting"
                        : g->wander_fr     > wander_lim ? "wandering" : NULL;
        if (bad) {
            if (g->bad_logged++ % RG_BAD_LOG_EVERY == 0)
                fprintf(stderr, "[aoip_engine] cap %s: clock %s over %ds (avg rate %+.1fppm, wander %.1ffr), holding mute\n",
                        d->name, bad, n, g->avg_ppm, g->wander_fr);
            rg_streak_reset(g);
            return;
        }
        g->want_resync = 1;
    } else if (g->st == RG_VERIFY && g->good >= RG_VERIFY_WIN) {
        rb_reset(&d->in_ring);
        atomic_store_explicit(&d->ravenna_ptp_locked, 1, memory_order_release);
        atomic_store_explicit(&g_ptp_locked, 1, memory_order_release);
        atomic_fetch_add_explicit(&g_ptp_resync_gen, 1, memory_order_release);
        fprintf(stderr, "[aoip_engine] cap %s: clock verified %ds after resync (max |rate| %.2fppm, max jump %.2ffr), unmuting → DSP prefill\n",
                d->name, g->good, g->max_ppm, g->max_jump);
        g->st = RG_LIVE;
        g->live_win = 0;
        g->max_ppm = g->max_jump = 0.0;
    }
}

/* 드라이버 읽기 위치를 현재 GlobalSAC 에 재동기화 (drop → prepare → start).
 * prepare 와 start 사이에 TIC 이 끼면 읽기 위치가 한 프레임 밀리므로 readi 직후
 * (마지막 TIC 직후) 에만 수행하고, 사후에 경계를 넘겼으면 다음 readi 에서 다시 한다. */
static void rg_try_resync(Device *d, RavGate *g, snd_pcm_t *pcm, int64_t last_tic_ns)
{
    int64_t t_pre = mono_ns();
    if (last_tic_ns > 0 && t_pre - last_tic_ns > RG_RESYNC_LATE_NS &&
        ++g->resync_tries < RG_RESYNC_MAX_TRIES)
        return;

    snd_pcm_drop(pcm);
    int err = snd_pcm_prepare(pcm);
    if (err == 0) err = snd_pcm_start(pcm);
    int64_t late_ns = mono_ns() - last_tic_ns;

    g->pos = 0;
    g->ph.anchored = 0;
    if (err < 0) {
        fprintf(stderr, "[aoip_engine] cap %s: resync prepare/start failed: %s\n",
                d->name, snd_strerror(err));
        rg_restart(g);
        return;
    }
    if (last_tic_ns > 0 && late_ns > RG_RESYNC_LATE_NS &&
        ++g->resync_tries < RG_RESYNC_MAX_TRIES)
        return;   /* want_resync 유지 → 다음 readi 직후 재시도 */

    fprintf(stderr, "[aoip_engine] cap %s: clock stable %ds (avg rate %+.2fppm, wander %.1ffr, max jump %.2ffr) → driver read pointer resync (%lldus after TIC, tries %d), verifying\n",
            d->name, g->good, g->avg_ppm, g->wander_fr, g->max_jump,
            (long long)(late_ns / 1000), g->resync_tries);
    g->st = RG_VERIFY;
    g->want_resync = g->resync_tries = 0;
    rg_streak_reset(g);
}

/* ── ALSA 캡처 스레드 ────────────────────────────────────────────── */
static void *alsa_capture_thread(void *arg)
{
    Device *d = (Device *)arg;

    struct sched_param sp = { .sched_priority = d->thread_priority };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    pin_to_cpu(2);

    /* RAVENNA: I2S 베이스라인 보정 완료 대기 (최대 10s). 보정 전에 PCM open 하면
     * alsa 측 clk reconfig 가 우리 pll_audio_core 변경을 덮어쓸 수 있음. */
    if (d->is_ravenna) {
        for (int _w = 0; _w < 100; _w++) {
            if (atomic_load_explicit(&g_clk_ready, memory_order_acquire)) break;
            if (d->quit_cap || g_quit) return NULL;
            usleep(100000);
        }
        if (!atomic_load_explicit(&g_clk_ready, memory_order_acquire))
            fprintf(stderr, "[aoip_engine] cap %s: clk_ready timeout (10s), opening anyway\n", d->name);
        else
            fprintf(stderr, "[aoip_engine] cap %s: clk_ready=1, proceeding to open\n", d->name);
    }

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

    /* 언뮤트 게이트 상태 — thread-local.
     * struct 필드에 두면 bridge_start 재호출 등으로 이전 thread 잔재값을
     * 새 thread 가 그대로 보는 race 가 발생 (디버그로 확인됨). */
    RavGate gate;
    memset(&gate, 0, sizeof(gate));
    rg_restart(&gate);

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
         * 언락/소스 없음 상태에서 drop+prepare+start 하지 않음 — 재시작 시 데이터 누적 →
         * 즉시 EPIPE 유발. prepare 재동기화는 게이트가 클럭 안정을 확인한 직후
         * (rg_try_resync, TIC 직후 타이밍) 에만 한다. */
        if (d->is_ravenna) {
            struct pollfd pfds[4];
            int npfds = snd_pcm_poll_descriptors(pcm, pfds, 4);
            if (npfds > 0) {
                int ready = poll(pfds, (nfds_t)npfds, 50);
                if (d->quit_cap || g_quit) break;
                if (ready == 0) {
                    /* 타임아웃: 소스 없음 또는 PTP 미잠금 (드라이버는 언락 중 TIC 인터럽트를
                     * 멈춘다) — 즉시 mute + 측정 리셋. silence fill 로 흡수하지 않는다
                     * (silence/실 sample 위상 어긋남으로 비트 시프트 이력). 재락 후엔 게이트가
                     * 클럭을 다시 재고 prepare 재동기화를 거친 뒤에만 언뮤트. */
                    rav_mute(d, &gate, "poll timeout (PTP unlock)", 1);
                    continue;
                }
                if (ready < 0) {
                    if (errno != EINTR) {
                        snd_pcm_recover(pcm, -EPIPE, 1);
                        snd_pcm_start(pcm);
                        gate.pos = 0;
                        rav_mute(d, &gate, "poll error", 1);
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
                gate.pos = 0;   /* recover = prepare → hw 위치 0 부터 */
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
                /* 뮤트 + 게이트 처음부터 — DSP 입력 뮤트, 재측정/재동기화 후 언뮤트 */
                rav_mute(d, &gate, "capture xrun", 0);
            }
            continue;
        }
        if (n == -ESTRPIPE) {
            while (!g_quit && snd_pcm_resume(pcm) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(pcm);
            if (d->is_ravenna) {
                snd_pcm_start(pcm);
                d->ravenna_accum = 0;
                gate.pos = 0;
                rav_mute(d, &gate, "suspend/resume", 0);
            }
            continue;
        }
        if (n == -EIO) {
            if (d->is_ravenna) {
                /* PTP 미잠금 또는 소스 없음 — 즉시 mute + 게이트 리셋 (silence fill 금지). */
                if (atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_relaxed))
                    cap_err_count = 1;
                else if (cap_err_count++ == 0)
                    fprintf(stderr, "[aoip_engine] cap %s: EIO (PTP not locked), muting\n", d->name);
                rav_mute(d, &gate, "EIO (PTP unlock)", 1);
                d->ravenna_accum = 0;
                usleep(50000);
            } else {
                /* 일반 ALSA(USB UAC2 등): 호스트 스트림 미활성.
                 * 활성→비활성 전이 시 in_ring/SRC 리셋 + DSP 측 cap_stream_active=0 으로 뮤트. */
                if (atomic_load_explicit(&d->cap_stream_active, memory_order_relaxed)) {
                    fprintf(stderr, "[aoip_engine] cap %s: host stream inactive (EIO), muting\n",
                            d->name);
                    atomic_store_explicit(&d->cap_stream_active, 0, memory_order_release);
                    rb_reset(&d->in_ring);
                    if (d->cap_src) src_reset(d->cap_src);
                    cap_err_count = 1;
                } else if (cap_err_count++ == 0) {
                    fprintf(stderr, "[aoip_engine] cap %s: EIO, host stream not active\n", d->name);
                }
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
            if (d->is_ravenna) {
                d->ravenna_accum = 0;
                gate.pos = 0;
                rav_mute(d, &gate, "pcm error", 1);
            }
            while (!d->quit_cap && !g_quit) {
                SLEEP_INTERRUPTIBLE(500, d->quit_cap);
                if (d->quit_cap || g_quit) break;
                pcm = alsa_open(d->dev, SND_PCM_STREAM_CAPTURE,
                                d->rate, d->period, d->nperiods, d->channels);
                if (pcm) {
                    cap_err_count = 0;
                    /* RAVENNA: PREPARED 상태로는 poll 에 POLLIN 이 안 옴 — 명시적 start */
                    if (d->is_ravenna) snd_pcm_start(pcm);
                    break;
                }
            }
            continue;
        }
        /* RAVENNA 언뮤트 게이트 — 뮤트 중엔 클럭만 재고 in_ring 에 쓰지 않는다.
         * 언뮤트(ravenna_ptp_locked=1) 전환 시 DSP cap_prebuf_ready=0 이므로
         * DSP-level prefill 이 자동으로 재시작된다. */
        int64_t rav_hts_ns = 0;
        if (d->is_ravenna && (int)n > 0) {
            snd_pcm_uframes_t hav = 0;
            rav_hts_ns = pcm_hts_ns(pcm, &hav);
            gate.pos += n;
            RavWin w;
            if (rav_hts_ns > 0 &&
                rav_phase_feed(&gate.ph, gate.pos + (int64_t)hav, rav_hts_ns, d->rate, &w))
                rg_on_window(d, &gate, &w);
            if (gate.want_resync) {
                /* readi 직후 = 마지막 TIC 직후 — 이 블록은 버리고 읽기 위치 재동기화 */
                rg_try_resync(d, &gate, pcm, rav_hts_ns);
                cap_err_count = 0;
                continue;
            }
            if (!atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_relaxed)) {
                cap_err_count = 0;
                continue;
            }
        }
        cap_err_count = 0;
        /* hw:aoip(I2S): 첫 readi 성공 시점에 PLL 베이스라인 보정 적용 → g_clk_ready set.
         * 이 시점은 DMA RUNNING + alsa hw_params/prepare 완료 이후라 pll_audio_core 변경이
         * alsa 측 후속 reconfig 로 덮어쓰이지 않는다. */
        if (d->is_i2s && !atomic_load_explicit(&g_clk_ready, memory_order_acquire))
            clk2_apply_initial_ppb();

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
            /* 일반 ALSA(USB UAC2 등): 비활성→활성 전이 시 ring/SRC 리셋 + DSP 측 언뮤트.
             * Ravenna 는 PTP 잠금 로직이 별도 관리하므로 건드리지 않음. */
            if (!d->is_ravenna &&
                !atomic_load_explicit(&d->cap_stream_active, memory_order_relaxed)) {
                fprintf(stderr, "[aoip_engine] cap %s: host stream active, starting\n", d->name);
                rb_reset(&d->in_ring);
                if (d->cap_src) {
                    src_reset(d->cap_src);
                    d->cap_pi.ratio  = 1.0;
                    d->cap_pi.integ  = 0.0;
                    d->cap_pi.smooth = 0.0;
                }
                atomic_store_explicit(&d->cap_stream_active, 1, memory_order_release);
                cap_err_count = 0;
            }
            i32_to_f32_block(ibuf, fbuf, (int)n * d->channels);
            /* RAVENNA/기타: 기존 RingBuf에 기록 */
            written = rb_write(&d->in_ring, fbuf, (int)n);
        }

        /* RAVENNA: htstamp 갱신 (DSP 클럭 신호 없음 — hw:aoip가 DSP 마스터) */
        if (d->is_ravenna) {
            clk2_writer_commit(&g_ravenna_seq,
                               &g_ravenna_frames, &g_ravenna_hts_ns,
                               (int64_t)written, rav_hts_ns);
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
            snd_pcm_uframes_t hav = 0;
            clk2_writer_commit(&g_aoip_seq,
                               &g_aoip_frames, &g_aoip_hts_ns,
                               (int64_t)written, pcm_hts_ns(pcm, &hav));
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

    /* RAVENNA: I2S 베이스라인 보정 완료 대기 (최대 10s) — cap 스레드와 동일 이유 */
    if (d->is_ravenna) {
        for (int _w = 0; _w < 100; _w++) {
            if (atomic_load_explicit(&g_clk_ready, memory_order_acquire)) break;
            if (d->quit_play || g_quit) return NULL;
            usleep(100000);
        }
        if (!atomic_load_explicit(&g_clk_ready, memory_order_acquire))
            fprintf(stderr, "[aoip_engine] play %s: clk_ready timeout (10s), opening anyway\n", d->name);
        else
            fprintf(stderr, "[aoip_engine] play %s: clk_ready=1, proceeding to open\n", d->name);
    }

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
            if (d->is_ravenna) {
                rb_reset(&d->out_ring);
                atomic_store_explicit(&d->play_src_reset, 1, memory_order_release);
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
                /* UAC2 가젯: 호스트 스트림 미활성.
                 * DSP는 계속 ring을 채우므로 여기서 ring 건드리지 않음 — 복귀 시점에
                 * rb_reset 으로 현재 write 위치(=가장 신선한 오디오)로 점프한다. */
                if (atomic_load_explicit(&d->play_stream_active, memory_order_relaxed)) {
                    fprintf(stderr, "[aoip_engine] play %s: host stream inactive (EIO)\n",
                            d->name);
                    atomic_store_explicit(&d->play_stream_active, 0, memory_order_release);
                    play_err_count = 1;
                } else if (play_err_count++ == 0) {
                    fprintf(stderr, "[aoip_engine] play %s: EIO, host stream not active\n",
                            d->name);
                }
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
            /* UAC2: 비활성→활성 복귀 — rb_reset 으로 read 포인터를 현재 write 위치로 점프.
             * 비활성 동안 DSP가 채워둔 stale 오디오는 버리고, 그 다음 DSP 틱부터 신선한
             * 데이터를 재생. SRC delay line 도 함께 리셋해 잔향 제거. */
            if (!d->is_ravenna && !d->is_i2s &&
                !atomic_load_explicit(&d->play_stream_active, memory_order_relaxed)) {
                fprintf(stderr, "[aoip_engine] play %s: host stream active, resuming\n",
                        d->name);
                atomic_store_explicit(&d->play_stream_active, 1, memory_order_release);
                rb_reset(&d->out_ring);
                atomic_store_explicit(&d->play_src_reset, 1, memory_order_release);
            }
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
        atomic_store_explicit(&d->cap_stream_active, 0, memory_order_relaxed);
        pthread_create(&d->cap_tid, NULL, alsa_capture_thread, d);
    }
    if (d->mode != 1) {  /* playback */
        if (d->is_i2s) {
            slot_ring_init(&d->i2s_out_ring, SLOT_COUNT, MAX_PERIOD_FRAMES, d->channels);
        } else {
            rb_init(&d->out_ring, RING_FRAMES, d->channels);
        }
        /* UAC2: 활성으로 초기화 — prebuf 대기가 진행되어야 하므로 DSP가 ring을 채울 수 있게 함.
         * 첫 writei가 EIO면 비활성으로 전이, 이후 호스트가 스트림 열면 다시 활성으로 복귀.
         * RAVENNA/I2S는 이 플래그를 보지 않으므로 영향 없음. */
        atomic_store_explicit(&d->play_stream_active, 1, memory_order_relaxed);
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
