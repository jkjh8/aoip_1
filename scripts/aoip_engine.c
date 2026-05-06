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
 *   make -C scripts/   (또는 make -j$(nproc))
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
#include <sched.h>

#include "include/engine_constants.h"
#include "include/shm_ring.h"
#include "include/ring_buf.h"
#include "include/alsa_device.h"
#include "include/dsp_neon.h"
#include "include/rtp_recv.h"
#include "include/rtp_send.h"
#include "include/clk2.h"
#include "include/rtp_utils.h"
#include "include/rtp_stream.h"
#include "include/dsp_src.h"

/* DSP 타이밍 eventfd — 클럭 마스터 장치(hw:aoip)가 g_period_frames 누적 시 신호 */
int g_dsp_clock_fd = -1;

/* ── 처리 단위 프레임 수 (런타임 설정 가능, alsa_device.c extern 참조) */
int g_period_frames = DEFAULT_PERIOD_FRAMES;

/* ── RT 우선순위 (CLI로 재정의 가능) ─────────────────────────────── */
static int g_prio_dsp     = 59;
static int g_prio_alsa    = 58;
static int g_prio_ravenna = 57;
static int g_prio_rtp     = 56;

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
        sched_yield();
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

/* ── 전역 상태 ────────────────────────────────────────────────────── */
_Atomic int g_quit = 0;   /* alsa_device.c 에서 extern 참조 */

static Device  g_dev[MAX_DEVICES];
static int     g_n_dev = 0;

static RtpStream  g_rtp_in[MAX_RTP];
static int     g_n_rtp_in = 0;
static RtpStream  g_rtp_out[MAX_RTP];
static int     g_n_rtp_out = 0;

static Channel g_in_ch[MAX_CH];
static Channel g_out_ch[MAX_CH];
static int     g_n_in = 8, g_n_out = 8;
static float   g_sr   = (float)SAMPLE_RATE;
static int     g_bypass_all_dsp = 0;

static float   g_route[MAX_CH][MAX_CH];

static _Atomic float g_in_level[MAX_CH];
static _Atomic float g_out_level[MAX_CH];

static volatile int g_reporter_running = 0;
static volatile int g_lvl_report       = 1;

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
static void sig_handler(int s) { (void)s; atomic_store_explicit(&g_quit, 1, memory_order_relaxed); close(STDIN_FILENO); }

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
/* I2S 경로: SlotRing 슬롯을 직접 가리킴 (zero-copy).
 * RAVENNA/RTP 경로: 아래 정적 버퍼를 가리킴. */
float *g_in_ptr[MAX_CH];
float *g_out_ptr[MAX_CH];

/* RAVENNA/RTP 폴백 정적 버퍼 (SRC 경로, I2S는 사용 안 함) */
static float g_in_buf_static [MAX_CH][MAX_PERIOD_FRAMES];
static float g_out_buf_static[MAX_CH][MAX_PERIOD_FRAMES];

/* RTP 출력 인터리빙 임시 버퍼 — dsp_write_outputs 전용 (DSP 스레드만 접근) */
static float g_interleave_tmp[MAX_PERIOD_FRAMES * MAX_CH];

static inline void atomic_max_float(_Atomic float *target, float val)
{
    float old = atomic_load_explicit(target, memory_order_relaxed);
    while (val > old &&
           !atomic_compare_exchange_weak_explicit(target, &old, val,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed));
}

static void process_channel_dsp(Channel *chs, float * const *bufs,
                                _Atomic float *levels,
                                int ch_start, int ch_count, int n_ch)
{
    for (int ch = ch_start; ch < ch_start + ch_count && ch < n_ch; ch++) {
        Channel *c   = &chs[ch];
        float   *buf = bufs[ch];
        if (g_bypass_all_dsp || c->bypass_dsp) {
            float peak = level_peak_neon(buf, g_period_frames);
            atomic_max_float(&levels[ch], peak);
            continue;
        }
        if (c->muted) {
            memset(buf, 0, (size_t)g_period_frames * sizeof(float));
            c->gain_cur = c->gain_tgt;
            atomic_store_explicit(&levels[ch], 0.0f, memory_order_relaxed);
            continue;
        }
        gain_ramp_neon(buf, c->gain_cur, c->gain_tgt, g_period_frames);
        c->gain_cur = c->gain_tgt;
        float peak = level_peak_neon(buf, g_period_frames);
        atomic_max_float(&levels[ch], peak);
    }
}

static void process_routing(int out_start, int out_count, int n_in, int n_out)
{
    for (int out = out_start; out < out_start + out_count && out < n_out; out++) {
        memset(g_out_ptr[out], 0, (size_t)g_period_frames * sizeof(float));
        for (int in = 0; in < n_in; in++) {
            float gain = g_route[out][in];
            if (gain == 0.0f) continue;
            route_add_neon(g_out_ptr[out], g_in_ptr[in], gain, g_period_frames);
        }
    }
}


static void *dsp_worker_thread(void *arg)
{
    WorkerArg *w = (WorkerArg *)arg;

    struct sched_param sp = { .sched_priority = g_prio_dsp };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    pin_to_cpu(2);

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
        process_channel_dsp(g_in_ch,  g_in_ptr,  g_in_level,  w->ch_start, w->ch_count, w->n_in);
        pthread_barrier_wait(&g_barrier_input_done);
        process_routing(w->ch_start, w->ch_count, w->n_in, w->n_out);
        pthread_barrier_wait(&g_barrier_routing_done);
        process_channel_dsp(g_out_ch, g_out_ptr, g_out_level, w->ch_start, w->ch_count, w->n_out);
        pthread_barrier_wait(&g_barrier_work_done);
    }
    return NULL;
}

/* ── DSP 틱 대기 ─────────────────────────────────────────────────── */
/* hw:aoip(I2S) eventfd 신호 대기. 반환값: 0=종료, 1=계속 */
static int dsp_wait_tick(long long period_ns)
{
    struct pollfd _pfd = { .fd = g_dsp_clock_fd, .events = POLLIN };
    int timeout_ms = (int)(period_ns * 3 / 1000000LL);
    if (timeout_ms < 4) timeout_ms = 4;
    int pr = poll(&_pfd, 1, timeout_ms);
    if (atomic_load_explicit(&g_quit, memory_order_relaxed)) return 0;
    if (pr < 0) { if (errno != EINTR) return 0; return 1; }
    if (_pfd.revents & POLLIN) {
        uint64_t _val;
        (void)read(g_dsp_clock_fd, &_val, sizeof(_val));
    }
    return 1;
}

/* ── ALSA 입력 읽기 ──────────────────────────────────────────────── */
/* ── ALSA 장치 단일 읽기 (클럭 마스터 직접 읽기 / SRC 경로) ─────── */
/* I2S 클럭 마스터: SlotRing 포인터 직접 획득 (zero-copy) */
static void read_alsa_master(Device *d)
{
    float *rptrs[MAX_CH];
    if (slot_ring_acquire_read(&d->i2s_in_ring, rptrs)) {
        d->i2s_in_acquired = 1;
        for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
            g_in_ptr[d->ch_start+c] = rptrs[c];
        if (d->cap_underrun > 0) {
            fprintf(stderr, "[aoip_engine] alsa '%s': capture recovered after %u ticks\n",
                    d->name, d->cap_underrun);
            d->cap_underrun = 0;
        }
    } else {
        d->i2s_in_acquired = 0;
        d->cap_underrun++;
        if (d->cap_underrun == 10 || (d->cap_underrun > 10 && d->cap_underrun % 500 == 0))
            fprintf(stderr, "[aoip_engine] alsa '%s': capture underrun %u ticks (avail=%d)\n",
                    d->name, d->cap_underrun, slot_ring_avail(&d->i2s_in_ring));
        for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++) {
            g_in_ptr[d->ch_start+c] = g_in_buf_static[d->ch_start+c];
            memset(g_in_ptr[d->ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
        }
    }
}

/* ALSA 장치 단일 읽기 dispatcher */
static void read_alsa_device(Device *d)
{
    if (d->is_ravenna) {
        for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
            g_in_ptr[d->ch_start+c] = g_in_buf_static[d->ch_start+c];
        if (!atomic_load_explicit(&d->ravenna_ptp_locked, memory_order_acquire)) {
            for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                memset(g_in_ptr[d->ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
            d->cap_prebuf_ready = 0;
            return;
        }
        /* PTP 잠금 직후 in_ring이 비어있음 — RAVENNA_FILL_TARGET/2 충전까지 zeros 출력.
         * 충전 완료 시점에 SRC/PI 리셋하여 cold-start 아티팩트 없이 시작. */
        if (!d->cap_prebuf_ready) {
            if (rb_avail(&d->in_ring) < RAVENNA_FILL_TARGET / 2) {
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    memset(g_in_ptr[d->ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
                return;
            }
            src_reset(d->cap_src);
            pi_reset(&d->cap_pi);
            d->cap_prebuf_ready = 1;
            fprintf(stderr, "[aoip_engine] ravenna '%s': cap prebuffer done (fill=%d), SRC ready\n",
                    d->name, rb_avail(&d->in_ring));
        }
        ring_capture_src(d->cap_src, &d->cap_pi, &d->in_ring,
                         d->tmp_cap_in, d->tmp_cap_out,
                         RAVENNA_FILL_TARGET, d->channels, d->ch_start);
    } else {
        read_alsa_master(d);
    }
}

/* ── 전체 입력 읽기 + I2S 출력 슬롯 미리 획득 ───────────────────── */
static void dsp_read_inputs(void)
{
    /* 먼저 전체 채널 포인터를 정적 버퍼로 초기화 (장치가 커버하지 않는 채널 폴백) */
    for (int ch = 0; ch < g_n_in; ch++)
        g_in_ptr[ch] = g_in_buf_static[ch];
    for (int ch = 0; ch < g_n_out; ch++)
        g_out_ptr[ch] = g_out_buf_static[ch];

    for (int di = 0; di < g_n_dev; di++) {
        Device *d = &g_dev[di];
        if (!d->enabled) continue;
        /* I2S 출력 슬롯 미리 획득: DSP가 직접 슬롯에 쓰기 위해 필요 */
        if (d->is_i2s && d->mode != 1) {
            float *wptrs[MAX_CH];
            if (slot_ring_acquire_write(&d->i2s_out_ring, wptrs)) {
                for (int c = 0; c < d->channels && (d->ch_start+c) < MAX_CH; c++)
                    g_out_ptr[d->ch_start+c] = wptrs[c];
                d->i2s_out_acquired = 1;
                if (d->play_overflow > 0) {
                    fprintf(stderr, "[aoip_engine] alsa '%s': playback recovered after %u ticks\n",
                            d->name, d->play_overflow);
                    d->play_overflow = 0;
                }
            } else {
                d->i2s_out_acquired = 0;
                d->play_overflow++;
                if (d->play_overflow == 10 || (d->play_overflow > 10 && d->play_overflow % 500 == 0))
                    fprintf(stderr, "[aoip_engine] alsa '%s': playback overflow %u ticks (free=%d)\n",
                            d->name, d->play_overflow, slot_ring_free_slots(&d->i2s_out_ring));
            }
        }
        if (d->mode != 2) read_alsa_device(d);
    }
    for (int ri = 0; ri < g_n_rtp_in; ri++) {
        RtpStream *r = &g_rtp_in[ri];
        for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
            g_in_ptr[r->ch_start+c] = g_in_buf_static[r->ch_start+c];
        if (!r->enabled || !r->ring.buf || !r->rtp_src) {
            for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                memset(g_in_ptr[r->ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
            continue;
        }
        /* 프리버퍼링: 설정 버퍼의 절반 이상 쌓일 때까지 zeros 출력 */
        if (r->prebuffering) {
            if (rb_avail(&r->ring) >= r->fill_target / 2) {
                r->prebuffering = 0;
                fprintf(stderr, "[aoip_engine] rtp_in '%s': prebuffer done (fill=%d, target=%d)\n",
                        r->name, rb_avail(&r->ring), r->fill_target);
            } else {
                for (int c = 0; c < r->channels && (r->ch_start+c) < MAX_CH; c++)
                    memset(g_in_ptr[r->ch_start+c], 0, (size_t)g_period_frames * sizeof(float));
                continue;
            }
        }
        if (rb_avail(&r->ring) > r->fill_target * 2)
            r->overrun_total++;
        int used = ring_capture_src(r->rtp_src, &r->rtp_pi, &r->ring,
                                    r->rtp_in_buf, r->rtp_out_buf,
                                    r->fill_target, r->channels, r->ch_start);
        if (used > 0) {
            r->rtp_underrun = 0;
        } else {
            r->rtp_underrun++;
            r->underrun_total++;
            if (r->rtp_underrun == 10 || (r->rtp_underrun > 10 && r->rtp_underrun % 500 == 0))
                fprintf(stderr, "[aoip_engine] rtp_in '%s': underrun %d ticks (fill=%d), re-prebuffering\n",
                        r->name, r->rtp_underrun, rb_avail(&r->ring));
            /* 10틱 지속 언더런 시 SRC/PI 리셋 후 재충전 대기 */
            if (r->rtp_underrun == 10) {
                src_reset(r->rtp_src);
                pi_reset(&r->rtp_pi);
                r->prebuffering = 1;
            }
        }
    }
}

/* ── 병렬 DSP 실행 ───────────────────────────────────────────────── */
static void dsp_run_parallel(void)
{
    for (int w = 0; w < DSP_WORKER_COUNT; w++) {
        g_worker_arg[w].n_in  = g_n_in;
        g_worker_arg[w].n_out = g_n_out;
    }
    pthread_barrier_wait(&g_barrier_work_start);
    process_channel_dsp(g_in_ch,  g_in_ptr,  g_in_level,  0, DSP_WORKER_CH, g_n_in);
    pthread_barrier_wait(&g_barrier_input_done);
    process_routing(0, DSP_WORKER_CH, g_n_in, g_n_out);
    pthread_barrier_wait(&g_barrier_routing_done);
    process_channel_dsp(g_out_ch, g_out_ptr, g_out_level, 0, DSP_WORKER_CH, g_n_out);
    pthread_barrier_wait(&g_barrier_work_done);
}

/* ── 출력 쓰기 (ALSA + RTP) ─────────────────────────────────────── */
static void interleave_buf(int ch_start, int channels, float *tmp)
{
    if (channels == 2 && ch_start + 1 < MAX_CH) {
        interleave_2ch_neon(g_out_ptr[ch_start], g_out_ptr[ch_start + 1], tmp, g_period_frames);
        return;
    }
    memset(tmp, 0, (size_t)(g_period_frames * channels) * sizeof(float));
    for (int f = 0; f < g_period_frames; f++)
        for (int c = 0; c < channels && (ch_start+c) < MAX_CH; c++)
            tmp[f * channels + c] = g_out_ptr[ch_start+c][f];
}

static void dsp_write_outputs(void)
{
    for (int di = 0; di < g_n_dev; di++) {
        Device *d = &g_dev[di];
        if (!d->enabled) continue;

        if (d->is_i2s) {
            /* zero-copy: 입력 슬롯 release.
             * 출력 슬롯은 dsp_read_inputs()에서 acquire_write()가 성공한 경우만 commit.
             * g_out_ptr[ch]가 슬롯을 가리키면 commit, 정적 버퍼면 skip. */
            if (d->mode != 2 && d->i2s_in_acquired)
                slot_ring_consume_read(&d->i2s_in_ring);
            if (d->mode != 1 && d->i2s_out_acquired)
                slot_ring_commit_write(&d->i2s_out_ring);
            continue;
        }

        if (d->mode == 1) continue;  /* capture-only: 출력 없음 */

        alsa_playback_src(d);
    }

    for (int ri = 0; ri < g_n_rtp_out; ri++) {
        RtpStream *r = &g_rtp_out[ri];
        if (!r->enabled || !r->ring.buf) continue;
        interleave_buf(r->ch_start, r->channels, g_interleave_tmp);
        if (rb_write(&r->ring, g_interleave_tmp, g_period_frames) < g_period_frames)
            r->overrun_total++;
    }
}

/* ── DSP 스레드 마스터 루프 ──────────────────────────────────────── */
static void *dsp_thread(void *arg)
{
    (void)arg;

    struct sched_param sp = { .sched_priority = g_prio_dsp };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    pin_to_cpu(2);

    int64_t clk2_pa_fr = 0, clk2_pa_hts = 0;
    int64_t clk2_pr_fr = 0, clk2_pr_hts = 0;
    struct timespec _now;
    clock_gettime(CLOCK_MONOTONIC, &_now);
    int64_t clk2_next_ns = (int64_t)_now.tv_sec * 1000000000LL + _now.tv_nsec
                           + 1LL * 1000000000LL;  /* 초기 1s 빠른 측정 시작 */

    int cached_period_frames = g_period_frames;
    long long period_ns = (long long)cached_period_frames * 1000000000LL / SAMPLE_RATE;

    while (!atomic_load_explicit(&g_quit, memory_order_relaxed)) {
        if (g_period_frames != cached_period_frames) {
            cached_period_frames = g_period_frames;
            period_ns = (long long)cached_period_frames * 1000000000LL / SAMPLE_RATE;
        }

        if (!dsp_wait_tick(period_ns)) break;

        clk2_report(&clk2_pa_fr, &clk2_pa_hts, &clk2_pr_fr, &clk2_pr_hts, &clk2_next_ns);

        Cmd cmd;
        while (cmd_pop(&cmd)) apply_cmd(&cmd);

        dsp_read_inputs();
        dsp_run_parallel();
        dsp_write_outputs();
    }

    return NULL;
}

/* ── 리포터 스레드 ────────────────────────────────────────────────── */
static void *reporter_thread(void *arg)
{
    (void)arg;
    int buf_tick = 0;
    while (g_reporter_running) {
        usleep(125000);
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
        /* 2초마다 rtp_in 버퍼 fill 상태 보고 */
        if (++buf_tick >= 16) {
            buf_tick = 0;
            for (int i = 0; i < g_n_rtp_in; i++) {
                RtpStream *r = &g_rtp_in[i];
                if (!r->enabled || !r->ring.buf) continue;
                int fill = rb_avail(&r->ring);
                int fill_ms  = fill * 1000 / SAMPLE_RATE;
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

/* ── ALSA 브릿지 start/stop (SRC 포함) ──────────────────────────── */
static void bridge_start(Device *d)
{
    if (!d->is_i2s) {
        int err;
        if (d->mode != 2) {
            d->cap_src   = src_new(SRC_SINC_FASTEST, d->channels, &err);
            d->clk_accum = 0;
            double ratio_hint = d->is_ravenna
                ? atomic_load_explicit(&g_ravenna_ratio_hint, memory_order_relaxed)
                : 1.0;
            d->cap_pi  = (PiState){ .ratio=ratio_hint, .kp=RATIO_KP, .ki=RATIO_KI, .min=RATIO_MIN, .max=RATIO_MAX };
            if (d->is_ravenna) {
                /* 시작 시 ptp_locked=0 → 캡처 스레드가 prebuffer 후 언뮤트 */
                atomic_store_explicit(&d->ravenna_ptp_locked, 0, memory_order_relaxed);
                atomic_store_explicit(&d->ravenna_flush, 0, memory_order_relaxed);
                d->ravenna_prebuf_count = 0;
                d->cap_prebuf_ready = 0;
            }
        }
        if (d->mode != 1) {
            d->play_src = src_new(SRC_SINC_FASTEST, d->channels, &err);
            d->play_pi = (PiState){ .ratio=1.0, .kp=RATIO_KP, .ki=RATIO_KI, .min=RATIO_MIN, .max=RATIO_MAX };
        }
    }
    device_start(d);
}

static void bridge_stop(Device *d)
{
    device_stop(d);  /* 스레드 join 후 ring buf 해제 */
    if (d->cap_src)  { src_delete(d->cap_src);  d->cap_src  = NULL; }
    if (d->play_src) { src_delete(d->play_src); d->play_src = NULL; }
}

/* ── RTP shm 헬퍼 ────────────────────────────────────────────────── */

typedef struct {
    RingBuf *ring;
    char     key[32];
    char     sock_path[256];
    int      is_send;
    RtpStream  *buf;
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

static int rtp_stream_open(RtpStream *r, int is_out)
{
    rb_init(&r->ring, RTP_RING_FRAMES, r->channels);
    if (!r->ring.buf) {
        fprintf(stderr, "[aoip_engine] rtp_%s '%s': ring alloc failed\n",
                is_out ? "out" : "in", r->name);
        return 0;
    }
    r->is_send = is_out;
    r->rtp_ctx = NULL;

    if (!is_out) {
        int err;
        r->rtp_src    = src_new(SRC_SINC_FASTEST, r->channels, &err);
        r->rtp_pi     = (PiState){ .ratio=1.0, .kp=RTP_RATIO_KP, .ki=RTP_RATIO_KI, .min=RTP_RATIO_MIN, .max=RTP_RATIO_MAX };
        r->rtp_underrun  = 0;
        r->prebuffering  = 1;
    }

    fprintf(stderr, "[aoip_engine] rtp_%s '%s' ch=%d ch_start=%d\n",
            is_out ? "out" : "in", r->name, r->channels, r->ch_start);

    RtpLaunchArg *la = malloc(sizeof(RtpLaunchArg));
    if (!la) {
        fprintf(stderr, "[aoip_engine] rtp_%s '%s': malloc failed\n",
                is_out ? "out" : "in", r->name);
        free(r->ring.buf); r->ring.buf = NULL;
        return 0;
    }
    la->ring    = &r->ring;
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

    return 1;
}

static void rtp_stream_close(RtpStream *r)
{
    if (r->rtp_ctx) {
        if (r->is_send)
            rtp_send_stop((RtpSendCtx *)r->rtp_ctx);
        else
            rtp_recv_stop((RtpRecvCtx *)r->rtp_ctx);
        r->rtp_ctx = NULL;
    }
    if (r->rtp_src) { src_delete(r->rtp_src); r->rtp_src = NULL; }
    free(r->ring.buf); r->ring.buf = NULL;
    r->enabled = 0;
}

/* ── 명령 핸들러 ─────────────────────────────────────────────────── */

static void cmd_set(int n, char **tok)
{
    if (n < 3 || strcmp(tok[1], "period")) return;
    int pf = atoi(tok[2]);
    if (pf >= 1 && pf <= MAX_PERIOD_FRAMES) {
        g_period_frames = pf;
        fprintf(stderr, "[aoip_engine] period_frames=%d\n", g_period_frames);
    } else {
        fprintf(stderr, "[aoip_engine] set period: invalid %d (1..%d)\n",
                pf, MAX_PERIOD_FRAMES);
    }
}

static void cmd_bridge(int n, char **tok)
{
    if (n < 3) return;
    const char *sub  = tok[1];
    const char *name = tok[2];

    if ((!strcmp(sub, "add") || !strcmp(sub, "add_in") || !strcmp(sub, "add_out")) && n >= 8) {
        Device *d = NULL;
        for (int i = 0; i < g_n_dev; i++)
            if (!strcmp(g_dev[i].name, name)) { d = &g_dev[i]; break; }
        if (d) {
            bridge_stop(d);
        } else {
            if (g_n_dev >= MAX_DEVICES) return;
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
        d->is_ravenna      = strstr(d->dev, "RAVENNA") ? 1 : 0;
        d->thread_priority = d->is_ravenna ? g_prio_ravenna : g_prio_alsa;
        /* analog(I2S)을 DSP 마스터 클럭으로 지정 */
        d->is_i2s = (!strcmp(name, "analog") && d->mode != 2) ? 1 : 0;
        d->clk_accum       = 0;
        bridge_start(d);
    } else if (!strcmp(sub, "start")) {
        for (int i = 0; i < g_n_dev; i++)
            if (!strcmp(g_dev[i].name, name)) { bridge_start(&g_dev[i]); break; }
    } else if (!strcmp(sub, "stop")) {
        for (int i = 0; i < g_n_dev; i++)
            if (!strcmp(g_dev[i].name, name)) { bridge_stop(&g_dev[i]); break; }
    }
}

static void cmd_route(int n, char **tok)
{
    if (n < 4) return;
    int in_ch  = atoi(tok[2]) - 1;
    int out_ch = atoi(tok[3]) - 1;
    if (in_ch < 0 || in_ch >= MAX_CH || out_ch < 0 || out_ch >= MAX_CH) return;
    Cmd cmd = { .type = CMD_ROUTE_SET };
    cmd.route.in_ch  = in_ch;
    cmd.route.out_ch = out_ch;
    cmd.route.level  = (!strcmp(tok[1], "remove")) ? 0.0f :
                       (n >= 5 ? (float)atof(tok[4]) : 1.0f);
    cmd_push(&cmd);
    printf("route:updated\n");
    fflush(stdout);
}

static void cmd_rtp_in(int n, char **tok)
{
    if (n < 3) return;
    const char *sub  = tok[1];
    const char *name = tok[2];
    if (!strcmp(sub, "add") && n >= 3) {
        int slot = -1;
        for (int i = 0; i < g_n_rtp_in; i++)
            if (!g_rtp_in[i].enabled) { slot = i; break; }
        if (slot < 0) {
            if (g_n_rtp_in >= MAX_RTP) return;
            slot = g_n_rtp_in;
        }
        RtpStream *r = &g_rtp_in[slot];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%s", name);
        r->channels = n >= 4 ? atoi(tok[3]) : 2;
        r->ch_start = n >= 5 ? atoi(tok[4]) : slot * 2;
        int buf_ms  = RTP_FILL_TARGET * 1000 / SAMPLE_RATE; /* 기본값 */
        for (int i = 5; i < n; i++) {
            int v; if (sscanf(tok[i], "bufMs=%d", &v) == 1) { buf_ms = v; break; }
        }
        r->fill_target = buf_ms * SAMPLE_RATE / 1000;
        if (rtp_stream_open(r, 0)) {
            r->enabled = 1;
            if (slot == g_n_rtp_in) g_n_rtp_in++;
        }
    } else if (!strcmp(sub, "remove")) {
        for (int i = 0; i < g_n_rtp_in; i++)
            if (!strcmp(g_rtp_in[i].name, name) && g_rtp_in[i].enabled) {
                rtp_stream_close(&g_rtp_in[i]); break;
            }
    }
}

static void cmd_rtp_out(int n, char **tok)
{
    if (n < 3) return;
    const char *sub  = tok[1];
    const char *name = tok[2];
    if (!strcmp(sub, "add") && n >= 3) {
        int slot = -1;
        for (int i = 0; i < g_n_rtp_out; i++)
            if (!g_rtp_out[i].enabled) { slot = i; break; }
        if (slot < 0) {
            if (g_n_rtp_out >= MAX_RTP) return;
            slot = g_n_rtp_out;
        }
        RtpStream *r = &g_rtp_out[slot];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%s", name);
        r->channels = n >= 4 ? atoi(tok[3]) : 2;
        r->ch_start = n >= 5 ? atoi(tok[4]) : slot * 2;
        if (rtp_stream_open(r, 1)) {
            r->enabled = 1;
            if (slot == g_n_rtp_out) g_n_rtp_out++;
        }
    } else if (!strcmp(sub, "remove")) {
        for (int i = 0; i < g_n_rtp_out; i++)
            if (!strcmp(g_rtp_out[i].name, name) && g_rtp_out[i].enabled) {
                rtp_stream_close(&g_rtp_out[i]); break;
            }
    }
}

static void cmd_dsp(const char *verb, int n, char **tok)
{
    if (n < 3) return;
    int dir = strcmp(tok[1], "in") ? 1 : 0;
    int ch  = atoi(tok[2]) - 1;
    if (ch < 0 || (dir == 0 && ch >= g_n_in) || (dir == 1 && ch >= g_n_out)) return;
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
        if      (!strcmp(verb, "set"))     cmd_set(n, tok);
        else if (!strcmp(verb, "bridge"))  cmd_bridge(n, tok);
        else if (!strcmp(verb, "route"))   cmd_route(n, tok);
        else if (!strcmp(verb, "rtp_in"))  cmd_rtp_in(n, tok);
        else if (!strcmp(verb, "rtp_out")) cmd_rtp_out(n, tok);
        else if (!strcmp(verb, "clk2"))  { g_clk2_report = n>=2 ? atoi(tok[1]) : 1;
                                           fprintf(stderr, "[aoip_engine] clk2 %s\n", g_clk2_report ? "on":"off"); }
        else if (!strcmp(verb, "lvl"))   { g_lvl_report  = n>=2 ? atoi(tok[1]) : 1;
                                           fprintf(stderr, "[aoip_engine] lvl %s\n",  g_lvl_report  ? "on":"off"); }
        else                               cmd_dsp(verb, n, tok);
    }
}


/* ── audio.json 간단 파서 ────────────────────────────────────────── */
static int json_bool(const char *json, const char *key, int def)
{
    char pat[64];
    snprintf(pat, sizeof(pat), "\"%s\"", key);
    const char *p = strstr(json, pat);
    if (!p) return def;
    p = strchr(p + strlen(pat), ':');
    if (!p) return def;
    while (*p == ':' || *p == ' ' || *p == '\t') p++;
    if (strncmp(p, "true",  4) == 0) return 1;
    if (strncmp(p, "false", 5) == 0) return 0;
    return def;
}

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
    char buf[8192] = "";
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
    if ((v = json_int(section, "periodFrames", 0)) > 0 && v <= MAX_PERIOD_FRAMES)
        g_period_frames = v;
    g_lvl_report = json_bool(section, "lvlReport", 1);
    g_clk2_report = g_lvl_report;

    fprintf(stderr, "[aoip_engine] config: dsp=%d alsa=%d ravenna=%d rtp=%d period=%d lvl=%d clk2=%d\n",
            g_prio_dsp, g_prio_alsa, g_prio_ravenna, g_prio_rtp, g_period_frames, g_lvl_report, g_clk2_report);
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
    int sfd = rtp_unix_connect("/run/aoip/engine.sock", 30, 100);
    if (sfd < 0) {
        fprintf(stderr, "[aoip_engine] cannot connect to /run/aoip/engine.sock\n");
        return 1;
    }

    char init_line[128] = "";
    rtp_read_line(sfd, init_line, sizeof(init_line));
    g_n_in  = rtp_cfg_int(init_line, "n_in",  0);
    g_n_out = rtp_cfg_int(init_line, "n_out", 0);

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
        g_rtp_in[i].ring.buf  = NULL;
        g_rtp_out[i].ring.buf = NULL;
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

    atomic_store_explicit(&g_quit, 1, memory_order_relaxed);
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
        if (g_dev[i].enabled) bridge_stop(&g_dev[i]);

    for (int i = 0; i < g_n_rtp_in; i++)
        if (g_rtp_in[i].enabled) rtp_stream_close(&g_rtp_in[i]);
    for (int i = 0; i < g_n_rtp_out; i++)
        if (g_rtp_out[i].enabled) rtp_stream_close(&g_rtp_out[i]);

    if (g_dsp_clock_fd >= 0) { close(g_dsp_clock_fd); g_dsp_clock_fd = -1; }

    return 0;
}
