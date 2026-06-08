/*
 * aoip_engine.c — 통합 AoIP 오디오 매트릭스 엔진 (핵심 루프)
 *
 * 스레드:
 *   [P85] dsp_thread          — I2S eventfd 기반 마스터 오디오 루프
 *   [P80] alsa_capture/playback × N  (alsa_device.c)
 *   [   ] reporter_thread     — ~12Hz stdout 레벨/GR 미터
 *   [   ] stdin cmd_loop      — 명령 파서 (main 스레드)
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
#include "include/engine_globals.h"
#include "include/shm_ring.h"
#include "include/ring_buf.h"
#include "include/alsa_device.h"
#include "include/dsp_neon.h"
#include "include/dsp_channel.h"
#include "include/cmd_handler.h"
#include "include/dsp_worker.h"
#include "include/dsp_io.h"
#include "include/dsp_reporter.h"
#include "include/bridge_manager.h"
#include "include/config_loader.h"
#include "include/rtp_recv.h"
#include "include/rtp_send.h"
#include "include/clk2.h"
#include "include/rtp_utils.h"
#include "include/rtp_stream.h"
#include "include/dsp_src.h"

/* ── 전역 상태 정의 ─────────────────────────────────────────────── */
int           g_dsp_clock_fd       = -1;
int           g_period_frames      = DEFAULT_PERIOD_FRAMES;
int           g_ravenna_fill_target = RAVENNA_FILL_TARGET;
int           g_prio_dsp           = 59;
int           g_prio_alsa          = 58;
int           g_prio_ravenna       = 57;
int           g_prio_rtp           = 56;
_Atomic int   g_quit               = 0;
int           g_bypass_all_dsp     = 0;
float         g_sr                 = (float)SAMPLE_RATE;

Device        g_dev[MAX_DEVICES];
int           g_n_dev              = 0;

RtpStream     g_rtp_in[MAX_RTP];
int           g_n_rtp_in           = 0;
RtpStream     g_rtp_out[MAX_RTP];
int           g_n_rtp_out          = 0;

InChDspState  g_in_ch[MAX_CH];
OutChDspState g_out_ch[MAX_CH];
int           g_n_in               = 8;
int           g_n_out              = 8;

float         g_route[MAX_CH][MAX_CH];

_Atomic float g_in_level[MAX_CH];
_Atomic float g_out_level[MAX_CH];

_Atomic float g_in_gate_in_level[MAX_CH];
_Atomic float g_out_gate_in_level[MAX_CH];

_Atomic int   g_in_gate_phase[MAX_CH];
_Atomic int   g_out_gate_phase[MAX_CH];

_Atomic float g_in_gr_gate[MAX_CH];
_Atomic float g_in_gr_comp[MAX_CH];
_Atomic float g_out_gr_gate[MAX_CH];
_Atomic float g_out_gr_comp[MAX_CH];
_Atomic float g_out_gr_lim[MAX_CH];
volatile int  g_gr_report          = 0;

volatile int  g_reporter_running   = 0;
volatile int  g_lvl_report         = 1;

float        *g_in_ptr[MAX_CH];
float        *g_out_ptr[MAX_CH];
float         g_in_buf_static [MAX_CH][MAX_PERIOD_FRAMES];
float         g_out_buf_static[MAX_CH][MAX_PERIOD_FRAMES];
float         g_interleave_tmp[MAX_PERIOD_FRAMES * MAX_CH];

/* ── 시그널 핸들러 ───────────────────────────────────────────────── */
static void sig_handler(int s) {
    (void)s;
    atomic_store_explicit(&g_quit, 1, memory_order_relaxed);
    close(STDIN_FILENO);
}

/* ── atomic max ──────────────────────────────────────────────────── */
static inline void atomic_max_float(_Atomic float *target, float val)
{
    float old = atomic_load_explicit(target, memory_order_relaxed);
    while (val > old &&
           !atomic_compare_exchange_weak_explicit(target, &old, val,
                                                  memory_order_relaxed,
                                                  memory_order_relaxed));
}

/* ── 명령 적용 (DSP 스레드) ──────────────────────────────────────── */
static void apply_cmd(const Cmd *cmd)
{
    InChDspState  *in_ch  = (cmd->ch >= 0 && cmd->ch < MAX_CH) ? &g_in_ch[cmd->ch]  : NULL;
    OutChDspState *out_ch = (cmd->ch >= 0 && cmd->ch < MAX_CH) ? &g_out_ch[cmd->ch] : NULL;

    switch (cmd->type) {
    case CMD_GAIN:
        if (cmd->dir == 0 && in_ch)  in_ch->gain_tgt  = cmd->gain;
        if (cmd->dir == 1 && out_ch) out_ch->gain_tgt = cmd->gain;
        break;
    case CMD_MUTE:
        if (cmd->dir == 0 && in_ch) {
            in_ch->muted = cmd->flag;
            if (cmd->flag) in_ch->gain_cur = in_ch->gain_tgt;
        }
        if (cmd->dir == 1 && out_ch) {
            out_ch->muted = cmd->flag;
            if (cmd->flag) out_ch->gain_cur = out_ch->gain_tgt;
        }
        break;
    case CMD_BYPASS:
        if (cmd->dir == 0 && in_ch)  in_ch->bypass_dsp  = cmd->flag;
        if (cmd->dir == 1 && out_ch) out_ch->bypass_dsp = cmd->flag;
        break;
    case CMD_ROUTE_SET:
        if (cmd->route.out_ch < MAX_CH && cmd->route.in_ch < MAX_CH)
            g_route[cmd->route.out_ch][cmd->route.in_ch] = cmd->route.level;
        break;
    case CMD_TRIM:
        if (cmd->dir == 0 && in_ch) in_ch_apply_trim(in_ch, cmd->trim_db);
        break;
    case CMD_HPF:
        if (cmd->dir == 0 && in_ch) in_ch_apply_hpf(in_ch, &cmd->hpf);
        break;
    case CMD_EQ_BAND:
        if (cmd->dir == 0 && in_ch)  in_ch_apply_eq(in_ch, &cmd->eq_band);
        if (cmd->dir == 1 && out_ch) out_ch_apply_eq(out_ch, &cmd->eq_band);
        break;
    case CMD_GATE:
        if (cmd->dir == 0 && in_ch)  in_ch_apply_gate(in_ch, &cmd->gate);
        if (cmd->dir == 1 && out_ch) out_ch_apply_gate(out_ch, &cmd->gate);
        break;
    case CMD_COMP:
        if (cmd->dir == 0 && in_ch)  in_ch_apply_comp(in_ch, &cmd->comp);
        if (cmd->dir == 1 && out_ch) out_ch_apply_comp(out_ch, &cmd->comp);
        break;
    case CMD_LIM:
        if (cmd->dir == 1 && out_ch) out_ch_apply_lim(out_ch, &cmd->lim);
        break;
    case CMD_GR_ENABLE:
        g_gr_report = cmd->flag;
        break;
    }
}

/* ── DSP 처리 함수 (워커/마스터 공용, dsp_worker.c에서 extern 참조) */
void process_channel_dsp_in(int ch_start, int ch_count)
{
    for (int ch = ch_start; ch < ch_start + ch_count && ch < g_n_in; ch++) {
        InChDspState *c   = &g_in_ch[ch];
        float        *buf = g_in_ptr[ch];
        if (g_bypass_all_dsp || c->bypass_dsp) {
            float peak = level_peak_neon(buf, g_period_frames);
            atomic_max_float(&g_in_level[ch], peak);
            continue;
        }
        if (c->muted) {
            memset(buf, 0, (size_t)g_period_frames * sizeof(float));
            c->gain_cur = c->gain_tgt;
            atomic_store_explicit(&g_in_level[ch], 0.0f, memory_order_relaxed);
            continue;
        }
        gain_ramp_neon(buf, c->gain_cur, c->gain_tgt, g_period_frames);
        c->gain_cur = c->gain_tgt;
        in_ch_dsp_pre_gate(c, buf, g_period_frames);
        float pre_gate_peak = level_peak_neon(buf, g_period_frames);
        atomic_max_float(&g_in_gate_in_level[ch], pre_gate_peak);
        in_ch_dsp_gate_on(c, buf, g_period_frames);
        float peak = level_peak_neon(buf, g_period_frames);
        atomic_max_float(&g_in_level[ch], peak);
        /* GR 원자 업데이트 */
        atomic_store_explicit(&g_in_gr_gate[ch], c->gate.gr_cur, memory_order_relaxed);
        atomic_store_explicit(&g_in_gr_comp[ch], c->comp.gr_cur, memory_order_relaxed);
        atomic_store_explicit(&g_in_gate_phase[ch], (int)c->gate.phase, memory_order_relaxed);
    }
}

void process_channel_dsp_out(int ch_start, int ch_count)
{
    for (int ch = ch_start; ch < ch_start + ch_count && ch < g_n_out; ch++) {
        OutChDspState *c   = &g_out_ch[ch];
        float         *buf = g_out_ptr[ch];
        if (g_bypass_all_dsp || c->bypass_dsp) {
            float peak = level_peak_neon(buf, g_period_frames);
            atomic_max_float(&g_out_level[ch], peak);
            continue;
        }
        if (c->muted) {
            memset(buf, 0, (size_t)g_period_frames * sizeof(float));
            c->gain_cur = c->gain_tgt;
            atomic_store_explicit(&g_out_level[ch], 0.0f, memory_order_relaxed);
            continue;
        }
        gain_ramp_neon(buf, c->gain_cur, c->gain_tgt, g_period_frames);
        c->gain_cur = c->gain_tgt;
        /* 출력 체인은 게이트가 첫 단계 — out_ch_dsp 직전에 게이트 입력 피크 측정 */
        float pre_gate_peak = level_peak_neon(buf, g_period_frames);
        atomic_max_float(&g_out_gate_in_level[ch], pre_gate_peak);
        out_ch_dsp(c, buf, g_period_frames);
        float peak = level_peak_neon(buf, g_period_frames);
        atomic_max_float(&g_out_level[ch], peak);
        atomic_store_explicit(&g_out_gr_gate[ch], c->gate.gr_cur, memory_order_relaxed);
        atomic_store_explicit(&g_out_gr_comp[ch], c->comp.gr_cur, memory_order_relaxed);
        atomic_store_explicit(&g_out_gr_lim[ch],  c->lim.gr_cur,  memory_order_relaxed);
        atomic_store_explicit(&g_out_gate_phase[ch], (int)c->gate.phase, memory_order_relaxed);
    }
}

void process_routing(int out_start, int out_count, int n_in, int n_out)
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

/* ── DSP 마스터 루프 ─────────────────────────────────────────────── */
static void *dsp_thread(void *arg)
{
    (void)arg;
    struct sched_param sp = { .sched_priority = g_prio_dsp };
    pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    pin_to_cpu(2);

    int64_t clk2_pa_fr = 0, clk2_pa_hts = 0;
    int64_t clk2_pr_fr = 0, clk2_pr_hts = 0;
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    int64_t clk2_next_ns = (int64_t)now.tv_sec * 1000000000LL + now.tv_nsec
                           + 1LL * 1000000000LL;

    int cached_period = g_period_frames;
    long long period_ns = (long long)cached_period * 1000000000LL / SAMPLE_RATE;

    while (!atomic_load_explicit(&g_quit, memory_order_relaxed)) {
        if (g_period_frames != cached_period) {
            cached_period = g_period_frames;
            period_ns = (long long)cached_period * 1000000000LL / SAMPLE_RATE;
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

/* ── main ────────────────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    signal(SIGPIPE, SIG_IGN);
    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "[aoip_engine] mlockall failed: %s\n", strerror(errno));

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

    dup2(sfd, STDIN_FILENO);
    dup2(sfd, STDOUT_FILENO);
    close(sfd);

    /* 채널 초기 상태: gain=1, trim=1(0dB), DSP disabled */
    for (int i = 0; i < g_n_in; i++) {
        g_in_ch[i].gain_tgt = g_in_ch[i].gain_cur = 1.0f;
        g_in_ch[i].trim_lin = 1.0f;
    }
    for (int i = 0; i < g_n_out; i++)
        g_out_ch[i].gain_tgt = g_out_ch[i].gain_cur = 1.0f;

    memset(g_route, 0, sizeof(g_route));

    for (int i = 0; i < MAX_RTP; i++) {
        g_rtp_in[i].ring.buf  = NULL;
        g_rtp_out[i].ring.buf = NULL;
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    g_dsp_clock_fd = eventfd(0, EFD_SEMAPHORE);
    if (g_dsp_clock_fd < 0)
        fprintf(stderr, "[aoip_engine] eventfd: %s — dsp clock master disabled\n", strerror(errno));
    else
        fprintf(stderr, "[aoip_engine] dsp clock eventfd=%d ready\n", g_dsp_clock_fd);

    /* 병렬 DSP 워커 초기화 */
    pthread_barrier_init(&g_barrier_work_start,   NULL, DSP_WORKER_COUNT + 1);
    pthread_barrier_init(&g_barrier_input_done,   NULL, DSP_WORKER_COUNT + 1);
    pthread_barrier_init(&g_barrier_routing_done, NULL, DSP_WORKER_COUNT + 1);
    pthread_barrier_init(&g_barrier_work_done,    NULL, DSP_WORKER_COUNT + 1);
    for (int w = 0; w < DSP_WORKER_COUNT; w++) {
        g_worker_arg[w].id       = w;
        g_worker_arg[w].ch_start = (w + 1) * DSP_WORKER_CH;
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
