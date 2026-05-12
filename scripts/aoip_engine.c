/*
 * aoip_engine.c — Unified AoIP 오디오 매트릭스 엔진
 *
 * 스레드 구성:
 *   [P85] dsp_thread          — I2S 크리스탈(eventfd) 기반 마스터 오디오 루프
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
#include <time.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/eventfd.h>
#include <poll.h>

#include "include/engine_constants.h"
#include "include/shm_ring.h"
#include "include/ring_buf.h"
#include "include/alsa_device.h"
#include "include/rtp_recv.h"
#include "include/rtp_send.h"

/* ── DSP 클럭: RAVENNA 우선, ptp0 폴백 ─────────────────────────── */
#define CLOCKFD 3
#define FD_TO_CLOCKID(fd)  ((~(clockid_t)(fd) << 3) | CLOCKFD)
static int       g_ptp_fd      = -1;
static clockid_t g_ptp_clockid = CLOCK_MONOTONIC;  /* /dev/ptp0 실패 시 폴백 */

/* DSP 타이밍 eventfd — 클럭 마스터 장치(hw:aoip)가 g_period_frames 누적 시 신호 */
int g_dsp_clock_fd = -1;

/* hw:aoip ↔ hw:RAVENNA 클럭 비교 — ALSA htstamp 기반 (DMA 인터럽트 시점)
 * 스레드 지터 없음: 커널이 DMA 완료 시 기록하는 CLOCK_MONOTONIC 타임스탬프 사용
 * frames: 누적 캡처 프레임 수,  hts_ns: 최신 ALSA htstamp (ns) */
_Atomic int64_t g_aoip_frames    = 0;
_Atomic int64_t g_aoip_hts_ns    = 0;
_Atomic int64_t g_ravenna_frames = 0;
_Atomic int64_t g_ravenna_hts_ns = 0;

/* ── 처리 단위 프레임 수 (런타임 설정 가능, alsa_device.c extern 참조) */
int g_period_frames = DEFAULT_PERIOD_FRAMES;

/* ── RT 우선순위 (CLI로 재정의 가능) ─────────────────────────────── */
static int g_prio_dsp     = 92;
static int g_prio_alsa    = 80;
static int g_prio_ravenna = 95;
static int g_prio_rtp     = 45;

/* ── SPSC 명령 링버퍼 ─────────────────────────────────────────────── */
typedef enum {
    CMD_GAIN, CMD_MUTE, CMD_BYPASS,
    CMD_ROUTE_SET,
} CmdType;

typedef struct {
    CmdType type;
    int     dir;
    int     ch;
    union {
        float gain;
        int   flag;
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

/* ── 채널별 DSP 상태 (볼륨 + 레벨 미터만) ────────────────────────── */
typedef struct {
    float gain_tgt, gain_cur;
    int   muted, bypass_dsp;
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
    int     is_send;   /* 1 = rtp_out (RtpSendCtx), 0 = rtp_in (RtpRecvCtx) */
    void   *rtp_ctx;   /* RtpRecvCtx * or RtpSendCtx * */
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

static volatile int g_reporter_running = 0;

/* ── 병렬 DSP 워커 ────────────────────────────────────────────────── */
typedef struct {
    int id;
    int ch_start;
    int ch_count;
    int n_in;
    int n_out;
} WorkerArg;

static WorkerArg         g_worker_arg[DSP_WORKER_COUNT];
static pthread_t         g_worker_tid[DSP_WORKER_COUNT];
static pthread_barrier_t g_barrier_work_start;
static pthread_barrier_t g_barrier_input_done;   /* 입력 DSP 완료 후 라우팅 전 동기화 */
static pthread_barrier_t g_barrier_routing_done; /* 라우팅 완료 후 출력 DSP 전 동기화 */
static pthread_barrier_t g_barrier_work_done;
static volatile int      g_worker_quit = 0;


/* ── 시그널 핸들러 ───────────────────────────────────────────────── */
static void sig_handler(int s) { (void)s; g_quit = 1; close(STDIN_FILENO); }

/* ── 명령 적용 (DSP 스레드) ──────────────────────────────────────── */
static void apply_cmd(const Cmd *cmd)
{
    Channel *ch = (cmd->dir == 0) ? &g_in_ch[cmd->ch] : &g_out_ch[cmd->ch];
    switch (cmd->type) {
    case CMD_GAIN:   ch->gain_tgt = cmd->gain; break;
    case CMD_MUTE:   ch->muted = cmd->flag; if (ch->muted) ch->gain_cur = ch->gain_tgt; break;
    case CMD_BYPASS: ch->bypass_dsp = cmd->flag; break;
    case CMD_ROUTE_SET:
        if (cmd->route.out_ch < MAX_CH && cmd->route.in_ch < MAX_CH)
            g_route[cmd->route.out_ch][cmd->route.in_ch] = cmd->route.level;
        break;
    }
}

/* ── DSP 처리 함수 (채널 슬라이스 단위, 워커/마스터 공용) ─────────── */
static float g_in_buf[MAX_CH][MAX_PERIOD_FRAMES];
static float g_out_buf[MAX_CH][MAX_PERIOD_FRAMES];

static void process_input_dsp(int ch_start, int ch_count, int n_in)
{
    for (int ch = ch_start; ch < ch_start + ch_count && ch < n_in; ch++) {
        Channel *ic  = &g_in_ch[ch];
        float   *buf = g_in_buf[ch];
        if (ic->muted) {
            memset(buf, 0, g_period_frames * sizeof(float));
            ic->gain_cur = ic->gain_tgt;
            continue;
        }
        float cur  = ic->gain_cur;
        float step = (ic->gain_tgt - cur) / (float)g_period_frames;
        float peak = 0.0f;
        for (int i = 0; i < g_period_frames; i++) {
            cur += step;
            buf[i] *= cur;
            float ap = fabsf(buf[i]);
            if (ap > peak) peak = ap;
        }
        ic->gain_cur = ic->gain_tgt;
        if (peak > g_in_level[ch]) g_in_level[ch] = peak;
    }
}

static void process_routing(int out_start, int out_count, int n_in, int n_out)
{
    for (int out = out_start; out < out_start + out_count && out < n_out; out++) {
        memset(g_out_buf[out], 0, g_period_frames * sizeof(float));
        for (int in = 0; in < n_in; in++) {
            float gain = g_route[out][in];
            if (gain == 0.0f) continue;
            for (int f = 0; f < g_period_frames; f++)
                g_out_buf[out][f] += g_in_buf[in][f] * gain;
        }
    }
}

static void process_output_dsp(int ch_start, int ch_count, int n_out)
{
    for (int ch = ch_start; ch < ch_start + ch_count && ch < n_out; ch++) {
        Channel *oc  = &g_out_ch[ch];
        float   *buf = g_out_buf[ch];
        if (oc->muted) {
            memset(buf, 0, g_period_frames * sizeof(float));
            oc->gain_cur = oc->gain_tgt;
            continue;
        }
        float cur  = oc->gain_cur;
        float step = (oc->gain_tgt - cur) / (float)g_period_frames;
        float peak = 0.0f;
        for (int i = 0; i < g_period_frames; i++) {
            cur += step;
            buf[i] *= cur;
            float ap = fabsf(buf[i]);
            if (ap > peak) peak = ap;
        }
        oc->gain_cur = oc->gain_tgt;
        if (peak > g_out_level[ch]) g_out_level[ch] = peak;
    }
}

static void *dsp_worker_thread(void *arg)
{
    WorkerArg *w = (WorkerArg *)arg;

    struct sched_param sp = { .sched_priority = g_prio_dsp };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* RT 스택 page-fault 방지 */
    volatile char stack_touch[4096];
    memset((void *)stack_touch, 0, sizeof(stack_touch));

    while (1) {
        pthread_barrier_wait(&g_barrier_work_start);
        if (g_worker_quit) {
            pthread_barrier_wait(&g_barrier_input_done);
            pthread_barrier_wait(&g_barrier_routing_done);
            pthread_barrier_wait(&g_barrier_work_done);
            break;
        }
        process_input_dsp(w->ch_start, w->ch_count, w->n_in);
        pthread_barrier_wait(&g_barrier_input_done);
        process_routing(w->ch_start, w->ch_count, w->n_in, w->n_out);
        pthread_barrier_wait(&g_barrier_routing_done);
        process_output_dsp(w->ch_start, w->ch_count, w->n_out);
        pthread_barrier_wait(&g_barrier_work_done);
    }
    return NULL;
}

/* ── DSP 스레드 마스터 루프 ──────────────────────────────────────── */

static void *dsp_thread(void *arg)
{
    (void)arg;

    struct sched_param sp = { .sched_priority = g_prio_dsp };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);

    /* ── DSP 클럭 상태 (ptp0 폴백) ── */
    struct timespec _ptp_init;
    clock_gettime(g_ptp_clockid, &_ptp_init);
    long long ptp_base_ns   = _ptp_init.tv_sec * 1000000000LL + _ptp_init.tv_nsec;
    long long dsp_tick      = 0;
    long long cur_period_ns = 0;

    /* ── hw:aoip ↔ hw:RAVENNA 클럭 비교 상태 ── */
    int64_t clk2_pa_fr  = 0;  /* 직전 aoip 누적 프레임 */
    int64_t clk2_pa_hts = 0;  /* 직전 aoip ALSA htstamp (ns) */
    int64_t clk2_pr_fr  = 0;  /* 직전 ravenna 누적 프레임 */
    int64_t clk2_pr_hts = 0;  /* 직전 ravenna ALSA htstamp (ns) */
    int64_t clk2_next_ns = 0; /* 다음 보고 시각 */
    {
        struct timespec _now;
        clock_gettime(CLOCK_MONOTONIC, &_now);
        clk2_next_ns = (int64_t)_now.tv_sec * 1000000000LL + _now.tv_nsec
                       + 30LL * 1000000000LL;
    }

    while (!g_quit) {
        /* g_period_frames는 cmd_loop에서 변경될 수 있으므로 매 틱 갱신 */
        long long period_ns = (long long)g_period_frames * 1000000000LL / SAMPLE_RATE;

        /* ── DSP 타이밍: hw:aoip 클럭 마스터 우선, ptp0 폴백 ── */
        if (g_dsp_clock_fd >= 0) {
            /* 클럭 마스터(hw:aoip) 캡처가 g_period_frames 누적 시 eventfd 신호 → DSP 틱
             * poll timeout = 3×period (소스 없을 때 무한 블로킹 방지) */
            struct pollfd _pfd = { .fd = g_dsp_clock_fd, .events = POLLIN };
            int timeout_ms = (int)(period_ns * 3 / 1000000LL);
            if (timeout_ms < 4) timeout_ms = 4;
            int _ready = poll(&_pfd, 1, timeout_ms);
            if (g_quit) break;
            if (_ready > 0 && (_pfd.revents & POLLIN)) {
                uint64_t _val;
                (void)read(g_dsp_clock_fd, &_val, sizeof(_val));
                /* EFD_SEMAPHORE: 1 틱 소비 — 즉시 DSP 처리 */
            }
            /* timeout: RAVENNA 소스 없음 — DSP는 계속 (I2S 유지) */

            /* ptp0 폴백 상태를 현재 시각으로 유지 (hw:aoip→ptp0 전환 시 연속성) */
            if (period_ns != cur_period_ns) {
                clock_gettime(CLOCK_MONOTONIC, &_ptp_init);
                ptp_base_ns   = _ptp_init.tv_sec * 1000000000LL + _ptp_init.tv_nsec;
                dsp_tick      = 0;
                cur_period_ns = period_ns;
            }
        } else {
            /* CLOCK_MONOTONIC 폴백 (hw:aoip 미기동 시) */
            if (period_ns != cur_period_ns) {
                clock_gettime(CLOCK_MONOTONIC, &_ptp_init);
                ptp_base_ns   = _ptp_init.tv_sec * 1000000000LL + _ptp_init.tv_nsec;
                dsp_tick      = 0;
                cur_period_ns = period_ns;
            }

            dsp_tick++;
            long long tgt_ns = ptp_base_ns + dsp_tick * period_ns;

            struct timespec _now;
            clock_gettime(CLOCK_MONOTONIC, &_now);
            long long now_ns    = _now.tv_sec * 1000000000LL + _now.tv_nsec;
            long long remain_ns = tgt_ns - now_ns;

            if (remain_ns > 10 * period_ns || remain_ns < -10 * period_ns) {
                fprintf(stderr, "[aoip_engine] DSP clock resync (%.1fms)\n",
                        (double)remain_ns / 1e6);
                clock_gettime(CLOCK_MONOTONIC, &_ptp_init);
                ptp_base_ns = _ptp_init.tv_sec * 1000000000LL + _ptp_init.tv_nsec;
                dsp_tick    = 0;
            } else if (remain_ns > 0) {
                struct timespec _tgt = {
                    .tv_sec  = tgt_ns / 1000000000LL,
                    .tv_nsec = (long)(tgt_ns % 1000000000LL),
                };
                clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &_tgt, NULL);
            }
        }
        if (g_quit) break;

        /* ── hw:aoip ↔ hw:RAVENNA 클럭 비교 (30s 주기) ─────────────────
         * ALSA htstamp: DMA 인터럽트 시점 커널 타임스탬프 → 스레드 지터 없음
         * rate = delta_frames / delta_htstamp_s  →  drift_ppm = (aoip-ravenna)/48000*1e6 */
        {
            struct timespec _now;
            clock_gettime(CLOCK_MONOTONIC, &_now);
            int64_t now_ns = (int64_t)_now.tv_sec * 1000000000LL + _now.tv_nsec;

            if (now_ns >= clk2_next_ns) {
                int64_t a_fr  = atomic_load_explicit(&g_aoip_frames,    memory_order_acquire);
                int64_t a_hts = atomic_load_explicit(&g_aoip_hts_ns,    memory_order_acquire);
                int64_t r_fr  = atomic_load_explicit(&g_ravenna_frames,  memory_order_acquire);
                int64_t r_hts = atomic_load_explicit(&g_ravenna_hts_ns,  memory_order_acquire);

                if (clk2_pa_hts > 0 && clk2_pr_hts > 0 &&
                    a_hts > clk2_pa_hts && r_hts > clk2_pr_hts) {

                    int64_t da_fr  = a_fr  - clk2_pa_fr;
                    int64_t da_hts = a_hts - clk2_pa_hts;
                    int64_t dr_fr  = r_fr  - clk2_pr_fr;
                    int64_t dr_hts = r_hts - clk2_pr_hts;

                    double aoip_rate    = (double)da_fr * 1e9 / (double)da_hts;
                    double ravenna_rate = (double)dr_fr * 1e9 / (double)dr_hts;
                    double drift_ppm    = (aoip_rate - ravenna_rate) / SAMPLE_RATE * 1e6;
                    double elapsed_s    = (double)da_hts / 1e9;

                    double dsp_tick_ms = (double)g_period_frames / aoip_rate * 1000.0;
                    printf("clk2 aoip_rate=%.3f ravenna_rate=%.3f drift_ppm=%+.3f elapsed=%.0f"
                           " dsp_period=%d dsp_tick_ms=%.3f dsp_src=%s\n",
                           aoip_rate, ravenna_rate, drift_ppm, elapsed_s,
                           g_period_frames, dsp_tick_ms,
                           g_dsp_clock_fd >= 0 ? "aoip" : "monotonic");
                    fflush(stdout);
                }

                clk2_pa_fr  = a_fr;  clk2_pa_hts = a_hts;
                clk2_pr_fr  = r_fr;  clk2_pr_hts = r_hts;
                clk2_next_ns = now_ns + 30LL * 1000000000LL;
            }
        }

        /* ── 명령 링 드레인 ── */
        Cmd cmd;
        while (cmd_pop(&cmd)) apply_cmd(&cmd);

        /* ── 입력 읽기: ALSA 장치 ── */
        for (int di = 0; di < g_n_dev; di++) {
            Device *d = &g_dev[di];
            if (!d->enabled || d->mode == 2) continue;

            /* RAVENNA: PTP 미잠금(EIO) 중 뮤트 */
            if (d->is_ravenna && !atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_acquire)) {
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    memset(g_in_buf[d->ch_start+c], 0, g_period_frames*sizeof(float));
                continue;
            }

            if (d->is_clock_master) {
                /* hw:aoip: DSP 클럭 마스터 — SRC 불필요, in_ring 직접 읽기 */
                if (rb_avail(&d->in_ring) >= g_period_frames) {
                    rb_read(&d->in_ring, d->tmp_cap_in, g_period_frames);
                    for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                        for (int f = 0; f < g_period_frames; f++)
                            g_in_buf[d->ch_start+c][f] = d->tmp_cap_in[f*d->channels+c];
                } else {
                    for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                        memset(g_in_buf[d->ch_start+c], 0, g_period_frames*sizeof(float));
                }
                continue;
            }
            /* RAVENNA 및 I2S 기타: SRC로 클럭 도메인 차이 보정 */

            int avail = rb_avail(&d->in_ring);

            if (!d->cap_pi.prebuf_done) {
                if (avail < PREBUF_FRAMES) {
                    for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                        memset(g_in_buf[d->ch_start+c], 0, g_period_frames*sizeof(float));
                    continue;
                }
                d->cap_pi.prebuf_done = 1;
            }

            pi_update(&d->cap_pi, avail, FILL_TARGET);

            int input_need = (int)ceil((double)g_period_frames / d->cap_pi.ratio) + 2;
            if (input_need > avail) {
                /* 언더런: 페이드아웃으로 클릭 방지 */
                if (d->cap_fade_buf) {
                    float scale = (d->cap_fade_cnt < UNDERRUN_FADE_PERIODS)
                                  ? 1.0f - (float)(d->cap_fade_cnt + 1) / (float)UNDERRUN_FADE_PERIODS
                                  : 0.0f;
                    for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++) {
                        float *dst = g_in_buf[d->ch_start+c];
                        for (int f = 0; f < g_period_frames; f++)
                            dst[f] = d->cap_fade_buf[f * d->channels + c] * scale;
                    }
                    if (d->cap_fade_cnt < UNDERRUN_FADE_PERIODS) d->cap_fade_cnt++;
                } else {
                    for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                        memset(g_in_buf[d->ch_start+c], 0, g_period_frames*sizeof(float));
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
                .output_frames = g_period_frames,
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
                for (long f = gen; f < g_period_frames; f++) dst[f] = 0.0f;
            }
            if (d->cap_fade_buf) {
                d->cap_fade_cnt = 0;
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    for (int f = 0; f < g_period_frames; f++)
                        d->cap_fade_buf[f * d->channels + c] = g_in_buf[d->ch_start+c][f];
            }
        }

        /* ── 입력 읽기: RTP 공유 메모리 ── */
        for (int ri = 0; ri < g_n_rtp_in; ri++) {
            ShmBuf *r = &g_rtp_in[ri];
            if (!r->enabled || !r->shm) {
                for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                    memset(g_in_buf[r->ch_start+c], 0, g_period_frames*sizeof(float));
                continue;
            }
            ShmRing *ring = r->shm;
            uint32_t wp = atomic_load_explicit(&ring->wp, memory_order_acquire);
            uint32_t rp = atomic_load_explicit(&ring->rp, memory_order_relaxed);
            if ((int32_t)(wp - rp) >= g_period_frames) {
                for (int f = 0; f < g_period_frames; f++) {
                    uint32_t idx = (rp + (uint32_t)f) % (uint32_t)SHM_RING_FRAMES;
                    for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                        g_in_buf[r->ch_start+c][f] = ring->buf[idx * SHM_MAX_CH + c];
                }
                atomic_store_explicit(&ring->rp, rp + (uint32_t)g_period_frames,
                                      memory_order_release);
            } else {
                for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                    memset(g_in_buf[r->ch_start+c], 0, g_period_frames*sizeof(float));
            }
        }

        /* ── 병렬 DSP: 입력 DSP + 라우팅 + 출력 DSP ── */
        /* 워커에 채널 수 전달 (배리어 전 — 워커는 아직 대기 중) */
        for (int w = 0; w < DSP_WORKER_COUNT; w++) {
            g_worker_arg[w].n_in  = g_n_in;
            g_worker_arg[w].n_out = g_n_out;
        }
        /* 워커 해제 + 마스터는 ch 0..(DSP_WORKER_CH-1) 담당 */
        pthread_barrier_wait(&g_barrier_work_start);
        process_input_dsp(0, DSP_WORKER_CH, g_n_in);
        pthread_barrier_wait(&g_barrier_input_done);   /* 전체 입력 DSP 완료 대기 */
        process_routing(0, DSP_WORKER_CH, g_n_in, g_n_out);
        pthread_barrier_wait(&g_barrier_routing_done); /* 전체 라우팅 완료 대기 */
        process_output_dsp(0, DSP_WORKER_CH, g_n_out);
        pthread_barrier_wait(&g_barrier_work_done);

        /* ── 출력 기록: ALSA 장치 ── */
        for (int di = 0; di < g_n_dev; di++) {
            Device *d = &g_dev[di];
            if (!d->enabled || d->mode == 1) continue;

            /* 출력 데이터 인터리브 패킹 */
            for (int f = 0; f < g_period_frames; f++)
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    d->tmp_play_in[f*d->channels+c] = g_out_buf[d->ch_start+c][f];

            if (d->is_clock_master) {
                /* hw:aoip: DSP 클럭 마스터 — SRC 불필요, out_ring 직접 쓰기 */
                if (rb_free(&d->out_ring) >= g_period_frames)
                    rb_write(&d->out_ring, d->tmp_play_in, g_period_frames);
                continue;
            }

            /* RAVENNA 및 I2S 기타: SRC로 클럭 도메인 변환 */
            int avail_out = rb_avail(&d->out_ring);
            pi_update(&d->play_pi, avail_out, FILL_TARGET);
            long out_max = (long)ceil((double)g_period_frames * d->play_pi.ratio) + 4;
            if (out_max > DEV_TMP_FRAMES) out_max = DEV_TMP_FRAMES;
            if (rb_free(&d->out_ring) < (int)out_max) continue;
            SRC_DATA sd = {
                .data_in       = d->tmp_play_in,
                .data_out      = d->tmp_play_out,
                .input_frames  = g_period_frames,
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
            for (int f = 0; f < g_period_frames; f++) {
                uint32_t idx = (wp + (uint32_t)f) % (uint32_t)SHM_RING_FRAMES;
                for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                    ring->buf[idx * SHM_MAX_CH + c] = g_out_buf[r->ch_start+c][f];
            }
            atomic_store_explicit(&ring->wp, wp + (uint32_t)g_period_frames,
                                  memory_order_release);
        }
    }

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
        fflush(stdout);
    }
    return NULL;
}

/* ── RTP shm 헬퍼 ────────────────────────────────────────────────── */

typedef struct {
    ShmRing *ring;
    char     key[32];
    char     sock_path[256];
    int      is_send;
    ShmBuf  *buf;
} RtpLaunchArg;

static void *rtp_launch_thread(void *arg)
{
    RtpLaunchArg *la = (RtpLaunchArg *)arg;
    if (la->is_send)
        la->buf->rtp_ctx = rtp_send_start(la->ring, la->key, la->sock_path, g_prio_rtp);
    else
        la->buf->rtp_ctx = rtp_recv_start(la->ring, la->key, la->sock_path, g_prio_rtp);
    free(la);
    return NULL;
}

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
    r->is_send  = is_out;
    r->rtp_ctx  = NULL;

    fprintf(stderr, "[aoip_engine] rtp_%s '%s' shm=%s ch=%d ch_start=%d\n",
            is_out ? "out" : "in", r->name, r->shm_name, r->channels, r->ch_start);

    /* RTP recv/send 스레드 기동 (Node.js 소켓 연결은 비동기로 처리) */
    RtpLaunchArg *la = malloc(sizeof(RtpLaunchArg));
    if (la) {
        la->ring    = r->shm;
        la->is_send = is_out;
        la->buf     = r;
        snprintf(la->key, sizeof(la->key), "%s", r->name);
        snprintf(la->sock_path, sizeof(la->sock_path),
                 "/run/aoip/%s_%s.sock",
                 is_out ? "rtp_send" : "rtp_recv", r->name);
        pthread_t tid;
        pthread_attr_t attr;
        pthread_attr_init(&attr);
        pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
        pthread_create(&tid, &attr, rtp_launch_thread, la);
        pthread_attr_destroy(&attr);
    }

    return 1;
}

static void shmbuf_close(ShmBuf *r)
{
    if (r->rtp_ctx) {
        if (r->is_send)
            rtp_send_stop((RtpSendCtx *)r->rtp_ctx);
        else
            rtp_recv_stop((RtpRecvCtx *)r->rtp_ctx);
        r->rtp_ctx = NULL;
    }
    if (r->shm) { munmap(r->shm, SHMRING_SIZE); r->shm = NULL; }
    if (r->fd >= 0) { close(r->fd); r->fd = -1; }
    shm_unlink(r->shm_name);
    r->enabled = 0;
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

        /* ── set period N — DSP 시작 전에만 유효 ── */
        if (!strcmp(verb, "set") && n >= 3 && !strcmp(tok[1], "period")) {
            int pf = atoi(tok[2]);
            if (pf >= 1 && pf <= MAX_PERIOD_FRAMES) {
                g_period_frames = pf;
                fprintf(stderr, "[aoip_engine] period_frames set to %d\n", g_period_frames);
            } else {
                fprintf(stderr, "[aoip_engine] set period: invalid value %d (1..%d)\n",
                        pf, MAX_PERIOD_FRAMES);
            }
            continue;
        }

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
                d->is_ravenna      = strstr(d->dev, "RAVENNA") ? 1 : 0;
                d->thread_priority = d->is_ravenna ? g_prio_ravenna : g_prio_alsa;
                /* analog 장치(I2S)를 DSP 마스터 클럭으로 지정 */
                d->is_clock_master = (!strcmp(name, "analog") && d->mode != 2) ? 1 : 0;
                d->clk_accum       = 0;
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

        Cmd cmd = { .dir = dir, .ch = ch };

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
        }
    }
}

/* ── Unix socket helpers ─────────────────────────────────────────── */
#include <sys/socket.h>
#include <sys/un.h>

static int unix_connect(const char *path, int retries, int ms)
{
    struct sockaddr_un addr = {0};
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, path, sizeof(addr.sun_path) - 1);
    for (int i = 0; i <= retries; i++) {
        int fd = socket(AF_UNIX, SOCK_STREAM, 0);
        if (fd < 0) return -1;
        if (connect(fd, (struct sockaddr *)&addr, sizeof(addr)) == 0) return fd;
        close(fd);
        if (i < retries) usleep(ms * 1000);
    }
    return -1;
}

static int read_line_fd(int fd, char *buf, int maxlen)
{
    int n = 0; char c;
    while (n < maxlen - 1) {
        if (read(fd, &c, 1) <= 0) break;
        if (c == '\n') break;
        if (c != '\r') buf[n++] = c;
    }
    buf[n] = '\0';
    return n;
}

static int cfg_int(const char *s, const char *key, int def)
{
    char pat[64]; int v = def;
    snprintf(pat, sizeof(pat), "%s=%%d", key);
    const char *p = strstr(s, key);
    if (p) sscanf(p, pat, &v);
    return v;
}

/* ── audio.json 간단 파서 ────────────────────────────────────────── */
static int json_int(const char *json, const char *key, int def)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return def;
    p += strlen(pat);
    while (*p == ' ' || *p == ':' || *p == '\t') p++;
    if (*p < '0' || *p > '9') return def;
    return atoi(p);
}

static void load_config_prios(const char *path)
{
    FILE *f = fopen(path, "r");
    if (!f) return;

    /* engine 섹션만 읽기 */
    char buf[4096] = "";
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';

    /* "engine" 섹션 위치 찾기 */
    const char *sec = strstr(buf, "\"engine\"");
    if (!sec) return;
    const char *start = strchr(sec, '{');
    if (!start) return;
    const char *end = strchr(start, '}');
    if (!end) return;

    char section[512] = "";
    size_t len = (size_t)(end - start + 1);
    if (len >= sizeof(section)) len = sizeof(section) - 1;
    memcpy(section, start, len);
    section[len] = '\0';

    int v;
    if ((v = json_int(section, "dspPrio",     0)) > 0) g_prio_dsp     = v;
    if ((v = json_int(section, "alsaPrio",    0)) > 0) g_prio_alsa    = v;
    if ((v = json_int(section, "ravennaPrio", 0)) > 0) g_prio_ravenna = v;
    if ((v = json_int(section, "rtpPrio",     0)) > 0) g_prio_rtp     = v;

    fprintf(stderr, "[aoip_engine] config prios: dsp=%d alsa=%d ravenna=%d rtp=%d\n",
            g_prio_dsp, g_prio_alsa, g_prio_ravenna, g_prio_rtp);
}

/* ── main ────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "[aoip_engine] mlockall failed: %s\n", strerror(errno));

    /* audio.json에서 우선순위 로드 (커맨드라인으로 덮어쓰기 가능) */
    load_config_prios("config/audio.json");

    const char *name = "aoip_engine";
    for (int i = 1; i < argc; i++) {
        if      (!strcmp(argv[i], "--name")         && i+1 < argc) name             = argv[++i];
        else if (!strcmp(argv[i], "--dsp-prio")     && i+1 < argc) g_prio_dsp     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--alsa-prio")    && i+1 < argc) g_prio_alsa    = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--ravenna-prio") && i+1 < argc) g_prio_ravenna = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--rtp-prio")     && i+1 < argc) g_prio_rtp     = atoi(argv[++i]);
        else if (!strcmp(argv[i], "--bypass-dsp"))                 g_bypass_all_dsp = 1;
    }

    /* Node.js 소켓 서버에 연결, init 라인 수신 */
    int sfd = unix_connect("/run/aoip/engine.sock", 30, 100);
    if (sfd < 0) {
        fprintf(stderr, "[aoip_engine] cannot connect to /run/aoip/engine.sock\n");
        return 1;
    }

    char init_line[128] = "";
    read_line_fd(sfd, init_line, sizeof(init_line));
    g_n_in  = cfg_int(init_line, "n_in",  0);
    g_n_out = cfg_int(init_line, "n_out", 0);

    if (g_n_in < 0 || g_n_in > MAX_CH || g_n_out < 0 || g_n_out > MAX_CH) {
        fprintf(stderr, "[aoip_engine] channel count out of range (in=%d out=%d max=%d)\n",
                g_n_in, g_n_out, MAX_CH);
        close(sfd);
        return 1;
    }

    /* socket → stdin (명령) + stdout (ready/lvl/lm) */
    dup2(sfd, STDIN_FILENO);
    dup2(sfd, STDOUT_FILENO);
    close(sfd);

    /* 채널 초기 상태 */
    for (int i = 0; i < g_n_in; i++)
        g_in_ch[i].gain_tgt = g_in_ch[i].gain_cur = 1.0f;
    for (int i = 0; i < g_n_out; i++)
        g_out_ch[i].gain_tgt = g_out_ch[i].gain_cur = 1.0f;

    memset(g_route, 0, sizeof(g_route));

    for (int i = 0; i < MAX_RTP; i++) {
        g_rtp_in[i].fd  = -1; g_rtp_in[i].shm  = NULL;
        g_rtp_out[i].fd = -1; g_rtp_out[i].shm = NULL;
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    /* DSP 클럭 마스터용 eventfd 생성 (EFD_SEMAPHORE: 틱당 1 카운트) */
    g_dsp_clock_fd = eventfd(0, EFD_SEMAPHORE);
    if (g_dsp_clock_fd < 0)
        fprintf(stderr, "[aoip_engine] eventfd: %s — dsp clock master disabled\n",
                strerror(errno));
    else
        fprintf(stderr, "[aoip_engine] dsp clock eventfd=%d ready\n",
                g_dsp_clock_fd);

    /* /dev/ptp0 를 DSP 폴백 클럭으로 오픈 (RAVENNA 없을 때 사용) */
    g_ptp_fd = open("/dev/ptp0", O_RDONLY);
    if (g_ptp_fd >= 0) {
        g_ptp_clockid = FD_TO_CLOCKID(g_ptp_fd);
        struct timespec _probe;
        if (clock_gettime(g_ptp_clockid, &_probe) < 0) {
            fprintf(stderr, "[aoip_engine] /dev/ptp0 clock_gettime failed: %s, fallback CLOCK_MONOTONIC\n",
                    strerror(errno));
            close(g_ptp_fd); g_ptp_fd = -1;
            g_ptp_clockid = CLOCK_MONOTONIC;
        } else {
            fprintf(stderr, "[aoip_engine] ptp0 fallback clock ready (clockid=%d)\n",
                    (int)g_ptp_clockid);
        }
    } else {
        fprintf(stderr, "[aoip_engine] /dev/ptp0 open failed: %s, fallback CLOCK_MONOTONIC\n",
                strerror(errno));
    }

    /* 병렬 DSP 워커 초기화 */
    pthread_barrier_init(&g_barrier_work_start,   NULL, DSP_WORKER_COUNT + 1);
    pthread_barrier_init(&g_barrier_input_done,   NULL, DSP_WORKER_COUNT + 1);
    pthread_barrier_init(&g_barrier_routing_done, NULL, DSP_WORKER_COUNT + 1);
    pthread_barrier_init(&g_barrier_work_done,    NULL, DSP_WORKER_COUNT + 1);
    for (int w = 0; w < DSP_WORKER_COUNT; w++) {
        g_worker_arg[w].id       = w;
        g_worker_arg[w].ch_start = (w + 1) * DSP_WORKER_CH;  /* 마스터가 0 담당, 워커는 그 이상 */
        g_worker_arg[w].ch_count = DSP_WORKER_CH;
        pthread_create(&g_worker_tid[w], NULL, dsp_worker_thread, &g_worker_arg[w]);
    }

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

    /* 워커 스레드 종료 */
    g_worker_quit = 1;
    pthread_barrier_wait(&g_barrier_work_start);
    pthread_barrier_wait(&g_barrier_input_done);
    pthread_barrier_wait(&g_barrier_routing_done);
    pthread_barrier_wait(&g_barrier_work_done);
    for (int w = 0; w < DSP_WORKER_COUNT; w++)
        pthread_join(g_worker_tid[w], NULL);
    pthread_barrier_destroy(&g_barrier_work_start);
    pthread_barrier_destroy(&g_barrier_input_done);
    pthread_barrier_destroy(&g_barrier_routing_done);
    pthread_barrier_destroy(&g_barrier_work_done);

    for (int i = 0; i < g_n_dev; i++)
        if (g_dev[i].enabled) device_stop(&g_dev[i]);

    for (int i = 0; i < g_n_rtp_in; i++)
        if (g_rtp_in[i].enabled) shmbuf_close(&g_rtp_in[i]);
    for (int i = 0; i < g_n_rtp_out; i++)
        if (g_rtp_out[i].enabled) shmbuf_close(&g_rtp_out[i]);

    if (g_ptp_fd >= 0) { close(g_ptp_fd); g_ptp_fd = -1; }
    if (g_dsp_clock_fd >= 0) { close(g_dsp_clock_fd); g_dsp_clock_fd = -1; }

    return 0;
}
