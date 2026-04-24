/*
 * aoip_engine.c — Unified AoIP audio matrix engine (JACK-free)
 *
 * jackd + audio_in + audio_out + dsp_engine + jack_pipe_in + jack_pipe_out 통합 대체.
 *
 * 스레드 구성:
 *   [P85] dsp_thread          — timerfd 기반 마스터 오디오 루프
 *   [P80] alsa_capture_thread × N  — ALSA 장치별 캡처
 *   [P80] alsa_playback_thread × N — ALSA 장치별 재생
 *   [P75] rtp_fifo_reader_thread × N — FIFO(rtp_recv) → 입력 링버퍼
 *   [P75] rtp_fifo_writer_thread × N — 출력 링버퍼 → FIFO(rtp_send)
 *   [  ]  control_thread      — stdin 명령 파서
 *   [  ]  reporter_thread     — ~8Hz stdout 레벨 미터
 *
 * stdin 명령 (dsp_engine 완전 호환 + 신규):
 *   gain in|out <ch> <linear>
 *   mute in|out <ch> <0|1>
 *   bypass in|out <ch> <0|1>
 *   hpf in <ch> enable|freq|slope <val>
 *   eq in|out <ch> <band> enable|coeffs|freq|gain|q|type <val>
 *   limiter out <ch> enable|threshold|attack|release|makeup <val>
 *   bridge add     <name> <dev> <rate> <period> <nperiods> <ch> [ch_start]  (both)
 *   bridge add_in  <name> <dev> <rate> <period> <nperiods> <ch> [ch_start]  (capture only)
 *   bridge add_out <name> <dev> <rate> <period> <nperiods> <ch> [ch_start]  (playback only)
 *   bridge start|stop <name>
 *   route add <in_ch> <out_ch> [gain]    (1-based)
 *   route remove <in_ch> <out_ch>
 *   rtp_in add <name> <fifo> [ch] [ch_start]
 *   rtp_in remove <name>
 *   rtp_out add <name> <fifo> [ch] [ch_start]
 *   rtp_out remove <name>
 *
 * stdout 출력 (Node.js 파싱):
 *   [aoip_engine] ready client=<name> (in=N out=N sr=48000)
 *   lvl in <ch> <db>
 *   lvl out <ch> <db>
 *   lm out <ch> <pre_db> <post_db>
 *   bridge:<name>:ready
 *   bridge:<name>:stopped
 *   route:updated
 *
 * Build:
 *   gcc -O2 -o aoip_engine aoip_engine.c -lrt -lasound -lsamplerate -lpthread -lm
 */

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>
#include <sys/timerfd.h>
#include <alsa/asoundlib.h>
#include <samplerate.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── 상수 ──────────────────────────────────────────────── */
#define SAMPLE_RATE     48000
#define PERIOD_FRAMES   512
#define RING_FRAMES     16384       /* 2의 거듭제곱, ~341 ms */
#define MAX_CH          8           /* 최대 DSP 입출력 채널 수 */
#define MAX_DEVICES     4           /* 최대 ALSA 장치 수 */
#define MAX_RTP         4           /* 최대 RTP FIFO 연결 수 */
#define MAX_EQ_BANDS    4
#define CMD_RING_SIZE   128         /* 2의 거듭제곱 */
#define GAIN_MAX        2.0f
#define FILL_TARGET     (RING_FRAMES / 4)   /* PI 제어 목표 fill (~85 ms) */
#define PREBUF_FRAMES   (RING_FRAMES / 8)   /* 프리버퍼 기준 (~42 ms) */

/* PI 제어 상수 (audio_in/out에서 이식) */
#define RATIO_KP        0.00005
#define RATIO_KI        0.000000005
#define RATIO_MIN       0.99990     /* ±100 ppm */
#define RATIO_MAX       1.00010

/* ── 락프리 SPSC 링버퍼 ───────────────────────────────── */
typedef struct {
    float       *buf;
    atomic_uint  wp, rp;
    int          ring_frames;
    int          channels;
} RingBuf;

static void rb_init(RingBuf *r, int frames, int ch) {
    r->buf = calloc((size_t)(frames * ch), sizeof(float));
    atomic_init(&r->wp, 0);
    atomic_init(&r->rp, 0);
    r->ring_frames = frames;
    r->channels    = ch;
}

static int rb_avail(const RingBuf *r) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_acquire);
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_relaxed);
    return (int)(wp - rp);
}

static int rb_free(const RingBuf *r) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_relaxed);
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_acquire);
    return (int)((unsigned)r->ring_frames - (wp - rp));
}

/* n 프레임 기록. 가용 공간만큼만 기록. */
static void rb_write(RingBuf *r, const float *src, int n) {
    unsigned wp   = atomic_load_explicit(&r->wp, memory_order_relaxed);
    unsigned rp   = atomic_load_explicit(&r->rp, memory_order_acquire);
    int      free = (int)((unsigned)r->ring_frames - (wp - rp));
    if (n > free) n = free;
    for (int i = 0; i < n; i++) {
        unsigned idx = (wp + (unsigned)i) % (unsigned)r->ring_frames;
        memcpy(&r->buf[idx * r->channels], &src[i * r->channels],
               (size_t)r->channels * sizeof(float));
    }
    atomic_store_explicit(&r->wp, wp + (unsigned)n, memory_order_release);
}

/* n 프레임 읽기. 성공 시 1, 언더런 시 0. */
static int rb_read(RingBuf *r, float *dst, int n) {
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_relaxed);
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_acquire);
    if ((int)(wp - rp) < n) return 0;
    for (int i = 0; i < n; i++) {
        unsigned idx = (rp + (unsigned)i) % (unsigned)r->ring_frames;
        memcpy(&dst[i * r->channels], &r->buf[idx * r->channels],
               (size_t)r->channels * sizeof(float));
    }
    atomic_store_explicit(&r->rp, rp + (unsigned)n, memory_order_release);
    return 1;
}

static void rb_reset(RingBuf *r) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_relaxed);
    atomic_store_explicit(&r->rp, wp, memory_order_release);
}

/* ── Biquad 필터 (dsp_engine에서 이식) ───────────────── */
typedef struct { float b0,b1,b2,a1,a2, x1,x2,y1,y2; } Biquad;
typedef struct { double b0,b1,b2,a1,a2; } BqCoeffs;

static inline void bq_reset(Biquad *bq) {
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0f;
}

static inline float bq_process(Biquad *bq, float x) {
    float y = bq->b0*x + bq->b1*bq->x1 + bq->b2*bq->x2
                       - bq->a1*bq->y1  - bq->a2*bq->y2;
    bq->x2 = bq->x1; bq->x1 = x;
    bq->y2 = bq->y1; bq->y1 = y;
    return y;
}

typedef enum { T_PEAK=0, T_LOSHELF, T_HISHELF, T_LP, T_HP } EqType;

static void calc_hpf(BqCoeffs *c, float freq, float sr) {
    double w0 = 2.0*M_PI*freq/sr, cw = cos(w0), sw = sin(w0);
    double alpha = sw/(2.0*0.7071), a0 = 1.0+alpha;
    c->b0 =  (1.0+cw)/2.0/a0; c->b1 = -(1.0+cw)/a0; c->b2 = (1.0+cw)/2.0/a0;
    c->a1 = -2.0*cw/a0;       c->a2 =  (1.0-alpha)/a0;
}

static void calc_eq(BqCoeffs *c, EqType type, float freq, float gain_db,
                    float q, float sr) {
    double w0 = 2.0*M_PI*freq/sr, cw = cos(w0), sw = sin(w0);
    double A = pow(10.0, gain_db/40.0), alpha = sw/(2.0*q), sqA, a0;
    switch (type) {
    case T_PEAK:
        a0 = 1.0+alpha/A;
        c->b0 = (1.0+alpha*A)/a0; c->b1 = c->a1 = -2.0*cw/a0;
        c->b2 = (1.0-alpha*A)/a0; c->a2 = (1.0-alpha/A)/a0; break;
    case T_LOSHELF:
        sqA = sqrt(A); alpha = sw/2.0*sqrt((A+1.0/A)*(1.0/q-1.0)+2.0);
        a0 = (A+1)+(A-1)*cw+2.0*sqA*alpha;
        c->b0 =  A*((A+1)-(A-1)*cw+2.0*sqA*alpha)/a0;
        c->b1 = 2*A*((A-1)-(A+1)*cw)/a0;
        c->b2 =  A*((A+1)-(A-1)*cw-2.0*sqA*alpha)/a0;
        c->a1 = -2*((A-1)+(A+1)*cw)/a0;
        c->a2 =    ((A+1)+(A-1)*cw-2.0*sqA*alpha)/a0; break;
    case T_HISHELF:
        sqA = sqrt(A); alpha = sw/2.0*sqrt((A+1.0/A)*(1.0/q-1.0)+2.0);
        a0 = (A+1)-(A-1)*cw+2.0*sqA*alpha;
        c->b0 =  A*((A+1)+(A-1)*cw+2.0*sqA*alpha)/a0;
        c->b1 =-2*A*((A-1)+(A+1)*cw)/a0;
        c->b2 =  A*((A+1)+(A-1)*cw-2.0*sqA*alpha)/a0;
        c->a1 =  2*((A-1)-(A+1)*cw)/a0;
        c->a2 =    ((A+1)-(A-1)*cw-2.0*sqA*alpha)/a0; break;
    case T_LP:
        a0 = 1.0+alpha;
        c->b0 = (1.0-cw)/2.0/a0; c->b1 = (1.0-cw)/a0; c->b2 = (1.0-cw)/2.0/a0;
        c->a1 = -2.0*cw/a0; c->a2 = (1.0-alpha)/a0; break;
    case T_HP:
        a0 = 1.0+alpha;
        c->b0 =  (1.0+cw)/2.0/a0; c->b1 = -(1.0+cw)/a0; c->b2 = (1.0+cw)/2.0/a0;
        c->a1 = -2.0*cw/a0; c->a2 = (1.0-alpha)/a0; break;
    }
}

/* ── 리미터 (dsp_engine에서 이식) ───────────────────── */
typedef struct {
    int   enabled;
    float threshold, attack_coef, release_coef, makeup;
    float env, gr;
} Limiter;

typedef struct { float threshold, attack_coef, release_coef, makeup; } LimCoeffs;

static inline void lim_reset(Limiter *l) { l->env = 0.0f; l->gr = 1.0f; }

static inline float lim_process(Limiter *l, float x) {
    float peak = fabsf(x);
    if (peak > l->env) l->env += l->attack_coef  * (peak - l->env);
    else               l->env += l->release_coef * (peak - l->env);
    float gr = (l->env > l->threshold && l->env > 0.0f)
               ? l->threshold / l->env : 1.0f;
    l->gr = gr;
    return x * gr * l->makeup;
}

static LimCoeffs calc_limiter(float thr_db, float atk_ms, float rel_ms,
                               float mkup_db, float sr) {
    LimCoeffs c;
    c.threshold    = powf(10.0f, thr_db/20.0f);
    c.attack_coef  = 1.0f - expf(-1.0f / fmaxf(1.0f, atk_ms*sr/1000.0f));
    c.release_coef = 1.0f - expf(-1.0f / fmaxf(1.0f, rel_ms*sr/1000.0f));
    c.makeup       = powf(10.0f, mkup_db/20.0f);
    return c;
}

/* ── SPSC 명령 링버퍼 (DSP 스레드 소비자) ───────────── */
typedef enum {
    CMD_GAIN, CMD_MUTE, CMD_BYPASS,
    CMD_HPF_ENABLE, CMD_HPF_COEFFS, CMD_HPF_STAGES,
    CMD_EQ_ENABLE, CMD_EQ_COEFFS,
    CMD_LIMITER_ENABLE, CMD_LIMITER_PARAMS,
    CMD_ROUTE_SET,
} CmdType;

typedef struct {
    CmdType type;
    int     dir;    /* 0=in, 1=out */
    int     ch;     /* 0-based */
    int     band;
    union {
        float     gain;
        int       flag;
        BqCoeffs  coeffs;
        LimCoeffs lim_coeffs;
        struct { int in_ch, out_ch; float level; } route;
    };
} Cmd;

typedef struct {
    Cmd            buf[CMD_RING_SIZE];
    _Atomic size_t wr, rd;
} CmdRing;

static CmdRing g_cmd_ring;

static void cmd_push(const Cmd *c) {
    size_t wr = atomic_load_explicit(&g_cmd_ring.wr, memory_order_relaxed);
    while (wr - atomic_load_explicit(&g_cmd_ring.rd, memory_order_acquire) >= CMD_RING_SIZE)
        ;
    g_cmd_ring.buf[wr & (CMD_RING_SIZE-1)] = *c;
    atomic_store_explicit(&g_cmd_ring.wr, wr+1, memory_order_release);
}

static int cmd_pop(Cmd *c) {
    size_t rd = atomic_load_explicit(&g_cmd_ring.rd, memory_order_relaxed);
    if (rd == atomic_load_explicit(&g_cmd_ring.wr, memory_order_acquire)) return 0;
    *c = g_cmd_ring.buf[rd & (CMD_RING_SIZE-1)];
    atomic_store_explicit(&g_cmd_ring.rd, rd+1, memory_order_release);
    return 1;
}

/* ── 채널별 DSP 상태 (dsp_engine에서 이식) ──────────── */
typedef struct {
    float   gain_tgt, gain_cur;
    int     muted, bypass_dsp;
    int     hpf_enabled, hpf_stages;
    Biquad  hpf[2];
    int     eq_enabled[MAX_EQ_BANDS];
    Biquad  eq[MAX_EQ_BANDS];
    Limiter lim;
} Channel;

/* ── PI 드리프트 보정 상태 (audio_in/out에서 이식) ───── */
typedef struct {
    double ratio, integ, smooth;
    int    prebuf_done;
} PiState;

static void pi_reset(PiState *p) {
    p->ratio = 1.0; p->integ = 0.0; p->smooth = 0.0; p->prebuf_done = 0;
}

static void pi_update(PiState *p, int avail, int target) {
    double err = ((double)avail - target) / (double)target;
    p->smooth += 0.05 * (err - p->smooth);
    p->integ  += p->smooth;
    p->ratio   = 1.0 + p->smooth * RATIO_KP + p->integ * RATIO_KI;
    if (p->ratio < RATIO_MIN) {
        p->ratio = RATIO_MIN;
        p->integ = (RATIO_MIN - 1.0 - p->smooth * RATIO_KP) / RATIO_KI;
    }
    if (p->ratio > RATIO_MAX) {
        p->ratio = RATIO_MAX;
        p->integ = (RATIO_MAX - 1.0 - p->smooth * RATIO_KP) / RATIO_KI;
    }
}

/* ── ALSA 장치 구조체 ────────────────────────────────── */
#define DEV_TMP_FRAMES ((PERIOD_FRAMES + 8) * 2)

typedef struct {
    char  name[32];
    char  dev[64];
    int   rate, period, nperiods, channels;
    int   enabled;
    int   ch_start;    /* DSP 채널 시작 인덱스 */
    int   mode;        /* 0=both, 1=capture_only, 2=playback_only */

    RingBuf    in_ring;     /* ALSA 캡처 → DSP */
    RingBuf    out_ring;    /* DSP → ALSA 재생 */

    PiState    cap_pi;      /* 캡처 드리프트 보정 */
    SRC_STATE *cap_src;
    PiState    play_pi;     /* 재생 드리프트 보정 */
    SRC_STATE *play_src;

    /* DSP 스레드용 임시 버퍼 */
    float tmp_cap_in [DEV_TMP_FRAMES * MAX_CH];
    float tmp_cap_out[DEV_TMP_FRAMES * MAX_CH];
    float tmp_play_in[DEV_TMP_FRAMES * MAX_CH];
    float tmp_play_out[DEV_TMP_FRAMES * MAX_CH];

    pthread_t    cap_tid;
    pthread_t    play_tid;
    volatile int quit_cap;
    volatile int quit_play;
} Device;

/* ── RTP FIFO 연결 구조체 ────────────────────────────── */
typedef struct {
    char  name[32];
    char  fifo_path[256];
    int   channels;
    int   ch_start;
    int   enabled;
    RingBuf    ring;
    int        fd;
    pthread_t  tid;
    volatile int quit;
} RtpFifo;

/* ── 전역 상태 ───────────────────────────────────────── */
static Device  g_dev[MAX_DEVICES];
static int     g_n_dev = 0;

static RtpFifo g_rtp_in[MAX_RTP];
static int     g_n_rtp_in = 0;
static RtpFifo g_rtp_out[MAX_RTP];
static int     g_n_rtp_out = 0;

static Channel g_in_ch[MAX_CH];
static Channel g_out_ch[MAX_CH];
static int     g_n_in = 8, g_n_out = 8;
static float   g_sr   = (float)SAMPLE_RATE;

/* 라우팅 매트릭스: g_route[out_ch][in_ch] = 믹스 게인 */
static float   g_route[MAX_CH][MAX_CH];

/* 레벨 미터 (DSP 스레드 기록, 리포터 스레드 읽기) */
static volatile float g_in_level[MAX_CH];
static volatile float g_out_level[MAX_CH];
static volatile float g_lim_pre[MAX_CH];
static volatile float g_lim_post[MAX_CH];

static volatile int g_quit = 0;
static volatile int g_reporter_running = 0;

/* 계수 재계산용 채널 파라미터 */
typedef struct {
    float hpf_freq; int hpf_slope;
    struct { float freq, gain_db, q; EqType type; } eq[MAX_EQ_BANDS];
    struct { float threshold_db, attack_ms, release_ms, makeup_db; } lim;
} ChState;

static ChState g_in_state[MAX_CH];
static ChState g_out_state[MAX_CH];

/* ── 시그널 핸들러 ───────────────────────────────────── */
static void sig_handler(int s) { (void)s; g_quit = 1; fclose(stdin); }

/* ── ALSA 헬퍼 ───────────────────────────────────────── */
static snd_pcm_t *alsa_open(const char *dev, int stream, int rate,
                              int period, int nperiods, int ch) {
    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, dev, stream, 0) < 0) return NULL;
    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, (unsigned)ch);
    unsigned r = (unsigned)rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &r, 0);
    snd_pcm_uframes_t p = (snd_pcm_uframes_t)period;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &p, 0);
    snd_pcm_uframes_t buf = p * (snd_pcm_uframes_t)nperiods;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &buf);
    if (snd_pcm_hw_params(pcm, hw) < 0) { snd_pcm_close(pcm); return NULL; }
    snd_pcm_prepare(pcm);
    return pcm;
}

/* ── ALSA 캡처 스레드 ────────────────────────────────── */
static void *alsa_capture_thread(void *arg) {
    Device *d = (Device *)arg;

    struct sched_param sp = { .sched_priority = 80 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    snd_pcm_t *pcm = alsa_open(d->dev, SND_PCM_STREAM_CAPTURE,
                                d->rate, d->period, d->nperiods, d->channels);
    if (!pcm) {
        fprintf(stderr, "[aoip_engine] cap: cannot open %s\n", d->dev);
        return NULL;
    }

    int32_t *ibuf = malloc((size_t)(d->period * d->channels) * sizeof(int32_t));
    float   *fbuf = malloc((size_t)(d->period * d->channels) * sizeof(float));

    while (!d->quit_cap && !g_quit) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, ibuf, (snd_pcm_uframes_t)d->period);
        if (n == -EPIPE) { snd_pcm_prepare(pcm); continue; }
        if (n == -ESTRPIPE) {
            while (!g_quit && snd_pcm_resume(pcm) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(pcm); continue;
        }
        if (n < 0) {
            fprintf(stderr, "[aoip_engine] cap %s: %s\n", d->name, snd_strerror((int)n));
            snd_pcm_close(pcm); pcm = NULL;
            rb_reset(&d->in_ring);
            while (!d->quit_cap && !g_quit) {
                usleep(500000);
                pcm = alsa_open(d->dev, SND_PCM_STREAM_CAPTURE,
                                d->rate, d->period, d->nperiods, d->channels);
                if (pcm) { fprintf(stderr, "[aoip_engine] cap %s reopened\n", d->name); break; }
            }
            continue;
        }
        /* S32LE → float */
        for (int i = 0; i < (int)n * d->channels; i++)
            fbuf[i] = (float)ibuf[i] * (1.0f / 2147483648.0f);
        rb_write(&d->in_ring, fbuf, (int)n);
    }

    free(ibuf); free(fbuf);
    if (pcm) snd_pcm_close(pcm);
    return NULL;
}

/* ── ALSA 재생 스레드 ────────────────────────────────── */
static void *alsa_playback_thread(void *arg) {
    Device *d = (Device *)arg;

    struct sched_param sp = { .sched_priority = 80 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    snd_pcm_t *pcm = alsa_open(d->dev, SND_PCM_STREAM_PLAYBACK,
                                d->rate, d->period, d->nperiods, d->channels);
    if (!pcm) {
        fprintf(stderr, "[aoip_engine] play: cannot open %s\n", d->dev);
        return NULL;
    }

    float   *fbuf = malloc((size_t)(d->period * d->channels) * sizeof(float));
    int32_t *ibuf = malloc((size_t)(d->period * d->channels) * sizeof(int32_t));

    /* 프리버퍼 대기 */
    while (!d->quit_play && !g_quit && rb_avail(&d->out_ring) < PREBUF_FRAMES)
        usleep(1000);

    while (!d->quit_play && !g_quit) {
        if (!rb_read(&d->out_ring, fbuf, d->period)) {
            memset(ibuf, 0, (size_t)(d->period * d->channels) * sizeof(int32_t));
        } else {
            for (int i = 0; i < d->period * d->channels; i++) {
                float v = fbuf[i];
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                ibuf[i] = (int32_t)(v * 2147483647.0f);
            }
        }

        snd_pcm_sframes_t n = snd_pcm_writei(pcm, ibuf, (snd_pcm_uframes_t)d->period);
        if (n == -EPIPE) {
            rb_reset(&d->out_ring); snd_pcm_prepare(pcm);
            while (!d->quit_play && !g_quit && rb_avail(&d->out_ring) < PREBUF_FRAMES)
                usleep(1000);
        } else if (n == -ESTRPIPE) {
            rb_reset(&d->out_ring);
            while (!g_quit && snd_pcm_resume(pcm) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(pcm);
            while (!d->quit_play && !g_quit && rb_avail(&d->out_ring) < PREBUF_FRAMES)
                usleep(1000);
        } else if (n < 0) {
            fprintf(stderr, "[aoip_engine] play %s: %s\n", d->name, snd_strerror((int)n));
            snd_pcm_close(pcm); pcm = NULL;
            rb_reset(&d->out_ring);
            while (!d->quit_play && !g_quit) {
                usleep(500000);
                pcm = alsa_open(d->dev, SND_PCM_STREAM_PLAYBACK,
                                d->rate, d->period, d->nperiods, d->channels);
                if (pcm) {
                    while (!d->quit_play && !g_quit && rb_avail(&d->out_ring) < PREBUF_FRAMES)
                        usleep(1000);
                    break;
                }
            }
        }
    }

    free(fbuf); free(ibuf);
    if (pcm) snd_pcm_close(pcm);
    return NULL;
}

/* ── RTP FIFO 리더 스레드 (rtp_recv → DSP 입력) ─────── */
static void *rtp_fifo_reader_thread(void *arg) {
    RtpFifo *r = (RtpFifo *)arg;

    struct sched_param sp = { .sched_priority = 75 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* Non-blocking으로 반복 시도 (writer가 연결될 때까지) */
    while (!r->quit && !g_quit) {
        r->fd = open(r->fifo_path, O_RDONLY | O_NONBLOCK);
        if (r->fd >= 0) break;
        usleep(100000);
    }
    if (r->fd < 0) return NULL;

    /* 연결 후 블로킹 모드로 전환 */
    int flags = fcntl(r->fd, F_GETFL);
    fcntl(r->fd, F_SETFL, flags & ~O_NONBLOCK);

    size_t frame_bytes = (size_t)r->channels * sizeof(float);
    size_t want = (size_t)PERIOD_FRAMES * frame_bytes;
    float *buf  = malloc(want);

    while (!r->quit && !g_quit) {
        /* PERIOD_FRAMES 만큼 정확히 읽기 */
        size_t got = 0;
        while (got < want && !r->quit && !g_quit) {
            ssize_t n = read(r->fd, (char *)buf + got, want - got);
            if (n <= 0) {
                if (errno == EINTR) continue;
                goto reader_eof;
            }
            got += (size_t)n;
        }
        if (got == want) rb_write(&r->ring, buf, PERIOD_FRAMES);
    }

reader_eof:
    free(buf);
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    return NULL;
}

/* ── RTP FIFO 라이터 스레드 (DSP 출력 → rtp_send) ───── */
static void *rtp_fifo_writer_thread(void *arg) {
    RtpFifo *r = (RtpFifo *)arg;

    struct sched_param sp = { .sched_priority = 75 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* Non-blocking으로 반복 시도 (reader가 연결될 때까지) */
    while (!r->quit && !g_quit) {
        r->fd = open(r->fifo_path, O_WRONLY | O_NONBLOCK);
        if (r->fd >= 0) break;
        usleep(100000);
    }
    if (r->fd < 0) return NULL;

    /* 블로킹 모드로 전환 (쓰기 차단으로 자연스러운 back-pressure) */
    int flags = fcntl(r->fd, F_GETFL);
    fcntl(r->fd, F_SETFL, flags & ~O_NONBLOCK);

    size_t frame_bytes = (size_t)r->channels * sizeof(float);
    size_t want = (size_t)PERIOD_FRAMES * frame_bytes;
    float *buf  = malloc(want);

    while (!r->quit && !g_quit) {
        if (!rb_read(&r->ring, buf, PERIOD_FRAMES)) {
            usleep(1000);
            continue;
        }
        ssize_t n = write(r->fd, buf, want);
        if (n < 0 && errno != EINTR) break;
    }

    free(buf);
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    return NULL;
}

/* ── 명령 적용 (DSP 스레드에서 호출) ────────────────── */
static void apply_cmd(const Cmd *cmd) {
    Channel *ch = (cmd->dir == 0) ? &g_in_ch[cmd->ch] : &g_out_ch[cmd->ch];
    switch (cmd->type) {
    case CMD_GAIN:   ch->gain_tgt = cmd->gain; break;
    case CMD_MUTE:   ch->muted = cmd->flag; if (ch->muted) ch->gain_cur = ch->gain_tgt; break;
    case CMD_BYPASS: ch->bypass_dsp = cmd->flag; break;
    case CMD_HPF_ENABLE: ch->hpf_enabled = cmd->flag; break;
    case CMD_HPF_STAGES: ch->hpf_stages  = cmd->flag; break;
    case CMD_HPF_COEFFS: {
        Biquad *bq = &ch->hpf[cmd->band];
        bq->b0=(float)cmd->coeffs.b0; bq->b1=(float)cmd->coeffs.b1; bq->b2=(float)cmd->coeffs.b2;
        bq->a1=(float)cmd->coeffs.a1; bq->a2=(float)cmd->coeffs.a2; break;
    }
    case CMD_EQ_ENABLE: ch->eq_enabled[cmd->band] = cmd->flag; break;
    case CMD_EQ_COEFFS: {
        Biquad *bq = &ch->eq[cmd->band];
        bq->b0=(float)cmd->coeffs.b0; bq->b1=(float)cmd->coeffs.b1; bq->b2=(float)cmd->coeffs.b2;
        bq->a1=(float)cmd->coeffs.a1; bq->a2=(float)cmd->coeffs.a2; break;
    }
    case CMD_LIMITER_ENABLE:
        ch->lim.enabled = cmd->flag;
        if (!cmd->flag) { lim_reset(&ch->lim); } break;
    case CMD_LIMITER_PARAMS:
        ch->lim.threshold    = cmd->lim_coeffs.threshold;
        ch->lim.attack_coef  = cmd->lim_coeffs.attack_coef;
        ch->lim.release_coef = cmd->lim_coeffs.release_coef;
        ch->lim.makeup       = cmd->lim_coeffs.makeup; break;
    case CMD_ROUTE_SET:
        if (cmd->route.out_ch < MAX_CH && cmd->route.in_ch < MAX_CH)
            g_route[cmd->route.out_ch][cmd->route.in_ch] = cmd->route.level;
        break;
    }
}

/* ── DSP 스레드 마스터 루프 ──────────────────────────── */
/* 채널별 오디오 버퍼 (스택 대신 정적으로 할당) */
static float g_in_buf[MAX_CH][PERIOD_FRAMES];
static float g_out_buf[MAX_CH][PERIOD_FRAMES];

static void *dsp_thread(void *arg) {
    (void)arg;

    struct sched_param sp = { .sched_priority = 85 };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* timerfd: 512프레임 / 48000 Hz = 10,666,666 ns 주기 */
    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec ts;
    ts.it_value.tv_sec     = 0;
    ts.it_value.tv_nsec    = 1000000LL;  /* 첫 발화: 1 ms 후 */
    ts.it_interval.tv_sec  = 0;
    ts.it_interval.tv_nsec = (long)(1000000000LL * PERIOD_FRAMES / SAMPLE_RATE);
    timerfd_settime(tfd, 0, &ts, NULL);

    while (!g_quit) {
        uint64_t exp;
        if (read(tfd, &exp, sizeof(exp)) < 0) break;
        if (exp > 2)
            fprintf(stderr, "[aoip_engine] DSP overrun: skipped %llu periods\n",
                    (unsigned long long)(exp - 1));

        /* ── 명령 링 드레인 ── */
        Cmd cmd;
        while (cmd_pop(&cmd)) apply_cmd(&cmd);

        /* ── 입력 읽기: ALSA 장치 (SRC + PI 드리프트 보정) ── */
        for (int di = 0; di < g_n_dev; di++) {
            Device *d = &g_dev[di];
            if (!d->enabled || d->mode == 2) continue;  /* playback-only: skip */

            int avail = rb_avail(&d->in_ring);

            if (!d->cap_pi.prebuf_done) {
                if (avail < PREBUF_FRAMES) {
                    for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                        memset(g_in_buf[d->ch_start+c], 0, PERIOD_FRAMES*sizeof(float));
                    continue;
                }
                d->cap_pi.prebuf_done = 1;
            }

            pi_update(&d->cap_pi, avail, FILL_TARGET);

            int input_need = (int)ceil((double)PERIOD_FRAMES / d->cap_pi.ratio) + 2;
            if (input_need > avail) {
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    memset(g_in_buf[d->ch_start+c], 0, PERIOD_FRAMES*sizeof(float));
                continue;
            }
            if (input_need > DEV_TMP_FRAMES) input_need = DEV_TMP_FRAMES;

            /* 링에서 input_need 프레임 복사 (rp 이동 전) */
            unsigned rp = atomic_load_explicit(&d->in_ring.rp, memory_order_relaxed);
            for (int i = 0; i < input_need; i++) {
                unsigned idx = (rp + (unsigned)i) % (unsigned)d->in_ring.ring_frames;
                memcpy(&d->tmp_cap_in[i * d->channels],
                       &d->in_ring.buf[idx * d->in_ring.channels],
                       (size_t)d->channels * sizeof(float));
            }

            SRC_DATA sd = {
                .data_in       = d->tmp_cap_in,
                .data_out      = d->tmp_cap_out,
                .input_frames  = input_need,
                .output_frames = PERIOD_FRAMES,
                .src_ratio     = d->cap_pi.ratio,
                .end_of_input  = 0,
            };
            src_process(d->cap_src, &sd);

            atomic_store_explicit(&d->in_ring.rp,
                rp + (unsigned)sd.input_frames_used, memory_order_release);

            /* de-interleave → 채널별 버퍼 */
            long gen = sd.output_frames_gen;
            for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++) {
                float *dst = g_in_buf[d->ch_start+c];
                for (long f = 0; f < gen; f++) dst[f] = d->tmp_cap_out[f*d->channels+c];
                for (long f = gen; f < PERIOD_FRAMES; f++) dst[f] = 0.0f;
            }
        }

        /* ── 입력 읽기: RTP FIFO ── */
        for (int ri = 0; ri < g_n_rtp_in; ri++) {
            RtpFifo *r = &g_rtp_in[ri];
            if (!r->enabled) continue;
            float tmp[PERIOD_FRAMES * MAX_CH];
            if (rb_read(&r->ring, tmp, PERIOD_FRAMES)) {
                for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++) {
                    float *dst = g_in_buf[r->ch_start+c];
                    for (int f = 0; f < PERIOD_FRAMES; f++) dst[f] = tmp[f*r->channels+c];
                }
            } else {
                for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                    memset(g_in_buf[r->ch_start+c], 0, PERIOD_FRAMES*sizeof(float));
            }
        }

        /* ── 입력 DSP: HPF → EQ → gain ramp → level meter ── */
        for (int ch = 0; ch < g_n_in; ch++) {
            Channel *ic  = &g_in_ch[ch];
            float   *buf = g_in_buf[ch];

            if (ic->muted) {
                memset(buf, 0, PERIOD_FRAMES*sizeof(float));
                ic->gain_cur = ic->gain_tgt;
                continue;
            }

            float cur  = ic->gain_cur;
            float step = (ic->gain_tgt - cur) / (float)PERIOD_FRAMES;

            for (int i = 0; i < PERIOD_FRAMES; i++) {
                float s = buf[i];
                if (!ic->bypass_dsp) {
                    if (ic->hpf_enabled) {
                        s = bq_process(&ic->hpf[0], s);
                        if (ic->hpf_stages > 1) s = bq_process(&ic->hpf[1], s);
                    }
                    for (int b = 0; b < MAX_EQ_BANDS; b++)
                        if (ic->eq_enabled[b]) s = bq_process(&ic->eq[b], s);
                }
                float peak = fabsf(s);
                if (peak > g_in_level[ch]) g_in_level[ch] = peak;
                cur += step;
                buf[i] = s * cur;
            }
            ic->gain_cur = ic->gain_tgt;
        }

        /* ── 라우팅 매트릭스 믹싱 ── */
        for (int out = 0; out < g_n_out; out++) {
            memset(g_out_buf[out], 0, PERIOD_FRAMES*sizeof(float));
            for (int in = 0; in < g_n_in; in++) {
                float gain = g_route[out][in];
                if (gain == 0.0f) continue;
                for (int f = 0; f < PERIOD_FRAMES; f++)
                    g_out_buf[out][f] += g_in_buf[in][f] * gain;
            }
        }

        /* ── 출력 DSP: EQ → limiter → gain ramp → level meter ── */
        for (int ch = 0; ch < g_n_out; ch++) {
            Channel *oc  = &g_out_ch[ch];
            float   *buf = g_out_buf[ch];

            if (oc->muted) {
                memset(buf, 0, PERIOD_FRAMES*sizeof(float));
                oc->gain_cur = oc->gain_tgt;
                continue;
            }

            float cur  = oc->gain_cur;
            float step = (oc->gain_tgt - cur) / (float)PERIOD_FRAMES;

            for (int i = 0; i < PERIOD_FRAMES; i++) {
                float s = buf[i];
                if (!oc->bypass_dsp) {
                    for (int b = 0; b < MAX_EQ_BANDS; b++)
                        if (oc->eq_enabled[b]) s = bq_process(&oc->eq[b], s);

                    float pre_peak = fabsf(s);
                    if (pre_peak > g_lim_pre[ch]) g_lim_pre[ch] = pre_peak;

                    if (oc->lim.enabled) s = lim_process(&oc->lim, s);

                    float post_peak = fabsf(s);
                    if (post_peak > g_lim_post[ch]) g_lim_post[ch] = post_peak;
                    if (post_peak > g_out_level[ch]) g_out_level[ch] = post_peak;
                } else {
                    float peak = fabsf(s);
                    if (peak > g_out_level[ch]) g_out_level[ch] = peak;
                }
                cur += step;
                buf[i] = s * cur;
            }
            oc->gain_cur = oc->gain_tgt;
        }

        /* ── 출력 기록: ALSA 장치 (SRC 드리프트 보정) ── */
        for (int di = 0; di < g_n_dev; di++) {
            Device *d = &g_dev[di];
            if (!d->enabled || d->mode == 1) continue;  /* capture-only: skip */

            /* 채널 interleave */
            for (int f = 0; f < PERIOD_FRAMES; f++) {
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    d->tmp_play_in[f*d->channels+c] = g_out_buf[d->ch_start+c][f];
            }

            /* 출력 SRC: DSP 주기 → ALSA 주기 드리프트 보정 */
            int avail_out = rb_avail(&d->out_ring);
            pi_update(&d->play_pi, avail_out, FILL_TARGET);

            long out_max = (long)ceil((double)PERIOD_FRAMES * d->play_pi.ratio) + 4;
            if (out_max > DEV_TMP_FRAMES) out_max = DEV_TMP_FRAMES;
            if (rb_free(&d->out_ring) < (int)out_max) continue;  /* ring full, drop */

            SRC_DATA sd = {
                .data_in       = d->tmp_play_in,
                .data_out      = d->tmp_play_out,
                .input_frames  = PERIOD_FRAMES,
                .output_frames = out_max,
                .src_ratio     = d->play_pi.ratio,
                .end_of_input  = 0,
            };
            src_process(d->play_src, &sd);
            rb_write(&d->out_ring, d->tmp_play_out, (int)sd.output_frames_gen);
        }

        /* ── 출력 기록: RTP FIFO ── */
        for (int ri = 0; ri < g_n_rtp_out; ri++) {
            RtpFifo *r = &g_rtp_out[ri];
            if (!r->enabled) continue;
            float tmp[PERIOD_FRAMES * MAX_CH];
            for (int f = 0; f < PERIOD_FRAMES; f++) {
                for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                    tmp[f*r->channels+c] = g_out_buf[r->ch_start+c][f];
            }
            rb_write(&r->ring, tmp, PERIOD_FRAMES);
        }
    }

    close(tfd);
    return NULL;
}

/* ── 리포터 스레드 (~8 Hz) ────────────────────────────── */
static void *reporter_thread(void *arg) {
    (void)arg;
    while (g_reporter_running) {
        usleep(125000);
        for (int i = 0; i < g_n_in; i++) {
            float pk = g_in_level[i]; g_in_level[i] = 0.0f;
            printf("lvl in %d %.1f\n", i+1, pk > 1e-7f ? 20.0f*log10f(pk) : -120.0f);
        }
        for (int i = 0; i < g_n_out; i++) {
            float pk = g_out_level[i]; g_out_level[i] = 0.0f;
            printf("lvl out %d %.1f\n", i+1, pk > 1e-7f ? 20.0f*log10f(pk) : -120.0f);
        }
        for (int i = 0; i < g_n_out; i++) {
            if (!g_out_ch[i].lim.enabled) continue;
            float pre_p  = g_lim_pre[i];  g_lim_pre[i]  = 0.0f;
            float post_p = g_lim_post[i]; g_lim_post[i] = 0.0f;
            printf("lm out %d %.1f %.1f\n", i+1,
                   pre_p  > 1e-7f ? 20.0f*log10f(pre_p)  : -120.0f,
                   post_p > 1e-7f ? 20.0f*log10f(post_p) : -120.0f);
        }
        fflush(stdout);
    }
    return NULL;
}

/* ── 장치 시작/중지 헬퍼 ─────────────────────────────── */
static void device_start(Device *d) {
    if (!d->enabled) return;
    int err;
    d->quit_cap = d->quit_play = 0;
    if (d->mode != 2) {  /* capture (both or capture_only) */
        d->cap_src = src_new(SRC_SINC_FASTEST, d->channels, &err);
        pi_reset(&d->cap_pi);
        rb_init(&d->in_ring,  RING_FRAMES, d->channels);
        pthread_create(&d->cap_tid,  NULL, alsa_capture_thread,  d);
    }
    if (d->mode != 1) {  /* playback (both or playback_only) */
        d->play_src = src_new(SRC_SINC_FASTEST, d->channels, &err);
        pi_reset(&d->play_pi);
        rb_init(&d->out_ring, RING_FRAMES, d->channels);
        pthread_create(&d->play_tid, NULL, alsa_playback_thread, d);
    }
    printf("bridge:%s:ready\n", d->name);
    fflush(stdout);
}

static void device_stop(Device *d) {
    if (d->mode != 2) {
        d->quit_cap = 1;
        pthread_join(d->cap_tid, NULL);
        if (d->cap_src) { src_delete(d->cap_src); d->cap_src = NULL; }
        if (d->in_ring.buf) { free(d->in_ring.buf); d->in_ring.buf = NULL; }
    }
    if (d->mode != 1) {
        d->quit_play = 1;
        pthread_join(d->play_tid, NULL);
        if (d->play_src) { src_delete(d->play_src); d->play_src = NULL; }
        if (d->out_ring.buf) { free(d->out_ring.buf); d->out_ring.buf = NULL; }
    }
    printf("bridge:%s:stopped\n", d->name);
    fflush(stdout);
}

/* ── EQ 타입 파서 ────────────────────────────────────── */
static EqType parse_eq_type(const char *s) {
    if (!strcmp(s, "loshelf")) return T_LOSHELF;
    if (!strcmp(s, "hishelf")) return T_HISHELF;
    if (!strcmp(s, "lp"))      return T_LP;
    if (!strcmp(s, "hp"))      return T_HP;
    return T_PEAK;
}

/* ── stdin 명령 루프 (control thread) ───────────────── */
static void cmd_loop(void) {
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        char *tok[16]; int n = 0;
        char *p = strtok(line, " \t\r\n");
        while (p && n < 16) { tok[n++] = p; p = strtok(NULL, " \t\r\n"); }
        if (n < 1) continue;

        const char *verb = tok[0];

        /* ── bridge 명령 ── */
        if (!strcmp(verb, "bridge") && n >= 3) {
            const char *sub  = tok[1];
            const char *name = tok[2];

            if ((!strcmp(sub, "add") || !strcmp(sub, "add_in") || !strcmp(sub, "add_out")) && n >= 8) {
                /* bridge add[_in|_out] <name> <dev> <rate> <period> <nperiods> <ch> [ch_start] */
                if (g_n_dev >= MAX_DEVICES) continue;
                Device *d = &g_dev[g_n_dev];
                snprintf(d->name, sizeof(d->name), "%s", name);
                snprintf(d->dev,  sizeof(d->dev),  "%s", tok[3]);
                d->rate     = atoi(tok[4]);
                d->period   = atoi(tok[5]);
                d->nperiods = atoi(tok[6]);
                d->channels = atoi(tok[7]);
                d->ch_start = n >= 9 ? atoi(tok[8]) : g_n_dev * 2;
                d->mode     = !strcmp(sub, "add_in")  ? 1 :
                              !strcmp(sub, "add_out") ? 2 : 0;
                d->enabled  = 1;
                g_n_dev++;
                device_start(d);
            } else if (!strcmp(sub, "start")) {
                for (int i = 0; i < g_n_dev; i++)
                    if (!strcmp(g_dev[i].name, name)) { device_start(&g_dev[i]); break; }
            } else if (!strcmp(sub, "stop")) {
                for (int i = 0; i < g_n_dev; i++)
                    if (!strcmp(g_dev[i].name, name)) { device_stop(&g_dev[i]); break; }
            }
            continue;
        }

        /* ── route 명령 ── */
        if (!strcmp(verb, "route") && n >= 4) {
            const char *sub = tok[1];
            int in_ch  = atoi(tok[2]) - 1;  /* 1-based → 0-based */
            int out_ch = atoi(tok[3]) - 1;
            if (in_ch < 0 || in_ch >= MAX_CH || out_ch < 0 || out_ch >= MAX_CH) continue;
            Cmd cmd = { .type = CMD_ROUTE_SET };
            cmd.route.in_ch  = in_ch;
            cmd.route.out_ch = out_ch;
            cmd.route.level  = (!strcmp(sub, "remove")) ? 0.0f :
                               (n >= 5 ? (float)atof(tok[4]) : 1.0f);
            cmd_push(&cmd);
            printf("route:updated\n");
            fflush(stdout);
            continue;
        }

        /* ── rtp_in 명령 ── */
        if (!strcmp(verb, "rtp_in") && n >= 4) {
            const char *sub  = tok[1];
            const char *name = tok[2];
            if (!strcmp(sub, "add") && g_n_rtp_in < MAX_RTP) {
                RtpFifo *r = &g_rtp_in[g_n_rtp_in];
                snprintf(r->name,      sizeof(r->name),      "%s", name);
                snprintf(r->fifo_path, sizeof(r->fifo_path), "%s", tok[3]);
                r->channels = n >= 5 ? atoi(tok[4]) : 2;
                r->ch_start = n >= 6 ? atoi(tok[5]) : g_n_rtp_in * 2;
                r->enabled = 1; r->fd = -1; r->quit = 0;
                rb_init(&r->ring, RING_FRAMES, r->channels);
                pthread_create(&r->tid, NULL, rtp_fifo_reader_thread, r);
                g_n_rtp_in++;
            } else if (!strcmp(sub, "remove")) {
                for (int i = 0; i < g_n_rtp_in; i++) {
                    if (!strcmp(g_rtp_in[i].name, name)) {
                        g_rtp_in[i].quit = 1;
                        pthread_join(g_rtp_in[i].tid, NULL);
                        free(g_rtp_in[i].ring.buf);
                        g_rtp_in[i].enabled = 0;
                        break;
                    }
                }
            }
            continue;
        }

        /* ── rtp_out 명령 ── */
        if (!strcmp(verb, "rtp_out") && n >= 4) {
            const char *sub  = tok[1];
            const char *name = tok[2];
            if (!strcmp(sub, "add") && g_n_rtp_out < MAX_RTP) {
                RtpFifo *r = &g_rtp_out[g_n_rtp_out];
                snprintf(r->name,      sizeof(r->name),      "%s", name);
                snprintf(r->fifo_path, sizeof(r->fifo_path), "%s", tok[3]);
                r->channels = n >= 5 ? atoi(tok[4]) : 2;
                r->ch_start = n >= 6 ? atoi(tok[5]) : g_n_rtp_out * 2;
                r->enabled = 1; r->fd = -1; r->quit = 0;
                rb_init(&r->ring, RING_FRAMES, r->channels);
                pthread_create(&r->tid, NULL, rtp_fifo_writer_thread, r);
                g_n_rtp_out++;
            } else if (!strcmp(sub, "remove")) {
                for (int i = 0; i < g_n_rtp_out; i++) {
                    if (!strcmp(g_rtp_out[i].name, name)) {
                        g_rtp_out[i].quit = 1;
                        pthread_join(g_rtp_out[i].tid, NULL);
                        free(g_rtp_out[i].ring.buf);
                        g_rtp_out[i].enabled = 0;
                        break;
                    }
                }
            }
            continue;
        }

        /* ── DSP 명령 (dsp_engine 완전 호환) ── */
        if (n < 3) continue;
        int dir = strcmp(tok[1], "in") ? 1 : 0;
        int ch  = atoi(tok[2]) - 1;  /* 1-based → 0-based */
        if (ch < 0 || (dir==0 && ch>=g_n_in) || (dir==1 && ch>=g_n_out)) continue;

        ChState *cs  = (dir == 0) ? &g_in_state[ch] : &g_out_state[ch];
        Cmd      cmd = { .dir = dir, .ch = ch };

        if (!strcmp(verb, "gain") && n >= 4) {
            cmd.type = CMD_GAIN;
            cmd.gain = fmaxf(0.0f, fminf(GAIN_MAX, (float)atof(tok[3])));
            cmd_push(&cmd);

        } else if (!strcmp(verb, "mute") && n >= 4) {
            cmd.type = CMD_MUTE; cmd.flag = atoi(tok[3]);
            cmd_push(&cmd);

        } else if (!strcmp(verb, "bypass") && n >= 4) {
            cmd.type = CMD_BYPASS; cmd.flag = atoi(tok[3]);
            cmd_push(&cmd);

        } else if (!strcmp(verb, "hpf") && n >= 5) {
            const char *param = tok[3];
            if (!strcmp(param, "enable")) {
                cmd.type = CMD_HPF_ENABLE; cmd.flag = atoi(tok[4]);
                cmd_push(&cmd);
            } else if (!strcmp(param, "freq")) {
                cs->hpf_freq = (float)atof(tok[4]);
                BqCoeffs c; calc_hpf(&c, cs->hpf_freq, g_sr);
                cmd.type = CMD_HPF_COEFFS; cmd.band = 0; cmd.coeffs = c;
                cmd_push(&cmd);
                if (cs->hpf_slope >= 24) { cmd.band = 1; cmd_push(&cmd); }
            } else if (!strcmp(param, "slope")) {
                cs->hpf_slope = atoi(tok[4]);
                int stages = (cs->hpf_slope >= 24) ? 2 : 1;
                cmd.type = CMD_HPF_STAGES; cmd.flag = stages;
                cmd_push(&cmd);
                if (cs->hpf_freq > 0.0f) {
                    BqCoeffs c; calc_hpf(&c, cs->hpf_freq, g_sr);
                    cmd.type = CMD_HPF_COEFFS; cmd.band = 0; cmd.coeffs = c;
                    cmd_push(&cmd);
                    if (stages > 1) { cmd.band = 1; cmd_push(&cmd); }
                }
            }

        } else if (!strcmp(verb, "eq") && n >= 6) {
            int band = atoi(tok[3]);
            if (band < 0 || band >= MAX_EQ_BANDS) continue;
            cmd.band = band;
            const char *param = tok[4];
            if (!strcmp(param, "enable")) {
                cmd.type = CMD_EQ_ENABLE; cmd.flag = atoi(tok[5]);
                cmd_push(&cmd);
            } else if (!strcmp(param, "coeffs") && n >= 10) {
                BqCoeffs c = {atof(tok[5]),atof(tok[6]),atof(tok[7]),atof(tok[8]),atof(tok[9])};
                cmd.type = CMD_EQ_COEFFS; cmd.coeffs = c;
                cmd_push(&cmd);
            } else {
                if      (!strcmp(param, "freq")) cs->eq[band].freq    = (float)atof(tok[5]);
                else if (!strcmp(param, "gain")) cs->eq[band].gain_db = (float)atof(tok[5]);
                else if (!strcmp(param, "q"))    cs->eq[band].q       = fmaxf(0.1f,(float)atof(tok[5]));
                else if (!strcmp(param, "type")) cs->eq[band].type    = parse_eq_type(tok[5]);
                else continue;
                BqCoeffs c;
                calc_eq(&c, cs->eq[band].type, cs->eq[band].freq,
                        cs->eq[band].gain_db, cs->eq[band].q, g_sr);
                cmd.type = CMD_EQ_COEFFS; cmd.coeffs = c;
                cmd_push(&cmd);
            }

        } else if (!strcmp(verb, "limiter") && n >= 5) {
            if (dir != 1) continue;
            const char *param = tok[3];
            if (!strcmp(param, "enable")) {
                cmd.type = CMD_LIMITER_ENABLE; cmd.flag = atoi(tok[4]);
                cmd_push(&cmd);
            } else {
                if      (!strcmp(param, "threshold")) cs->lim.threshold_db = (float)atof(tok[4]);
                else if (!strcmp(param, "attack"))    cs->lim.attack_ms    = fmaxf(0.1f,(float)atof(tok[4]));
                else if (!strcmp(param, "release"))   cs->lim.release_ms   = fmaxf(1.0f,(float)atof(tok[4]));
                else if (!strcmp(param, "makeup"))    cs->lim.makeup_db    = (float)atof(tok[4]);
                else continue;
                LimCoeffs lc = calc_limiter(cs->lim.threshold_db, cs->lim.attack_ms,
                                            cs->lim.release_ms,   cs->lim.makeup_db, g_sr);
                cmd.type = CMD_LIMITER_PARAMS; cmd.lim_coeffs = lc;
                cmd_push(&cmd);
            }
        }
    }
}

/* ── main ────────────────────────────────────────────── */
int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: aoip_engine <n_in> <n_out> [--name <name>]\n");
        return 1;
    }
    g_n_in  = atoi(argv[1]);
    g_n_out = atoi(argv[2]);
    if (g_n_in  < 0 || g_n_in  > MAX_CH ||
        g_n_out < 0 || g_n_out > MAX_CH) {
        fprintf(stderr, "[aoip_engine] channel count out of range (max %d)\n", MAX_CH);
        return 1;
    }

    const char *name = "aoip_engine";
    for (int i = 3; i < argc; i++)
        if (!strcmp(argv[i], "--name") && i+1 < argc) name = argv[++i];

    /* 기본 채널 상태 초기화 (dsp_engine과 동일) */
    for (int i = 0; i < g_n_in; i++) {
        g_in_state[i].hpf_freq  = 80.0f;
        g_in_state[i].hpf_slope = 12;
        g_in_ch[i].hpf_stages   = 1;
        for (int b = 0; b < MAX_EQ_BANDS; b++) {
            g_in_state[i].eq[b].freq    = 100.0f;
            g_in_state[i].eq[b].gain_db = 0.0f;
            g_in_state[i].eq[b].q       = 0.7f;
            g_in_state[i].eq[b].type    = T_PEAK;
        }
        g_in_ch[i].gain_tgt = g_in_ch[i].gain_cur = 1.0f;
        g_in_ch[i].lim.gr   = 1.0f;
    }
    for (int i = 0; i < g_n_out; i++) {
        g_out_state[i].hpf_freq  = 80.0f;
        g_out_state[i].hpf_slope = 12;
        g_out_ch[i].hpf_stages   = 1;
        for (int b = 0; b < MAX_EQ_BANDS; b++) {
            g_out_state[i].eq[b].freq    = 100.0f;
            g_out_state[i].eq[b].gain_db = 0.0f;
            g_out_state[i].eq[b].q       = 0.7f;
            g_out_state[i].eq[b].type    = T_PEAK;
        }
        g_out_state[i].lim.threshold_db = -6.0f;
        g_out_state[i].lim.attack_ms    = 5.0f;
        g_out_state[i].lim.release_ms   = 100.0f;
        g_out_state[i].lim.makeup_db    = 0.0f;
        LimCoeffs lc = calc_limiter(-6.0f, 5.0f, 100.0f, 0.0f, g_sr);
        g_out_ch[i].lim.threshold    = lc.threshold;
        g_out_ch[i].lim.attack_coef  = lc.attack_coef;
        g_out_ch[i].lim.release_coef = lc.release_coef;
        g_out_ch[i].lim.makeup       = lc.makeup;
        g_out_ch[i].lim.gr           = 1.0f;
        g_out_ch[i].gain_tgt = g_out_ch[i].gain_cur = 1.0f;
    }

    /* 기본 라우팅: 단위 행렬 (in_ch N → out_ch N) */
    for (int i = 0; i < MAX_CH; i++)
        for (int j = 0; j < MAX_CH; j++)
            g_route[i][j] = (i == j) ? 1.0f : 0.0f;

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    /* DSP 스레드 시작 */
    pthread_t dsp_tid;
    pthread_create(&dsp_tid, NULL, dsp_thread, NULL);

    /* 리포터 스레드 시작 */
    pthread_t rep_tid;
    g_reporter_running = 1;
    pthread_create(&rep_tid, NULL, reporter_thread, NULL);

    /* 준비 완료 신호 */
    fprintf(stdout, "[aoip_engine] ready client=%s (in=%d out=%d sr=%.0f)\n",
            name, g_n_in, g_n_out, g_sr);
    fflush(stdout);

    /* stdin 명령 루프 (stdin 닫힐 때까지 블록) */
    cmd_loop();

    g_quit             = 1;
    g_reporter_running = 0;
    pthread_join(rep_tid, NULL);
    pthread_join(dsp_tid, NULL);

    /* 장치 중지 */
    for (int i = 0; i < g_n_dev; i++)
        if (g_dev[i].enabled) device_stop(&g_dev[i]);

    /* RTP FIFO 중지 */
    for (int i = 0; i < g_n_rtp_in; i++) {
        if (!g_rtp_in[i].enabled) continue;
        g_rtp_in[i].quit = 1;
        pthread_join(g_rtp_in[i].tid, NULL);
        free(g_rtp_in[i].ring.buf);
    }
    for (int i = 0; i < g_n_rtp_out; i++) {
        if (!g_rtp_out[i].enabled) continue;
        g_rtp_out[i].quit = 1;
        pthread_join(g_rtp_out[i].tid, NULL);
        free(g_rtp_out[i].ring.buf);
    }

    return 0;
}
