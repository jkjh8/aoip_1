/*
 * jack_pipe_in.c — named FIFO (F32LE interleaved) → JACK output ports (RT-safe)
 *
 * jack_pipe_out의 역방향: GStreamer(rtp_recv)가 FIFO에 쓰면
 * JACK process callback에서 non-blocking read → JACK 출력 포트로 전달.
 * FIFO가 비어 있으면 해당 주기를 무음으로 채움 (JACK XRun 방지).
 *
 * Usage: jack_pipe_in <client_name> <channels> <fifo_path>
 *
 * Stdout:
 *   [jack_pipe_in] fifo_ready         — FIFO 생성 완료 (rtp_recv 기동 가능)
 *   ports: <client>:out_1, ...        — JACK 포트 목록
 *   [jack_pipe_in] ready              — JACK 활성화 완료
 */

#define _GNU_SOURCE
#include <jack/jack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <fcntl.h>
#include <unistd.h>
#include <signal.h>
#include <sys/stat.h>
#include <errno.h>

#define MAX_CHANNELS  16
#define FIFO_BUF_SIZE (1 << 17)   /* 128 KB ≈ 340 ms at 48 kHz stereo F32LE */

static jack_client_t *g_client;
static jack_port_t   *g_ports[MAX_CHANNELS];
static int            g_ch         = 2;
static int            g_fifo_fd    = -1;
static volatile int   g_quit       = 0;
static int            g_jack_alive = 1;
static char           g_client_name[64];
static char           g_fifo_path[256];

/* 최대 nframes=1024, 채널=16: 1024*16*4 = 65536 bytes */
static float g_interleave_buf[1024 * MAX_CHANNELS];

static int process_cb(jack_nframes_t nframes, void *arg)
{
    (void)arg;

    float *dst_bufs[MAX_CHANNELS];
    for (int c = 0; c < g_ch; c++) {
        dst_bufs[c] = (float *)jack_port_get_buffer(g_ports[c], nframes);
        memset(dst_bufs[c], 0, nframes * sizeof(float));
    }

    /* non-blocking read — FIFO 비어 있으면 EAGAIN, 무음 유지 (RT-safe) */
    ssize_t bytes = (ssize_t)(nframes * (jack_nframes_t)g_ch * sizeof(float));
    ssize_t got   = read(g_fifo_fd, g_interleave_buf, (size_t)bytes);

    if (got > 0) {
        jack_nframes_t valid = (jack_nframes_t)(got / (g_ch * (ssize_t)sizeof(float)));
        for (jack_nframes_t f = 0; f < valid; f++)
            for (int c = 0; c < g_ch; c++)
                dst_bufs[c][f] = g_interleave_buf[f * g_ch + c];
    }

    return 0;
}

static void on_jack_shutdown(void *arg)
{
    (void)arg;
    g_jack_alive = 0;
    g_quit = 1;
}

static void on_signal(int sig) { (void)sig; g_quit = 1; }

int main(int argc, char *argv[])
{
    if (argc < 4) {
        fprintf(stderr, "Usage: jack_pipe_in <client_name> <channels> <fifo_path>\n");
        return 1;
    }

    snprintf(g_client_name, sizeof(g_client_name), "%s", argv[1]);
    g_ch = atoi(argv[2]);
    snprintf(g_fifo_path,   sizeof(g_fifo_path),   "%s", argv[3]);

    if (g_ch < 1 || g_ch > MAX_CHANNELS) {
        fprintf(stderr, "[jack_pipe_in] channels out of range (1-%d)\n", MAX_CHANNELS);
        return 1;
    }

    /* ── FIFO 생성 및 열기 ─────────────────────────────────── */
    unlink(g_fifo_path);
    if (mkfifo(g_fifo_path, 0600) < 0) {
        perror("[jack_pipe_in] mkfifo");
        return 1;
    }

    /*
     * O_RDWR | O_NONBLOCK: writer(rtp_recv) 없이도 즉시 열림.
     * read()는 non-blocking — 데이터 없으면 EAGAIN (RT-safe).
     */
    g_fifo_fd = open(g_fifo_path, O_RDWR | O_NONBLOCK);
    if (g_fifo_fd < 0) {
        perror("[jack_pipe_in] open fifo");
        return 1;
    }

    /* FIFO 커널 버퍼 확장 */
    fcntl(g_fifo_fd, F_SETPIPE_SZ, FIFO_BUF_SIZE);

    fprintf(stdout, "[jack_pipe_in] fifo_ready\n");
    fflush(stdout);

    /* ── JACK 연결 ─────────────────────────────────────────── */
    g_client = jack_client_open(g_client_name, JackNoStartServer, NULL);
    if (!g_client) {
        fprintf(stderr, "[jack_pipe_in] JACK connect failed (client=%s)\n", g_client_name);
        return 1;
    }

    /* 출력 포트 등록: out_1 … out_N */
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

    /* 포트 목록 및 ready 신호 출력 */
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

    if (g_jack_alive) {
        jack_deactivate(g_client);
        jack_client_close(g_client);
    }

    close(g_fifo_fd);
    unlink(g_fifo_path);

    fprintf(stderr, "[jack_pipe_in] exiting\n");
    return 0;
}
