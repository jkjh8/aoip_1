/*
 * aoip_engine.c — Unified AoIP 오디오 매트릭스 엔진
 *
 * 스레드 구성:
 *   [P85] dsp_thread          — timerfd 기반 마스터 오디오 루프
 *   [P80] alsa_capture_thread × N  — 장치별 캡처 (alsa_device.c)
 *   [P80] alsa_playback_thread × N — 장치별 재생 (alsa_device.c)
 *   [  ]  reporter_thread     — ~8Hz stdout 레벨 미터
 *   [  ]  stdin cmd_loop      — stdin 명령 파서 (main 스레드)
 *
 * Build:
 *   gcc -O3 -o aoip_engine aoip_engine.c alsa_device.c dsp_math.c \
 *       -I. -lrt -lasound -lsamplerate -lpthread -lm
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
#include <sys/mman.h>
#include <sys/stat.h>

#include "include/engine_constants.h"
#include "include/shm_ring.h"
#include "include/ring_buf.h"
#include "include/dsp_math.h"
#include "include/alsa_device.h"

/* ── RT 우선순위 (CLI로 재정의 가능) ─────────────────────────────── */
static int g_prio_dsp     = 92;
static int g_prio_alsa    = 80;
static int g_prio_ravenna = 95;

/* ── SPSC 명령 링버퍼 ─────────────────────────────────────────────── */
typedef enum {
    CMD_GAIN, CMD_MUTE, CMD_BYPASS,
    CMD_HPF_ENABLE, CMD_HPF_COEFFS, CMD_HPF_STAGES,
    CMD_EQ_ENABLE, CMD_EQ_COEFFS,
    CMD_LIMITER_ENABLE, CMD_LIMITER_PARAMS,
    CMD_ROUTE_SET,
} CmdType;

typedef struct {
    CmdType type;
    int     dir;
    int     ch;
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

/* ── 채널별 DSP 상태 ──────────────────────────────────────────────── */
typedef struct {
    float   gain_tgt, gain_cur;
    int     muted, bypass_dsp;
    int     hpf_enabled, hpf_stages;
    Biquad  hpf[2];
    int     eq_enabled[MAX_EQ_BANDS];
    Biquad  eq[MAX_EQ_BANDS];
    Limiter lim;
} Channel;

/* ── RTP 공유 메모리 연결 ─────────────────────────────────────────── */
typedef struct {
    char    name[32];
    char    shm_name[64];
    int     channels;
    int     ch_start;
    int     enabled;
    ShmRing *shm;
    int     fd;
} ShmBuf;

/* ── 전역 상태 ────────────────────────────────────────────────────── */
volatile int g_quit = 0;  /* alsa_device.c 에서 extern 참조 */

static Device  g_dev[MAX_DEVICES];
static int     g_n_dev = 0;

static ShmBuf  g_rtp_in[MAX_RTP];
static int     g_n_rtp_in = 0;
static ShmBuf  g_rtp_out[MAX_RTP];
static int     g_n_rtp_out = 0;

static Channel g_in_ch[MAX_CH];
static Channel g_out_ch[MAX_CH];
static int     g_n_in = 8, g_n_out = 8;
static float   g_sr   = (float)SAMPLE_RATE;
static int     g_bypass_all_dsp = 0;

static float   g_route[MAX_CH][MAX_CH];

static volatile float g_in_level[MAX_CH];
static volatile float g_out_level[MAX_CH];
static volatile float g_lim_pre[MAX_CH];
static volatile float g_lim_post[MAX_CH];

static volatile int g_reporter_running = 0;

typedef struct {
    float hpf_freq; int hpf_slope;
    struct { float freq, gain_db, q; EqType type; } eq[MAX_EQ_BANDS];
    struct { float threshold_db, attack_ms, release_ms, makeup_db; } lim;
} ChState;

static ChState g_in_state[MAX_CH];
static ChState g_out_state[MAX_CH];

/* ── 시그널 핸들러 ───────────────────────────────────────────────── */
static void sig_handler(int s) { (void)s; g_quit = 1; fclose(stdin); }

/* ── 명령 적용 (DSP 스레드) ──────────────────────────────────────── */
static void apply_cmd(const Cmd *cmd)
{
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

/* ── DSP 스레드 마스터 루프 ──────────────────────────────────────── */
static float g_in_buf[MAX_CH][PERIOD_FRAMES];
static float g_out_buf[MAX_CH][PERIOD_FRAMES];

static void *dsp_thread(void *arg)
{
    (void)arg;

    struct sched_param sp = { .sched_priority = g_prio_dsp };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    int tfd = timerfd_create(CLOCK_MONOTONIC, 0);
    struct itimerspec ts;
    ts.it_value.tv_sec     = 0;
    ts.it_value.tv_nsec    = 1000000LL;
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

        /* ── 입력 읽기: ALSA 장치 ── */
        for (int di = 0; di < g_n_dev; di++) {
            Device *d = &g_dev[di];
            if (!d->enabled || d->mode == 2) continue;

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
                /* 언더런: 페이드아웃으로 클릭 방지 */
                if (d->cap_fade_buf) {
                    float scale = (d->cap_fade_cnt < UNDERRUN_FADE_PERIODS)
                                  ? 1.0f - (float)(d->cap_fade_cnt + 1) / (float)UNDERRUN_FADE_PERIODS
                                  : 0.0f;
                    for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++) {
                        float *dst = g_in_buf[d->ch_start+c];
                        for (int f = 0; f < PERIOD_FRAMES; f++)
                            dst[f] = d->cap_fade_buf[f * d->channels + c] * scale;
                    }
                    if (d->cap_fade_cnt < UNDERRUN_FADE_PERIODS) d->cap_fade_cnt++;
                } else {
                    for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                        memset(g_in_buf[d->ch_start+c], 0, PERIOD_FRAMES*sizeof(float));
                }
                continue;
            }
            if (input_need > DEV_TMP_FRAMES) input_need = DEV_TMP_FRAMES;

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

            long gen = sd.output_frames_gen;
            for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++) {
                float *dst = g_in_buf[d->ch_start+c];
                for (long f = 0; f < gen; f++) dst[f] = d->tmp_cap_out[f*d->channels+c];
                for (long f = gen; f < PERIOD_FRAMES; f++) dst[f] = 0.0f;
            }
            if (d->cap_fade_buf) {
                d->cap_fade_cnt = 0;
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    for (int f = 0; f < PERIOD_FRAMES; f++)
                        d->cap_fade_buf[f * d->channels + c] = g_in_buf[d->ch_start+c][f];
            }
        }

        /* ── 입력 읽기: RTP 공유 메모리 ── */
        for (int ri = 0; ri < g_n_rtp_in; ri++) {
            ShmBuf *r = &g_rtp_in[ri];
            if (!r->enabled || !r->shm) {
                for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                    memset(g_in_buf[r->ch_start+c], 0, PERIOD_FRAMES*sizeof(float));
                continue;
            }
            ShmRing *ring = r->shm;
            uint32_t wp = atomic_load_explicit(&ring->wp, memory_order_acquire);
            uint32_t rp = atomic_load_explicit(&ring->rp, memory_order_relaxed);
            if ((int32_t)(wp - rp) >= PERIOD_FRAMES) {
                for (int f = 0; f < PERIOD_FRAMES; f++) {
                    uint32_t idx = (rp + (uint32_t)f) % (uint32_t)SHM_RING_FRAMES;
                    for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                        g_in_buf[r->ch_start+c][f] = ring->buf[idx * SHM_MAX_CH + c];
                }
                atomic_store_explicit(&ring->rp, rp + (uint32_t)PERIOD_FRAMES,
                                      memory_order_release);
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
                if (!oc->bypass_dsp && !g_bypass_all_dsp) {
                    float post_peak = fabsf(s);
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

        /* ── 출력 기록: ALSA 장치 ── */
        for (int di = 0; di < g_n_dev; di++) {
            Device *d = &g_dev[di];
            if (!d->enabled || d->mode == 1) continue;
            for (int f = 0; f < PERIOD_FRAMES; f++) {
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    d->tmp_play_in[f*d->channels+c] = g_out_buf[d->ch_start+c][f];
            }
            int avail_out = rb_avail(&d->out_ring);
            pi_update(&d->play_pi, avail_out, FILL_TARGET);
            long out_max = (long)ceil((double)PERIOD_FRAMES * d->play_pi.ratio) + 4;
            if (out_max > DEV_TMP_FRAMES) out_max = DEV_TMP_FRAMES;
            if (rb_free(&d->out_ring) < (int)out_max) continue;
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

        /* ── 출력 기록: RTP 공유 메모리 ── */
        for (int ri = 0; ri < g_n_rtp_out; ri++) {
            ShmBuf *r = &g_rtp_out[ri];
            if (!r->enabled || !r->shm) continue;
            ShmRing *ring = r->shm;
            uint32_t wp = atomic_load_explicit(&ring->wp, memory_order_relaxed);
            for (int f = 0; f < PERIOD_FRAMES; f++) {
                uint32_t idx = (wp + (uint32_t)f) % (uint32_t)SHM_RING_FRAMES;
                for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                    ring->buf[idx * SHM_MAX_CH + c] = g_out_buf[r->ch_start+c][f];
            }
            atomic_store_explicit(&ring->wp, wp + (uint32_t)PERIOD_FRAMES,
                                  memory_order_release);
        }
    }

    close(tfd);
    return NULL;
}

/* ── 리포터 스레드 ────────────────────────────────────────────────── */
static void *reporter_thread(void *arg)
{
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

/* ── RTP shm 헬퍼 ────────────────────────────────────────────────── */
static int shmbuf_open(ShmBuf *r, int is_out)
{
    shm_unlink(r->shm_name);
    r->fd = shm_open(r->shm_name, O_RDWR | O_CREAT | O_TRUNC, 0666);
    if (r->fd < 0) {
        fprintf(stderr, "[aoip_engine] shm_open(%s) failed: %s\n",
                r->shm_name, strerror(errno));
        return 0;
    }
    if (ftruncate(r->fd, (off_t)SHMRING_SIZE) < 0) {
        fprintf(stderr, "[aoip_engine] ftruncate %s: %s\n",
                r->shm_name, strerror(errno));
        close(r->fd); r->fd = -1;
        shm_unlink(r->shm_name);
        return 0;
    }
    r->shm = mmap(NULL, SHMRING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, r->fd, 0);
    if (r->shm == MAP_FAILED) {
        fprintf(stderr, "[aoip_engine] mmap %s: %s\n", r->shm_name, strerror(errno));
        close(r->fd); r->fd = -1;
        shm_unlink(r->shm_name);
        r->shm = NULL;
        return 0;
    }
    atomic_init(&r->shm->wp, 0u);
    atomic_init(&r->shm->rp, 0u);
    r->shm->channels = r->channels;
    __atomic_store_n(&r->shm->ring_frames, SHM_RING_FRAMES, __ATOMIC_RELEASE);
    fprintf(stderr, "[aoip_engine] rtp_%s '%s' shm=%s ch=%d ch_start=%d\n",
            is_out ? "out" : "in", r->name, r->shm_name, r->channels, r->ch_start);
    return 1;
}

static void shmbuf_close(ShmBuf *r)
{
    if (r->shm) { munmap(r->shm, SHMRING_SIZE); r->shm = NULL; }
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    shm_unlink(r->shm_name);
    r->enabled = 0;
}

/* ── EQ 타입 파서 ────────────────────────────────────────────────── */
static EqType parse_eq_type(const char *s)
{
    if (!strcmp(s, "loshelf")) return T_LOSHELF;
    if (!strcmp(s, "hishelf")) return T_HISHELF;
    if (!strcmp(s, "lp"))      return T_LP;
    if (!strcmp(s, "hp"))      return T_HP;
    return T_PEAK;
}

/* ── stdin 명령 루프 ─────────────────────────────────────────────── */
static void cmd_loop(void)
{
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
                Device *d = NULL;
                for (int i = 0; i < g_n_dev; i++)
                    if (!strcmp(g_dev[i].name, name)) { d = &g_dev[i]; break; }
                if (d) {
                    device_stop(d);
                } else {
                    if (g_n_dev >= MAX_DEVICES) continue;
                    d = &g_dev[g_n_dev++];
                }
                snprintf(d->name, sizeof(d->name), "%s", name);
                snprintf(d->dev,  sizeof(d->dev),  "%s", tok[3]);
                d->rate     = atoi(tok[4]);
                d->period   = atoi(tok[5]);
                d->nperiods = atoi(tok[6]);
                d->channels = atoi(tok[7]);
                d->ch_start = n >= 9 ? atoi(tok[8]) : (d - g_dev) * 2;
                d->mode     = !strcmp(sub, "add_in")  ? 1 :
                              !strcmp(sub, "add_out") ? 2 : 0;
                d->enabled  = 1;
                d->thread_priority = strstr(d->dev, "RAVENNA") ? g_prio_ravenna : g_prio_alsa;
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
            int in_ch  = atoi(tok[2]) - 1;
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
        if (!strcmp(verb, "rtp_in") && n >= 3) {
            const char *sub  = tok[1];
            const char *name = tok[2];
            if (!strcmp(sub, "add") && n >= 4 && g_n_rtp_in < MAX_RTP) {
                ShmBuf *r = &g_rtp_in[g_n_rtp_in];
                snprintf(r->name,     sizeof(r->name),     "%s", name);
                snprintf(r->shm_name, sizeof(r->shm_name), "%s", tok[3]);
                r->channels = n >= 5 ? atoi(tok[4]) : 2;
                r->ch_start = n >= 6 ? atoi(tok[5]) : g_n_rtp_in * 2;
                r->enabled  = 1; r->fd = -1; r->shm = NULL;
                if (shmbuf_open(r, 0)) g_n_rtp_in++;
                else                   r->enabled = 0;
            } else if (!strcmp(sub, "remove")) {
                for (int i = 0; i < g_n_rtp_in; i++)
                    if (!strcmp(g_rtp_in[i].name, name)) { shmbuf_close(&g_rtp_in[i]); break; }
            }
            continue;
        }

        /* ── rtp_out 명령 ── */
        if (!strcmp(verb, "rtp_out") && n >= 3) {
            const char *sub  = tok[1];
            const char *name = tok[2];
            if (!strcmp(sub, "add") && n >= 4 && g_n_rtp_out < MAX_RTP) {
                ShmBuf *r = &g_rtp_out[g_n_rtp_out];
                snprintf(r->name,     sizeof(r->name),     "%s", name);
                snprintf(r->shm_name, sizeof(r->shm_name), "%s", tok[3]);
                r->channels = n >= 5 ? atoi(tok[4]) : 2;
                r->ch_start = n >= 6 ? atoi(tok[5]) : g_n_rtp_out * 2;
                r->enabled  = 1; r->fd = -1; r->shm = NULL;
                if (shmbuf_open(r, 1)) g_n_rtp_out++;
                else                   r->enabled = 0;
            } else if (!strcmp(sub, "remove")) {
                for (int i = 0; i < g_n_rtp_out; i++)
                    if (!strcmp(g_rtp_out[i].name, name)) { shmbuf_close(&g_rtp_out[i]); break; }
            }
            continue;
        }

        /* ── DSP 명령 ── */
        if (n < 3) continue;
        int dir = strcmp(tok[1], "in") ? 1 : 0;
        int ch  = atoi(tok[2]) - 1;
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
                cmd.type = CMD_HPF_COEFFS; cmd.band = 0; cmd.coeffs = c; cmd_push(&cmd);
                if (cs->hpf_slope >= 24) { cmd.band = 1; cmd_push(&cmd); }
            } else if (!strcmp(param, "slope")) {
                cs->hpf_slope = atoi(tok[4]);
                int stages = (cs->hpf_slope >= 24) ? 2 : 1;
                cmd.type = CMD_HPF_STAGES; cmd.flag = stages; cmd_push(&cmd);
                if (cs->hpf_freq > 0.0f) {
                    BqCoeffs c; calc_hpf(&c, cs->hpf_freq, g_sr);
                    cmd.type = CMD_HPF_COEFFS; cmd.band = 0; cmd.coeffs = c; cmd_push(&cmd);
                    if (stages > 1) { cmd.band = 1; cmd_push(&cmd); }
                }
            }

        } else if (!strcmp(verb, "eq") && n >= 6) {
            int band = atoi(tok[3]);
            if (band < 0 || band >= MAX_EQ_BANDS) continue;
            cmd.band = band;
            const char *param = tok[4];
            if (!strcmp(param, "enable")) {
                cmd.type = CMD_EQ_ENABLE; cmd.flag = atoi(tok[5]); cmd_push(&cmd);
            } else if (!strcmp(param, "coeffs") && n >= 10) {
                BqCoeffs c = {atof(tok[5]),atof(tok[6]),atof(tok[7]),atof(tok[8]),atof(tok[9])};
                cmd.type = CMD_EQ_COEFFS; cmd.coeffs = c; cmd_push(&cmd);
            } else {
                if      (!strcmp(param, "freq")) cs->eq[band].freq    = (float)atof(tok[5]);
                else if (!strcmp(param, "gain")) cs->eq[band].gain_db = (float)atof(tok[5]);
                else if (!strcmp(param, "q"))    cs->eq[band].q       = fmaxf(0.1f,(float)atof(tok[5]));
                else if (!strcmp(param, "type")) cs->eq[band].type    = parse_eq_type(tok[5]);
                else continue;
                BqCoeffs c;
                calc_eq(&c, cs->eq[band].type, cs->eq[band].freq,
                        cs->eq[band].gain_db, cs->eq[band].q, g_sr);
                cmd.type = CMD_EQ_COEFFS; cmd.coeffs = c; cmd_push(&cmd);
            }

        } else if (!strcmp(verb, "limiter") && n >= 5) {
            if (dir != 1) continue;
            const char *param = tok[3];
            if (!strcmp(param, "enable")) {
                cmd.type = CMD_LIMITER_ENABLE; cmd.flag = atoi(tok[4]); cmd_push(&cmd);
            } else {
                if      (!strcmp(param, "threshold")) cs->lim.threshold_db = (float)atof(tok[4]);
                else if (!strcmp(param, "attack"))    cs->lim.attack_ms    = fmaxf(0.1f,(float)atof(tok[4]));
                else if (!strcmp(param, "release"))   cs->lim.release_ms   = fmaxf(1.0f,(float)atof(tok[4]));
                else if (!strcmp(param, "makeup"))    cs->lim.makeup_db    = (float)atof(tok[4]);
                else continue;
                LimCoeffs lc = calc_limiter(cs->lim.threshold_db, cs->lim.attack_ms,
                                            cs->lim.release_ms,   cs->lim.makeup_db, g_sr);
                cmd.type = CMD_LIMITER_PARAMS; cmd.lim_coeffs = lc; cmd_push(&cmd);
            }
        }
    }
}

/* ── main ────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "[aoip_engine] mlockall failed: %s\n", strerror(errno));

    if (argc < 3) {
        fprintf(stderr, "Usage: aoip_engine <n_in> <n_out> [--name <name>]\n");
        return 1;
    }
    g_n_in  = atoi(argv[1]);
    g_n_out = atoi(argv[2]);
    if (g_n_in < 0 || g_n_in > MAX_CH || g_n_out < 0 || g_n_out > MAX_CH) {
        fprintf(stderr, "[aoip_engine] channel count out of range (max %d)\n", MAX_CH);
        return 1;
    }

    const char *name = "aoip_engine";
    for (int i = 3; i < argc; i++) {
        if      (!strcmp(argv[i], "--name")         && i+1 < argc) name             = argv[++i];
        else if (!strcmp(argv[i], "--dsp-prio")     && i+1 < argc) g_prio_dsp     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--alsa-prio")    && i+1 < argc) g_prio_alsa    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ravenna-prio") && i+1 < argc) g_prio_ravenna = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bypass-dsp"))                 g_bypass_all_dsp = 1;
    }

    /* 채널 초기 상태 */
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

    memset(g_route, 0, sizeof(g_route));

    for (int i = 0; i < MAX_RTP; i++) {
        g_rtp_in[i].fd  = -1; g_rtp_in[i].shm  = NULL;
        g_rtp_out[i].fd = -1; g_rtp_out[i].shm = NULL;
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    pthread_t dsp_tid;
    pthread_create(&dsp_tid, NULL, dsp_thread, NULL);

    pthread_t rep_tid;
    g_reporter_running = 1;
    pthread_create(&rep_tid, NULL, reporter_thread, NULL);

    fprintf(stdout, "[aoip_engine] ready client=%s (in=%d out=%d sr=%.0f)\n",
            name, g_n_in, g_n_out, g_sr);
    fflush(stdout);

    cmd_loop();

    g_quit             = 1;
    g_reporter_running = 0;
    pthread_join(rep_tid, NULL);
    pthread_join(dsp_tid, NULL);

    for (int i = 0; i < g_n_dev; i++)
        if (g_dev[i].enabled) device_stop(&g_dev[i]);

    for (int i = 0; i < g_n_rtp_in; i++)
        if (g_rtp_in[i].enabled) shmbuf_close(&g_rtp_in[i]);
    for (int i = 0; i < g_n_rtp_out; i++)
        if (g_rtp_out[i].enabled) shmbuf_close(&g_rtp_out[i]);

    return 0;
}
