/*
 * audio_out.c — JACK input ports → ALSA playback (adaptive drift correction)
 *
 * JACK process_cb (SRC_SINC_FASTEST 드리프트 보정) → ring buffer → ALSA 재생 스레드
 *
 * 수정 사항:
 *   1. jack_port_get_buffer() 루프 내 반복 호출 제거 (성능 버그)
 *   2. P 제어 → PI 제어 + anti-windup (audio_in과 동일 구조)
 *   3. RATIO_MIN/MAX ±10ppm → ±100ppm 확장
 *   4. TARGET_FILL_FRAMES 명시적 지정 (ring_frames/2 고정 제거)
 *   5. ring_reset() 에 PI 상태 초기화 포함
 *
 * Usage: audio_out <client_name> <alsa_device> <rate> <period> <nperiods> <channels> [ring_frames]
 *
 * Stdout:
 *   [audio_out] ready
 */

#define _GNU_SOURCE
#include <jack/jack.h>
#include <alsa/asoundlib.h>
#include <samplerate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <math.h>

#define MAX_CH              8
#define TARGET_FILL_FRAMES  512         /* PI 제어 목표 fill: ~10 ms at 48 kHz */
#define RATIO_KP            0.00005     /* proportional gain */
#define RATIO_KI            0.000000005 /* integral gain */
#define RATIO_MIN           0.99990     /* ±100 ppm */
#define RATIO_MAX           1.00010

/* ── lock-free ring buffer (single producer / single consumer) ── */
static float       *g_ring        = NULL;
static int          g_ring_frames = 8192;
static atomic_uint  g_wp          = 0;
static atomic_uint  g_rp          = 0;

static int ring_avail(void)
{
    unsigned wp = atomic_load_explicit(&g_wp, memory_order_acquire);
    unsigned rp = atomic_load_explicit(&g_rp, memory_order_relaxed);
    return (int)(wp - rp);
}

static int ring_free(void)
{
    unsigned wp = atomic_load_explicit(&g_wp, memory_order_relaxed);
    unsigned rp = atomic_load_explicit(&g_rp, memory_order_acquire);
    return (int)((unsigned)g_ring_frames - (wp - rp));
}

static void ring_write_n(const float *src, int n, int ch)
{
    unsigned wp = atomic_load_explicit(&g_wp, memory_order_relaxed);
    for (int i = 0; i < n; i++) {
        unsigned idx = (wp + (unsigned)i) % (unsigned)g_ring_frames;
        memcpy(&g_ring[idx * ch], &src[i * ch], (size_t)ch * sizeof(float));
    }
    atomic_store_explicit(&g_wp, wp + (unsigned)n, memory_order_release);
}

static int ring_read_n(float *dst, int n, int ch)
{
    unsigned rp = atomic_load_explicit(&g_rp, memory_order_relaxed);
    unsigned wp = atomic_load_explicit(&g_wp, memory_order_acquire);
    if ((int)(wp - rp) < n) return 0;
    for (int i = 0; i < n; i++) {
        unsigned idx = (rp + (unsigned)i) % (unsigned)g_ring_frames;
        memcpy(&dst[i * ch], &g_ring[idx * ch], (size_t)ch * sizeof(float));
    }
    atomic_store_explicit(&g_rp, rp + (unsigned)n, memory_order_release);
    return 1;
}

/* ── globals ── */
static jack_client_t *g_jack;
static jack_port_t   *g_ports[MAX_CH];
static SRC_STATE     *g_src;
static double         g_ratio       = 1.0;
static double         g_ratio_integ = 0.0;
static double         g_fill_smooth = 0.0;  /* EMA of normalized fill error */
static volatile int   g_quit        = 0;
static int            g_jack_alive  = 1;
static int            g_ch          = 2;

static float g_tmp_in [4096 * MAX_CH];
static float g_tmp_out[4096 * MAX_CH];

/* ── JACK process callback ── */
static int process_cb(jack_nframes_t nframes, void *arg)
{
    (void)arg;

    /* ── 수정 1: jack_port_get_buffer 루프 밖으로 이동 ──
     *
     * 이전 코드: 내부 루프에서 nframes 횟수만큼 반복 호출 → 성능 버그
     * 수정: 포트별 1회 호출 후 포인터 캐시
     */
    float *port_buf[MAX_CH];
    for (int c = 0; c < g_ch; c++)
        port_buf[c] = (float *)jack_port_get_buffer(g_ports[c], nframes);

    /* interleave JACK 포트 → tmp_in */
    for (jack_nframes_t f = 0; f < nframes; f++)
        for (int c = 0; c < g_ch; c++)
            g_tmp_in[f * g_ch + c] = port_buf[c][f];

    /* ── 수정 2: P 제어 → PI 제어 + anti-windup ──
     *
     * audio_out 방향: ring이 비면 ratio↑ (출력 샘플 늘려 ALSA에 더 공급)
     *                 ring이 차면 ratio↓ (출력 샘플 줄여 ring 소모 속도 낮춤)
     * audio_in과 동일한 PI 구조, 방향만 반전
     */
    int avail = ring_avail();
    double error = ((double)avail - TARGET_FILL_FRAMES) / (double)TARGET_FILL_FRAMES;

    /* EMA 필터 α=0.05 */
    g_fill_smooth += 0.05 * (error - g_fill_smooth);

    g_ratio_integ += g_fill_smooth;
    /* audio_out: ring이 차있으면 ratio 증가 (더 많이 출력) */
    g_ratio = 1.0 + g_fill_smooth * RATIO_KP + g_ratio_integ * RATIO_KI;

    /* ── anti-windup: integ를 한계값 역산으로 고정 ── */
    if (g_ratio < RATIO_MIN) {
        g_ratio       = RATIO_MIN;
        g_ratio_integ = (RATIO_MIN - 1.0 - g_fill_smooth * RATIO_KP) / RATIO_KI;
    } else if (g_ratio > RATIO_MAX) {
        g_ratio       = RATIO_MAX;
        g_ratio_integ = (RATIO_MAX - 1.0 - g_fill_smooth * RATIO_KP) / RATIO_KI;
    }

    /* SRC: nframes 입력 → nframes * ratio 출력 */
    long out_max = (long)ceil((double)nframes * g_ratio) + 4;
    if ((int)ring_free() < (int)out_max) {
        /* ring 가득 → drop (SRC state는 유지) */
        return 0;
    }

    SRC_DATA sd = {
        .data_in       = g_tmp_in,
        .data_out      = g_tmp_out,
        .input_frames  = (long)nframes,
        .output_frames = out_max,
        .src_ratio     = g_ratio,
        .end_of_input  = 0,
    };
    src_process(g_src, &sd);

    ring_write_n(g_tmp_out, (int)sd.output_frames_gen, g_ch);
    return 0;
}

static void on_jack_shutdown(void *arg) { (void)arg; g_jack_alive = 0; g_quit = 1; }
static void on_signal(int s)           { (void)s; g_quit = 1; }

/* ── ALSA 재생 스레드 ── */
static char g_dev[64]  = "hw:0";
static int  g_rate     = 48000;
static int  g_period   = 48;
static int  g_nperiods = 8;

static snd_pcm_t *alsa_open_playback(void)
{
    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, g_dev, SND_PCM_STREAM_PLAYBACK, 0) < 0) return NULL;

    snd_pcm_hw_params_t *hw;
    snd_pcm_hw_params_alloca(&hw);
    snd_pcm_hw_params_any(pcm, hw);
    snd_pcm_hw_params_set_access(pcm, hw, SND_PCM_ACCESS_RW_INTERLEAVED);
    snd_pcm_hw_params_set_format(pcm, hw, SND_PCM_FORMAT_S32_LE);
    snd_pcm_hw_params_set_channels(pcm, hw, (unsigned)g_ch);

    unsigned rate = (unsigned)g_rate;
    snd_pcm_hw_params_set_rate_near(pcm, hw, &rate, 0);

    snd_pcm_uframes_t period = (snd_pcm_uframes_t)g_period;
    snd_pcm_hw_params_set_period_size_near(pcm, hw, &period, 0);

    snd_pcm_uframes_t bufsize = period * (snd_pcm_uframes_t)g_nperiods;
    snd_pcm_hw_params_set_buffer_size_near(pcm, hw, &bufsize);

    if (snd_pcm_hw_params(pcm, hw) < 0) {
        snd_pcm_close(pcm);
        return NULL;
    }
    snd_pcm_prepare(pcm);
    return pcm;
}

static void ring_reset(void)
{
    unsigned wp = atomic_load_explicit(&g_wp, memory_order_relaxed);
    atomic_store_explicit(&g_rp, wp, memory_order_release);
    /* ── 수정 3: PI 상태 초기화 ── */
    g_ratio       = 1.0;
    g_ratio_integ = 0.0;
    g_fill_smooth = 0.0;
}

/* 프리버퍼 대기 헬퍼 */
static void wait_prebuf(void)
{
    while (!g_quit && ring_avail() < TARGET_FILL_FRAMES)
        usleep(1000);
}

static void *alsa_thread(void *arg)
{
    (void)arg;
    snd_pcm_t *pcm = alsa_open_playback();
    if (!pcm) {
        fprintf(stderr, "[audio_out] cannot open %s\n", g_dev);
        g_quit = 1;
        return NULL;
    }

    float   *fbuf = malloc((size_t)(g_period * g_ch) * sizeof(float));
    int32_t *ibuf = malloc((size_t)(g_period * g_ch) * sizeof(int32_t));
    if (!fbuf || !ibuf) {
        fprintf(stderr, "[audio_out] buffer alloc failed\n");
        g_quit = 1;
        free(fbuf); free(ibuf);
        return NULL;
    }

    /* ── 수정 4: 프리버퍼 TARGET_FILL_FRAMES 기준 ── */
    wait_prebuf();

    while (!g_quit) {
        if (!ring_read_n(fbuf, g_period, g_ch)) {
            /* 언더런: 무음 출력 */
            memset(ibuf, 0, (size_t)(g_period * g_ch) * sizeof(int32_t));
        } else {
            for (int i = 0; i < g_period * g_ch; i++) {
                float v = fbuf[i];
                if (v >  1.0f) v =  1.0f;
                if (v < -1.0f) v = -1.0f;
                ibuf[i] = (int32_t)(v * 2147483647.0f);
            }
        }

        snd_pcm_sframes_t n = snd_pcm_writei(pcm, ibuf,
                                              (snd_pcm_uframes_t)g_period);
        if (n == -EPIPE) {
            ring_reset();
            snd_pcm_prepare(pcm);
            wait_prebuf();
        } else if (n == -ESTRPIPE) {
            ring_reset();
            while (!g_quit && snd_pcm_resume(pcm) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(pcm);
            wait_prebuf();
        } else if (n < 0) {
            fprintf(stderr, "[audio_out] ALSA error: %s — waiting for device\n",
                    snd_strerror((int)n));
            snd_pcm_close(pcm);
            pcm = NULL;
            ring_reset();
            while (!g_quit) {
                usleep(500000);
                pcm = alsa_open_playback();
                if (pcm) {
                    fprintf(stderr, "[audio_out] device reopened\n");
                    wait_prebuf();
                    break;
                }
            }
        }
    }

    free(fbuf);
    free(ibuf);
    if (pcm) snd_pcm_close(pcm);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 7) {
        fprintf(stderr,
            "Usage: audio_out <client> <device> <rate> <period>"
            " <nperiods> <channels> [ring_frames]\n");
        return 1;
    }

    const char *client = argv[1];
    snprintf(g_dev, sizeof(g_dev), "%s", argv[2]);
    g_rate        = atoi(argv[3]);
    g_period      = atoi(argv[4]);
    g_nperiods    = atoi(argv[5]);
    g_ch          = atoi(argv[6]);
    g_ring_frames = argc > 7 ? atoi(argv[7]) : 8192;

    if (g_ring_frames < 64) g_ring_frames = 64;
    if (g_ch < 1 || g_ch > MAX_CH) {
        fprintf(stderr, "[audio_out] channels out of range (1-%d)\n", MAX_CH);
        return 1;
    }

    g_ring = calloc((size_t)(g_ring_frames * MAX_CH), sizeof(float));
    if (!g_ring) {
        fprintf(stderr, "[audio_out] ring alloc failed\n");
        return 1;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    int err;
    g_src = src_new(SRC_SINC_FASTEST, g_ch, &err);
    if (!g_src) {
        fprintf(stderr, "[audio_out] src_new failed: %s\n", src_strerror(err));
        return 1;
    }

    g_jack = jack_client_open(client, JackNoStartServer, NULL);
    if (!g_jack) {
        fprintf(stderr, "[audio_out] JACK connect failed\n");
        return 1;
    }

    for (int c = 0; c < g_ch; c++) {
        char name[32];
        snprintf(name, sizeof(name), "audio_out_%d", c + 1);
        g_ports[c] = jack_port_register(g_jack, name,
                         JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!g_ports[c]) {
            fprintf(stderr, "[audio_out] port register failed ch=%d\n", c + 1);
            return 1;
        }
    }

    jack_set_process_callback(g_jack, process_cb, NULL);
    jack_on_shutdown(g_jack, on_jack_shutdown, NULL);

    if (jack_activate(g_jack)) {
        fprintf(stderr, "[audio_out] jack_activate failed\n");
        return 1;
    }

    pthread_t tid;
    pthread_create(&tid, NULL, alsa_thread, NULL);

    fprintf(stdout, "[audio_out] ready\n");
    fflush(stdout);

    while (!g_quit) usleep(10000);

    pthread_join(tid, NULL);
    if (g_jack_alive) {
        jack_deactivate(g_jack);
        jack_client_close(g_jack);
    }
    src_delete(g_src);
    free(g_ring);
    return 0;
}