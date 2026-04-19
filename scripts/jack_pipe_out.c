/*
 * jack_pipe_out.c — JACK input ports → named FIFO (RT-safe, non-blocking)
 *
 * JACK 클라이언트로 등록하여 입력 포트에서 오디오를 받아
 * named FIFO에 F32LE interleaved PCM으로 non-blocking 기록.
 * FIFO가 가득 차면 해당 주기 데이터를 drop (JACK XRun 방지).
 *
 * Usage: jack_pipe_out <client_name> <channels> <fifo_path>
 *
 * Stdout:
 *   [jack_pipe_out] fifo_ready          — FIFO 생성 완료 (rtp_send 기동 가능)
 *   ports: <client>:in_src_1, ...       — JACK 포트 목록
 *   [jack_pipe_out] ready               — JACK 활성화 완료
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

#define MAX_CHANNELS 16
#define FIFO_BUF_SIZE (1 << 17)   /* 128 KB ≈ 340 ms at 48 kHz stereo F32LE */

static jack_client_t *g_client;
static jack_port_t   *g_ports[MAX_CHANNELS];
static int            g_ch        = 2;
static int            g_fifo_fd   = -1;
static volatile int   g_quit      = 0;
static int            g_jack_alive = 1;
static char           g_client_name[64];
static char           g_fifo_path[256];

/* 최대 nframes=1024, 채널=16: 1024*16*4 = 65536 bytes */
static float g_interleave_buf[1024 * MAX_CHANNELS];

static int process_cb(jack_nframes_t nframes, void *arg)
{
    (void)arg;

    float *src_bufs[MAX_CHANNELS];
    for (int c = 0; c < g_ch; c++)
        src_bufs[c] = (float *)jack_port_get_buffer(g_ports[c], nframes);

    /* 채널 인터리브: F32LE interleaved */
    for (jack_nframes_t f = 0; f < nframes; f++)
        for (int c = 0; c < g_ch; c++)
            g_interleave_buf[f * g_ch + c] = src_bufs[c][f];

    /* non-blocking write — FIFO 가득 차면 EAGAIN으로 drop (RT-safe) */
    ssize_t bytes = (ssize_t)(nframes * (jack_nframes_t)g_ch * sizeof(float));
    write(g_fifo_fd, g_interleave_buf, (size_t)bytes);

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
        fprintf(stderr, "Usage: jack_pipe_out <client_name> <channels> <fifo_path>\n");
        return 1;
    }

    snprintf(g_client_name, sizeof(g_client_name), "%s", argv[1]);
    g_ch = atoi(argv[2]);
    snprintf(g_fifo_path,   sizeof(g_fifo_path),   "%s", argv[3]);

    if (g_ch < 1 || g_ch > MAX_CHANNELS) {
        fprintf(stderr, "[jack_pipe_out] channels out of range (1-%d)\n", MAX_CHANNELS);
        return 1;
    }

    /* ── FIFO 생성 및 열기 ─────────────────────────────────── */
    unlink(g_fifo_path);
    if (mkfifo(g_fifo_path, 0600) < 0) {
        perror("[jack_pipe_out] mkfifo");
        return 1;
    }

    /*
     * O_RDWR | O_NONBLOCK: reader 없이도 즉시 열림.
     * write() 시 reader가 없어도 SIGPIPE 없이 정상 동작.
     * reader(rtp_send)가 붙으면 실제 데이터가 전달됨.
     */
    g_fifo_fd = open(g_fifo_path, O_RDWR | O_NONBLOCK);
    if (g_fifo_fd < 0) {
        perror("[jack_pipe_out] open fifo");
        return 1;
    }

    /* FIFO 커널 버퍼 확장 */
    fcntl(g_fifo_fd, F_SETPIPE_SZ, FIFO_BUF_SIZE);

    fprintf(stdout, "[jack_pipe_out] fifo_ready\n");
    fflush(stdout);

    /* ── JACK 연결 ─────────────────────────────────────────── */
    g_client = jack_client_open(g_client_name, JackNoStartServer, NULL);
    if (!g_client) {
        fprintf(stderr, "[jack_pipe_out] JACK connect failed (client=%s)\n", g_client_name);
        return 1;
    }

    /* 입력 포트 등록: in_src_1 … in_src_N (rtp_send 기존 이름과 동일) */
    for (int c = 0; c < g_ch; c++) {
        char name[32];
        snprintf(name, sizeof(name), "in_src_%d", c + 1);
        g_ports[c] = jack_port_register(g_client, name,
            JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        if (!g_ports[c]) {
            fprintf(stderr, "[jack_pipe_out] port register failed (ch=%d)\n", c + 1);
            return 1;
        }
    }

    jack_set_process_callback(g_client, process_cb, NULL);
    jack_on_shutdown(g_client, on_jack_shutdown, NULL);

    if (jack_activate(g_client)) {
        fprintf(stderr, "[jack_pipe_out] jack_activate failed\n");
        return 1;
    }

    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    /* 포트 목록 및 ready 신호 출력 */
    fprintf(stdout, "ports:");
    for (int c = 0; c < g_ch; c++) {
        if (c) fprintf(stdout, ",");
        fprintf(stdout, " %s:in_src_%d", g_client_name, c + 1);
    }
    fprintf(stdout, "\n");
    fprintf(stdout, "[jack_pipe_out] ready\n");
    fflush(stdout);

    while (!g_quit)
        usleep(10000);

    if (g_jack_alive) {
        jack_deactivate(g_client);
        jack_client_close(g_client);
    }

    close(g_fifo_fd);
    unlink(g_fifo_path);

    fprintf(stderr, "[jack_pipe_out] exiting\n");
    return 0;
}
