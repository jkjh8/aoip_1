/*
 * alsa_in.c — ALSA capture → JACK output ports (adaptive drift correction)
 *
 * ALSA 캡처 스레드 → ring buffer → JACK process_cb (SRC_LINEAR 드리프트 보정)
 *
 * Usage: alsa_in <client_name> <alsa_device> <rate> <period> <nperiods> <channels>
 *
 * Stdout:
 *   [alsa_in] ready
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

#define MAX_CH       8
#define RING_FRAMES  8192        /* ~170ms at 48kHz — power of 2 */
#define RATIO_GAIN   0.00002f
#define RATIO_MIN    0.998f
#define RATIO_MAX    1.002f

/* ── lock-free ring buffer (single producer / single consumer) ── */
static float       g_ring[RING_FRAMES * MAX_CH];
static atomic_uint g_wp = 0;
static atomic_uint g_rp = 0;

static void ring_write(const float *src, int n, int ch) {
    unsigned wp = atomic_load_explicit(&g_wp, memory_order_relaxed);
    unsigned rp = atomic_load_explicit(&g_rp, memory_order_acquire);
    unsigned free = RING_FRAMES - (wp - rp);
    if ((unsigned)n > free) n = (int)free;
    for (int i = 0; i < n; i++) {
        unsigned idx = (wp + (unsigned)i) & (RING_FRAMES - 1);
        memcpy(&g_ring[idx * ch], &src[i * ch], (size_t)ch * sizeof(float));
    }
    atomic_store_explicit(&g_wp, wp + (unsigned)n, memory_order_release);
}

/* ── globals ── */
static jack_client_t *g_jack;
static jack_port_t   *g_ports[MAX_CH];
static SRC_STATE     *g_src;
static double         g_ratio    = 1.0;
static volatile int   g_quit     = 0;
static int            g_jack_alive = 1;
static int            g_ch       = 2;
static int            g_prebuf   = 0;   /* 1 = 초기 프리버퍼 완료 */

static float g_tmp_in [4096 * MAX_CH];
static float g_tmp_out[4096 * MAX_CH];

/* ── JACK process callback ── */
static int process_cb(jack_nframes_t nframes, void *arg)
{
    (void)arg;

    for (int c = 0; c < g_ch; c++)
        memset(jack_port_get_buffer(g_ports[c], nframes), 0,
               nframes * sizeof(float));

    unsigned wp   = atomic_load_explicit(&g_wp, memory_order_acquire);
    unsigned rp   = atomic_load_explicit(&g_rp, memory_order_relaxed);
    int avail = (int)(wp - rp);

    /* 초기 프리버퍼: ring이 절반 이상 찰 때까지 무음 대기 */
    if (!g_prebuf) {
        if (avail < RING_FRAMES / 2) return 0;
        g_prebuf = 1;
    }

    /* 드리프트 보정 ratio 조정 */
    float fill_ratio = (float)avail / (float)(RING_FRAMES / 2);
    g_ratio += (fill_ratio - 1.0f) * RATIO_GAIN;
    if (g_ratio < RATIO_MIN) g_ratio = RATIO_MIN;
    if (g_ratio > RATIO_MAX) g_ratio = RATIO_MAX;

    /* ring에서 input_need 프레임 peek (rp 아직 이동 안 함) */
    int input_need = (int)ceil((double)nframes / g_ratio) + 2;
    if (input_need > avail) input_need = avail;

    for (int i = 0; i < input_need; i++) {
        unsigned idx = (rp + (unsigned)i) & (RING_FRAMES - 1);
        memcpy(&g_tmp_in[i * g_ch], &g_ring[idx * g_ch],
               (size_t)g_ch * sizeof(float));
    }

    SRC_DATA sd = {
        .data_in       = g_tmp_in,
        .data_out      = g_tmp_out,
        .input_frames  = input_need,
        .output_frames = (long)nframes,
        .src_ratio     = g_ratio,
        .end_of_input  = 0,
    };
    src_process(g_src, &sd);

    /* SRC가 실제 소비한 만큼만 rp 전진 */
    atomic_store_explicit(&g_rp, rp + (unsigned)sd.input_frames_used,
                          memory_order_release);

    /* de-interleave → JACK 포트 */
    long gen = sd.output_frames_gen;
    for (int c = 0; c < g_ch; c++) {
        float *out = jack_port_get_buffer(g_ports[c], nframes);
        for (long f = 0; f < gen; f++)
            out[f] = g_tmp_out[f * g_ch + c];
    }

    return 0;
}

static void on_jack_shutdown(void *arg) { (void)arg; g_jack_alive = 0; g_quit = 1; }
static void on_signal(int s) { (void)s; g_quit = 1; }

/* ── ALSA 캡처 스레드 ── */
static char g_dev[64]    = "hw:0";
static int  g_rate       = 48000;
static int  g_period     = 48;
static int  g_nperiods   = 8;

static snd_pcm_t *alsa_open_capture(void)
{
    snd_pcm_t *pcm = NULL;
    if (snd_pcm_open(&pcm, g_dev, SND_PCM_STREAM_CAPTURE, 0) < 0) return NULL;
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
    snd_pcm_hw_params(pcm, hw);
    snd_pcm_prepare(pcm);
    return pcm;
}

static void ring_reset(void)
{
    unsigned wp = atomic_load_explicit(&g_wp, memory_order_relaxed);
    atomic_store_explicit(&g_rp, wp, memory_order_release);
    g_prebuf = 0;
}

static void *alsa_thread(void *arg)
{
    (void)arg;
    snd_pcm_t *pcm = alsa_open_capture();
    if (!pcm) { fprintf(stderr, "[alsa_in] cannot open %s\n", g_dev); g_quit = 1; return NULL; }

    int32_t *ibuf = malloc((size_t)(g_period * g_ch) * sizeof(int32_t));
    float   *fbuf = malloc((size_t)(g_period * g_ch) * sizeof(float));

    while (!g_quit) {
        snd_pcm_sframes_t n = snd_pcm_readi(pcm, ibuf,
                                             (snd_pcm_uframes_t)g_period);
        if (n == -EPIPE) {
            ring_reset();
            snd_pcm_prepare(pcm);
            continue;
        }
        if (n == -ESTRPIPE) {
            ring_reset();
            while (!g_quit && snd_pcm_resume(pcm) == -EAGAIN) usleep(10000);
            snd_pcm_prepare(pcm);
            continue;
        }
        if (n < 0) {
            /* device removed — reset ring and wait for reopen */
            fprintf(stderr, "[alsa_in] ALSA error: %s — waiting for device\n", snd_strerror((int)n));
            snd_pcm_close(pcm);
            pcm = NULL;
            ring_reset();
            while (!g_quit) {
                usleep(500000);
                pcm = alsa_open_capture();
                if (pcm) { fprintf(stderr, "[alsa_in] device reopened\n"); break; }
            }
            continue;
        }

        /* S32LE → float */
        for (int i = 0; i < (int)n * g_ch; i++)
            fbuf[i] = ibuf[i] * (1.0f / 2147483648.0f);

        ring_write(fbuf, (int)n, g_ch);
    }

    free(ibuf);
    free(fbuf);
    if (pcm) snd_pcm_close(pcm);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 7) {
        fprintf(stderr,
            "Usage: alsa_in <client> <device> <rate> <period> <nperiods> <channels>\n");
        return 1;
    }
    const char *client = argv[1];
    snprintf(g_dev, sizeof(g_dev), "%s", argv[2]);
    g_rate     = atoi(argv[3]);
    g_period   = atoi(argv[4]);
    g_nperiods = atoi(argv[5]);
    g_ch       = atoi(argv[6]);

    if (g_ch < 1 || g_ch > MAX_CH) {
        fprintf(stderr, "[alsa_in] channels out of range\n");
        return 1;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    int err;
    g_src = src_new(SRC_LINEAR, g_ch, &err);
    if (!g_src) {
        fprintf(stderr, "[alsa_in] src_new failed: %s\n", src_strerror(err));
        return 1;
    }

    g_jack = jack_client_open(client, JackNoStartServer, NULL);
    if (!g_jack) { fprintf(stderr, "[alsa_in] JACK connect failed\n"); return 1; }

    for (int c = 0; c < g_ch; c++) {
        char name[32];
        snprintf(name, sizeof(name), "capture_%d", c + 1);
        g_ports[c] = jack_port_register(g_jack, name,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!g_ports[c]) {
            fprintf(stderr, "[alsa_in] port register failed ch=%d\n", c + 1);
            return 1;
        }
    }

    jack_set_process_callback(g_jack, process_cb, NULL);
    jack_on_shutdown(g_jack, on_jack_shutdown, NULL);
    if (jack_activate(g_jack)) {
        fprintf(stderr, "[alsa_in] jack_activate failed\n");
        return 1;
    }

    pthread_t tid;
    pthread_create(&tid, NULL, alsa_thread, NULL);

    fprintf(stdout, "[alsa_in] ready\n");
    fflush(stdout);

    while (!g_quit) usleep(10000);

    pthread_join(tid, NULL);
    if (g_jack_alive) { jack_deactivate(g_jack); jack_client_close(g_jack); }
    src_delete(g_src);
    return 0;
}
