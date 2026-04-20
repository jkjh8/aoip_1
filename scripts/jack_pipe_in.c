/*
 * jack_pipe_in.c — named FIFO (F32LE interleaved) → JACK output ports
 *
 * Reader thread: FIFO → ring buffer
 * JACK process_cb: ring buffer → JACK ports (SRC_LINEAR 드리프트 보정)
 *
 * Usage: jack_pipe_in <client_name> <channels> <fifo_path>
 *
 * Stdout:
 *   [jack_pipe_in] fifo_ready
 *   ports: <client>:out_1, ...
 *   [jack_pipe_in] ready
 */

#define _GNU_SOURCE
#include <jack/jack.h>
#include <samplerate.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <errno.h>
#include <math.h>

#define MAX_CHANNELS  16
#define FIFO_BUF_SIZE (1 << 17)     /* 128 KB pipe kernel buffer */
#define RING_FRAMES   16384         /* ~340 ms at 48 kHz stereo — power of 2 */
#define RATIO_GAIN    0.00002f
#define RATIO_MIN     0.990f
#define RATIO_MAX     1.010f

/* ── lock-free ring buffer ── */
static float       g_ring[RING_FRAMES * MAX_CHANNELS];
static atomic_uint g_wp = 0;
static atomic_uint g_rp = 0;

static void ring_write(const float *src, int n, int ch) {
    unsigned wp = atomic_load_explicit(&g_wp, memory_order_relaxed);
    unsigned rp = atomic_load_explicit(&g_rp, memory_order_acquire);
    unsigned free = RING_FRAMES - (wp - rp);
    if ((unsigned)n > free) n = (int)free; /* drop oldest if full */
    for (int i = 0; i < n; i++) {
        unsigned idx = (wp + (unsigned)i) & (RING_FRAMES - 1);
        memcpy(&g_ring[idx * ch], &src[i * ch], (size_t)ch * sizeof(float));
    }
    atomic_store_explicit(&g_wp, wp + (unsigned)n, memory_order_release);
}

static jack_client_t *g_client;
static jack_port_t   *g_ports[MAX_CHANNELS];
static SRC_STATE     *g_src;
static double         g_ratio      = 1.0;
static int            g_ch         = 2;
static volatile int   g_quit       = 0;
static int            g_jack_alive = 1;
static int            g_prebuf     = 0;
static char           g_client_name[64];
static char           g_fifo_path[256];

static float g_tmp_in [4096 * MAX_CHANNELS];
static float g_tmp_out[4096 * MAX_CHANNELS];

static int process_cb(jack_nframes_t nframes, void *arg)
{
    (void)arg;

    for (int c = 0; c < g_ch; c++)
        memset(jack_port_get_buffer(g_ports[c], nframes), 0,
               nframes * sizeof(float));

    unsigned wp   = atomic_load_explicit(&g_wp, memory_order_acquire);
    unsigned rp   = atomic_load_explicit(&g_rp, memory_order_relaxed);
    int avail = (int)(wp - rp);

    if (!g_prebuf) {
        if (avail < RING_FRAMES / 2) return 0;
        g_prebuf = 1;
    }

    float fill_ratio = (float)avail / (float)(RING_FRAMES / 2);
    g_ratio += (fill_ratio - 1.0f) * RATIO_GAIN;
    if (g_ratio < RATIO_MIN) g_ratio = RATIO_MIN;
    if (g_ratio > RATIO_MAX) g_ratio = RATIO_MAX;

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

    atomic_store_explicit(&g_rp, rp + (unsigned)sd.input_frames_used,
                          memory_order_release);

    long gen = sd.output_frames_gen;
    for (int c = 0; c < g_ch; c++) {
        float *out = jack_port_get_buffer(g_ports[c], nframes);
        for (long f = 0; f < gen; f++)
            out[f] = g_tmp_out[f * g_ch + c];
    }

    return 0;
}

static void on_jack_shutdown(void *arg) { (void)arg; g_jack_alive = 0; g_quit = 1; }
static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ── FIFO reader thread ── */
static int g_fifo_fd = -1;

static void *fifo_reader_thread(void *arg)
{
    (void)arg;
    /* 읽기 버퍼: 최대 4096 프레임 */
    int   buf_frames = 4096;
    float *fbuf = malloc((size_t)(buf_frames * g_ch) * sizeof(float));

    while (!g_quit) {
        ssize_t bytes = (ssize_t)(buf_frames * g_ch) * (ssize_t)sizeof(float);
        ssize_t got   = read(g_fifo_fd, fbuf, (size_t)bytes);
        if (got > 0) {
            int frames = (int)(got / (g_ch * (ssize_t)sizeof(float)));
            ring_write(fbuf, frames, g_ch);
        } else if (got == 0 || (got < 0 && errno != EAGAIN)) {
            usleep(1000);
        }
    }

    free(fbuf);
    return NULL;
}

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: jack_pipe_in <client_name> <channels> <fifo_path>\n");
        return 1;
    }

    snprintf(g_client_name, sizeof(g_client_name), "%s", argv[1]);
    g_ch = atoi(argv[2]);
    snprintf(g_fifo_path, sizeof(g_fifo_path), "%s", argv[3]);

    if (g_ch < 1 || g_ch > MAX_CHANNELS) {
        fprintf(stderr, "[jack_pipe_in] channels out of range (1-%d)\n", MAX_CHANNELS);
        return 1;
    }

    /* ── FIFO ── */
    unlink(g_fifo_path);
    if (mkfifo(g_fifo_path, 0600) < 0) { perror("[jack_pipe_in] mkfifo"); return 1; }

    g_fifo_fd = open(g_fifo_path, O_RDWR | O_NONBLOCK);
    if (g_fifo_fd < 0) { perror("[jack_pipe_in] open fifo"); return 1; }
    fcntl(g_fifo_fd, F_SETPIPE_SZ, FIFO_BUF_SIZE);

    fprintf(stdout, "[jack_pipe_in] fifo_ready\n");
    fflush(stdout);

    /* ── SRC ── */
    int err;
    g_src = src_new(SRC_LINEAR, g_ch, &err);
    if (!g_src) { fprintf(stderr, "[jack_pipe_in] src_new: %s\n", src_strerror(err)); return 1; }

    /* ── JACK ── */
    g_client = jack_client_open(g_client_name, JackNoStartServer, NULL);
    if (!g_client) { fprintf(stderr, "[jack_pipe_in] JACK connect failed\n"); return 1; }

    for (int c = 0; c < g_ch; c++) {
        char name[32];
        snprintf(name, sizeof(name), "out_%d", c + 1);
        g_ports[c] = jack_port_register(g_client, name,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
        if (!g_ports[c]) {
            fprintf(stderr, "[jack_pipe_in] port register failed (ch=%d)\n", c + 1);
            return 1;
        }
    }

    jack_set_process_callback(g_client, process_cb, NULL);
    jack_on_shutdown(g_client, on_jack_shutdown, NULL);
    if (jack_activate(g_client)) {
        fprintf(stderr, "[jack_pipe_in] jack_activate failed\n");
        return 1;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    /* ── FIFO reader thread 시작 ── */
    pthread_t tid;
    pthread_create(&tid, NULL, fifo_reader_thread, NULL);

    fprintf(stdout, "ports:");
    for (int c = 0; c < g_ch; c++) {
        if (c) fprintf(stdout, ",");
        fprintf(stdout, " %s:out_%d", g_client_name, c + 1);
    }
    fprintf(stdout, "\n");
    fprintf(stdout, "[jack_pipe_in] ready\n");
    fflush(stdout);

    while (!g_quit)
        usleep(10000);

    pthread_join(tid, NULL);

    if (g_jack_alive) {
        jack_deactivate(g_client);
        jack_client_close(g_client);
    }
    src_delete(g_src);
    close(g_fifo_fd);
    unlink(g_fifo_path);

    return 0;
}
