/*
 * rtp_send.c — Lightweight RTP/UDP sender (shared memory input)
 *
 * GStreamer 완전 제거 — libsamplerate + libmp3lame + raw UDP 소켓
 *
 * 흐름:
 *   SHM(F32LE@48k) → libsamplerate → lame/L16 → RTP 패킷 → UDP sendto()
 *
 * Usage:  rtp_send <channels> <client> <proto> <outRate> shm <shm_name>
 *
 * Stdin commands:
 *   add <host> <port>
 *   remove <host> <port>
 *   codec <mp3|raw> [bitrate]
 *   quit
 */

#include <samplerate.h>
#include <lame/lame.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>

/* ── 상수 ────────────────────────────────────────────── */
#define MAX_TARGETS    16
#define PERIOD_FRAMES  512
#define SAMPLE_RATE    48000
#define LAME_GRAN      1152         /* LAME MP3 그래뉼 크기 */
#define MP3_ACC_MAX    (LAME_GRAN * 4)  /* 누적 버퍼: 4그래뉼 여유 */
#define MP3_BUF_SIZE   (LAME_GRAN * 5 / 4 + 7200) /* lame 권장 출력 버퍼 */
#define RTP_HDR_SIZE   12
#define MPA_HDR_SIZE   4            /* RFC 2250 MPEG audio header */
#define MAX_PKT_SIZE   1472         /* UDP payload MTU safe */

/* ── ShmRing ─────────────────────────────────────────── */
#define SHM_RING_FRAMES 16384
#define SHM_MAX_CH      8

typedef struct {
    _Atomic uint32_t wp;
    _Atomic uint32_t rp;
    int32_t  channels;
    int32_t  ring_frames;
    uint8_t  _pad[48];
    float    buf[SHM_RING_FRAMES * SHM_MAX_CH];
} ShmRing;

#define SHMRING_SIZE ((size_t)sizeof(ShmRing))

/* ── target ──────────────────────────────────────────── */
typedef struct {
    char             host[128];
    int              port;
    struct sockaddr_in addr;
} Target;

static Target          g_targets[MAX_TARGETS];
static int             g_n_targets  = 0;
static int             g_udp_sock   = -1;
static pthread_mutex_t g_target_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── codec ───────────────────────────────────────────── */
typedef enum { CODEC_MP3, CODEC_RAW } Codec;
static Codec  g_codec   = CODEC_MP3;
static int    g_bitrate = 320;

/* ── globals ─────────────────────────────────────────── */
static int          g_ch       = 2;
static int          g_use_rtp  = 1;
static int          g_out_rate = 44100;
static volatile int g_quit     = 0;
static char         g_client_name[64] = "rtp_send";

/* SHM */
static char     g_shm_name[256] = "";
static int      g_shm_fd        = -1;
static ShmRing *g_shm           = NULL;

/* libsamplerate */
static SRC_STATE *g_src = NULL;

/* lame */
static lame_t g_lame = NULL;
static pthread_mutex_t g_lame_mtx = PTHREAD_MUTEX_INITIALIZER;

/* MP3 입력 누적 버퍼 (1152 배수 단위로 인코딩하기 위해) */
static float   g_mp3_acc[MP3_ACC_MAX * 2];  /* 최대 2ch */
static int     g_mp3_acc_frames = 0;

/* RTP 상태 */
static uint16_t g_rtp_seq  = 0;
static uint32_t g_rtp_ts   = 0;
static uint32_t g_rtp_ssrc = 0;

/* stats */
static atomic_ulong g_bytes_sent = 0;

/* reader thread */
static pthread_t    g_reader_tid;
static volatile int g_reader_run = 0;

/* ── codec 변경 플래그 ───────────────────────────────── */
static volatile int    g_codec_changed = 0;
static Codec           g_new_codec     = CODEC_MP3;
static int             g_new_bitrate   = 320;
static pthread_mutex_t g_codec_mtx     = PTHREAD_MUTEX_INITIALIZER;

/* ── UDP 소켓 초기화 ─────────────────────────────────── */
static int udp_init(void) {
    if (g_udp_sock >= 0) { close(g_udp_sock); g_udp_sock = -1; }
    g_udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g_udp_sock < 0) {
        fprintf(stderr, "[rtp_send] socket: %s\n", strerror(errno));
        return 0;
    }
    int ttl = 15;
    setsockopt(g_udp_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    /* 유니캐스트 TTL도 설정 */
    setsockopt(g_udp_sock, IPPROTO_IP, IP_TTL, &ttl, sizeof(ttl));
    return 1;
}

/* ── RTP 패킷 전송 ───────────────────────────────────── */
static void rtp_send_packet(const uint8_t *payload, int payload_len, int marker)
{
    if (g_udp_sock < 0 || payload_len <= 0) return;

    /* PT: MPA=14(90kHz), L16-stereo=10, L16-mono=11 */
    uint8_t pt;
    if (g_codec == CODEC_MP3) {
        pt = 14;
    } else {
        pt = (g_ch == 1) ? 11 : 10;
        /* 비표준 레이트면 dynamic PT 사용 */
        if (g_out_rate != 44100) pt = 96;
    }

    uint8_t pkt[RTP_HDR_SIZE + MPA_HDR_SIZE + MAX_PKT_SIZE];
    int hdr_extra = (g_codec == CODEC_MP3) ? MPA_HDR_SIZE : 0;
    int pkt_len   = RTP_HDR_SIZE + hdr_extra + payload_len;

    /* RTP 헤더 */
    pkt[0]  = 0x80;                      /* V=2, P=0, X=0, CC=0 */
    pkt[1]  = (marker ? 0x80 : 0) | (pt & 0x7f);
    pkt[2]  = g_rtp_seq >> 8;
    pkt[3]  = g_rtp_seq & 0xff;
    pkt[4]  = g_rtp_ts >> 24;
    pkt[5]  = (g_rtp_ts >> 16) & 0xff;
    pkt[6]  = (g_rtp_ts >> 8)  & 0xff;
    pkt[7]  = g_rtp_ts & 0xff;
    pkt[8]  = g_rtp_ssrc >> 24;
    pkt[9]  = (g_rtp_ssrc >> 16) & 0xff;
    pkt[10] = (g_rtp_ssrc >> 8)  & 0xff;
    pkt[11] = g_rtp_ssrc & 0xff;
    g_rtp_seq++;

    /* RFC 2250 MPEG audio header (4바이트, 모두 0) */
    if (hdr_extra) memset(pkt + RTP_HDR_SIZE, 0, hdr_extra);

    memcpy(pkt + RTP_HDR_SIZE + hdr_extra, payload, payload_len);

    pthread_mutex_lock(&g_target_mtx);
    for (int i = 0; i < g_n_targets; i++) {
        ssize_t sent = sendto(g_udp_sock, pkt, pkt_len, 0,
                              (struct sockaddr *)&g_targets[i].addr,
                              sizeof(g_targets[i].addr));
        if (sent > 0)
            atomic_fetch_add_explicit(&g_bytes_sent, (unsigned long)sent,
                                      memory_order_relaxed);
    }
    pthread_mutex_unlock(&g_target_mtx);
}

/* ── lame 초기화 ─────────────────────────────────────── */
static int lame_reinit(int rate, int ch, int bitrate)
{
    if (g_lame) { lame_close(g_lame); g_lame = NULL; }
    g_lame = lame_init();
    if (!g_lame) return 0;
    lame_set_in_samplerate(g_lame, rate);
    lame_set_num_channels(g_lame, ch);
    lame_set_out_samplerate(g_lame, rate);
    lame_set_brate(g_lame, bitrate);
    lame_set_quality(g_lame, 7);        /* 7=fastest, 2=best */
    lame_set_VBR(g_lame, vbr_off);     /* CBR */
    lame_set_bWriteVbrTag(g_lame, 0);
    if (lame_init_params(g_lame) < 0) {
        lame_close(g_lame); g_lame = NULL; return 0;
    }
    fprintf(stderr, "[rtp_send] lame init: %dHz %dch %dkbps quality=7\n",
            rate, ch, bitrate);
    return 1;
}

/* ── SHM attach ──────────────────────────────────────── */
static int shm_attach(void)
{
    for (int i = 0; i < 50; i++) {
        g_shm_fd = shm_open(g_shm_name, O_RDWR, 0);
        if (g_shm_fd < 0) { usleep(100000); continue; }

        /* aoip_engine이 ftruncate를 완료하지 않은 0바이트 SHM → SIGBUS 방지 */
        struct stat st;
        if (fstat(g_shm_fd, &st) < 0 || (size_t)st.st_size < SHMRING_SIZE) {
            close(g_shm_fd); g_shm_fd = -1;
            usleep(10000);
            continue;
        }

        g_shm = mmap(NULL, SHMRING_SIZE, PROT_READ|PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
        if (g_shm == MAP_FAILED) {
            fprintf(stderr, "[rtp_send] mmap: %s\n", strerror(errno));
            close(g_shm_fd); g_shm_fd = -1; g_shm = NULL; return 0;
        }

        /* aoip_engine 초기화 완료 대기 (ring_frames 설정 확인) */
        for (int w = 0; w < 200; w++) {
            if (__atomic_load_n(&g_shm->ring_frames, __ATOMIC_ACQUIRE) != 0) break;
            usleep(5000);
        }
        if (g_shm->ring_frames == 0) {
            munmap(g_shm, SHMRING_SIZE); close(g_shm_fd);
            g_shm = NULL; g_shm_fd = -1;
            usleep(100000);
            continue;
        }

        /* 재시작 시 stale rp 제거: 현재 wp 위치에서 시작 */
        {
            uint32_t cur_wp = __atomic_load_n(&g_shm->wp, __ATOMIC_ACQUIRE);
            __atomic_store_n(&g_shm->rp, cur_wp, __ATOMIC_RELEASE);
        }

        fprintf(stderr, "[rtp_send] attached shm %s\n", g_shm_name);
        return 1;
    }
    fprintf(stderr, "[rtp_send] shm_open(%s): %s\n", g_shm_name, strerror(errno));
    return 0;
}

/* ── float → S16BE 변환 ──────────────────────────────── */
static void f32_to_s16be(const float *in, uint8_t *out, int samples)
{
    for (int i = 0; i < samples; i++) {
        float v = in[i];
        if (v >  1.0f) v =  1.0f;
        if (v < -1.0f) v = -1.0f;
        int16_t s = (int16_t)(v * 32767.0f);
        out[i*2]   = (s >> 8) & 0xff;
        out[i*2+1] = s & 0xff;
    }
}

/* ── 송신: L16 raw ───────────────────────────────────── */
#define L16_FRAMES_PER_PKT 256   /* 256 frames @ 44100Hz ≈ 5.8ms */

static void send_raw_l16(const float *buf, int frames)
{
    int frames_sent = 0;

    while (frames_sent < frames) {
        int chunk = frames - frames_sent;
        if (chunk > L16_FRAMES_PER_PKT) chunk = L16_FRAMES_PER_PKT;

        int samples = chunk * g_ch;
        uint8_t pcm[L16_FRAMES_PER_PKT * 8 * 2]; /* max ch=8, S16=2bytes */
        f32_to_s16be(buf + frames_sent * g_ch, pcm, samples);

        /* RTP 타임스탬프: L16은 샘플레이트 기반 */
        rtp_send_packet(pcm, samples * 2, (frames_sent == 0) ? 1 : 0);
        g_rtp_ts += (uint32_t)chunk;
        frames_sent += chunk;
    }
}

/* ── 송신: MP3 (1152 그래뉼 단위 누적 후 인코딩) ────── */
static void send_mp3(const float *buf, int frames)
{
    /* 누적 버퍼에 추가 */
    int space = MP3_ACC_MAX - g_mp3_acc_frames;
    if (frames > space) frames = space;
    memcpy(g_mp3_acc + g_mp3_acc_frames * g_ch, buf,
           (size_t)(frames * g_ch) * sizeof(float));
    g_mp3_acc_frames += frames;

    /* 1152 프레임이 쌓일 때마다 인코딩 */
    int ch2 = (g_ch >= 2);
    float left[LAME_GRAN], right[LAME_GRAN];

    while (g_mp3_acc_frames >= LAME_GRAN) {
        const float *src = g_mp3_acc;
        for (int f = 0; f < LAME_GRAN; f++) {
            left[f]  = src[f * g_ch];
            right[f] = ch2 ? src[f * g_ch + 1] : src[f * g_ch];
        }

        uint8_t mp3buf[MP3_BUF_SIZE];
        int mp3len = 0;
        pthread_mutex_lock(&g_lame_mtx);
        if (g_lame)
            mp3len = lame_encode_buffer_ieee_float(
                         g_lame, left, right, LAME_GRAN,
                         mp3buf, sizeof(mp3buf));
        pthread_mutex_unlock(&g_lame_mtx);

        /* 사용한 1152 프레임을 버퍼에서 제거 */
        g_mp3_acc_frames -= LAME_GRAN;
        if (g_mp3_acc_frames > 0)
            memmove(g_mp3_acc, g_mp3_acc + LAME_GRAN * g_ch,
                    (size_t)(g_mp3_acc_frames * g_ch) * sizeof(float));

        if (mp3len <= 0) continue;

        /* MP3 RTP: 90kHz 타임스탬프 */
        uint32_t ts_inc = (uint32_t)((uint64_t)LAME_GRAN * 90000 / (uint64_t)g_out_rate);
        rtp_send_packet(mp3buf, mp3len, 1);
        g_rtp_ts += ts_inc;
    }
}

/* ── shm reader thread ───────────────────────────────── */
static void *shm_reader_thread(void *arg)
{
    (void)arg;
    int out_rate  = g_out_rate;
    int need_src  = (out_rate != SAMPLE_RATE) && (g_src != NULL);

    float *in_buf  = calloc(PERIOD_FRAMES * g_ch, sizeof(float));
    int out_max    = need_src
        ? (int)((double)PERIOD_FRAMES * out_rate / SAMPLE_RATE * 1.1 + 64)
        : PERIOD_FRAMES;
    float *out_buf = calloc(out_max * g_ch, sizeof(float));

    while (g_reader_run) {
        /* 코덱 변경 처리 */
        if (g_codec_changed) {
            pthread_mutex_lock(&g_codec_mtx);
            Codec  nc = g_new_codec;
            int    nb = g_new_bitrate;
            g_codec_changed = 0;
            pthread_mutex_unlock(&g_codec_mtx);

            g_codec   = nc;
            g_bitrate = nb;
            g_mp3_acc_frames = 0;   /* 누적 버퍼 초기화 */
            if (g_codec == CODEC_MP3) {
                pthread_mutex_lock(&g_lame_mtx);
                lame_reinit(out_rate, g_ch, g_bitrate);
                pthread_mutex_unlock(&g_lame_mtx);
            }
            fprintf(stderr, "[rtp_send] codec=%s bitrate=%d\n",
                    g_codec == CODEC_MP3 ? "mp3" : "raw", g_bitrate);
        }

        if (!g_shm) {
            if (!shm_attach()) { usleep(100000); continue; }
        }

        ShmRing *ring = g_shm;
        uint32_t wp   = atomic_load_explicit(&ring->wp, memory_order_acquire);
        uint32_t rp   = atomic_load_explicit(&ring->rp, memory_order_relaxed);

        if ((int32_t)(wp - rp) < PERIOD_FRAMES) {
            usleep(1000);
            continue;
        }

        /* SHM → in_buf */
        for (int f = 0; f < PERIOD_FRAMES; f++) {
            uint32_t idx = (rp + (uint32_t)f) % SHM_RING_FRAMES;
            for (int c = 0; c < g_ch && c < SHM_MAX_CH; c++)
                in_buf[f * g_ch + c] = ring->buf[idx * SHM_MAX_CH + c];
        }
        atomic_store_explicit(&ring->rp, rp + PERIOD_FRAMES, memory_order_release);

        /* libsamplerate */
        float *send_buf;
        int    send_frames;
        if (need_src) {
            SRC_DATA sd = {
                .data_in       = in_buf,
                .data_out      = out_buf,
                .input_frames  = PERIOD_FRAMES,
                .output_frames = out_max,
                .src_ratio     = (double)out_rate / SAMPLE_RATE,
                .end_of_input  = 0,
            };
            src_process(g_src, &sd);
            send_buf    = out_buf;
            send_frames = (int)sd.output_frames_gen;
        } else {
            send_buf    = in_buf;
            send_frames = PERIOD_FRAMES;
        }

        if (send_frames <= 0 || g_n_targets == 0) continue;

        if (g_codec == CODEC_MP3)
            send_mp3(send_buf, send_frames);
        else
            send_raw_l16(send_buf, send_frames);
    }

    free(in_buf);
    free(out_buf);
    return NULL;
}

/* ── stats thread ────────────────────────────────────── */
static void *stats_thread(void *arg)
{
    (void)arg;
    unsigned long prev = 0;
    while (!g_quit) {
        sleep(2);
        unsigned long cur  = atomic_load(&g_bytes_sent);
        int           kbps = (int)((cur - prev) * 8 / 2 / 1000);
        prev = cur;
        pthread_mutex_lock(&g_target_mtx);
        int nt = g_n_targets;
        pthread_mutex_unlock(&g_target_mtx);
        fprintf(stdout, "stats targets=%d codec=%s bitrateKbps=%d bytesSent=%lu\n",
                nt, g_codec == CODEC_MP3 ? "mp3" : "raw", kbps, cur);
        fflush(stdout);
    }
    return NULL;
}

/* ── stdin command thread ────────────────────────────── */
static void *stdin_thread(void *arg)
{
    (void)arg;
    char line[256];
    while (!g_quit && fgets(line, sizeof(line), stdin) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        char cmd[64]; int p = 0; char h[128];
        if (sscanf(line, "%63s", cmd) < 1) continue;

        if (strcmp(cmd, "quit") == 0) { g_quit = 1; break; }
        if (strcmp(cmd, "add") == 0 && sscanf(line, "%*s %127s %d", h, &p) == 2) {
            pthread_mutex_lock(&g_target_mtx);
            int found = 0;
            for (int i = 0; i < g_n_targets; i++)
                if (strcmp(g_targets[i].host, h) == 0 && g_targets[i].port == p)
                    { found = 1; break; }
            if (!found && g_n_targets < MAX_TARGETS) {
                Target *t = &g_targets[g_n_targets++];
                snprintf(t->host, sizeof(t->host), "%s", h);
                t->port = p;
                memset(&t->addr, 0, sizeof(t->addr));
                t->addr.sin_family = AF_INET;
                t->addr.sin_port   = htons(p);
                inet_aton(h, &t->addr.sin_addr);
            }
            int nt = g_n_targets;
            pthread_mutex_unlock(&g_target_mtx);
            fprintf(stderr, "[rtp_send] add target %s:%d (total=%d)\n", h, p, nt);
            continue;
        }

        if (strcmp(cmd, "remove") == 0 && sscanf(line, "%*s %127s %d", h, &p) == 2) {
            pthread_mutex_lock(&g_target_mtx);
            for (int i = 0; i < g_n_targets; i++) {
                if (strcmp(g_targets[i].host, h) == 0 && g_targets[i].port == p) {
                    g_targets[i] = g_targets[--g_n_targets];
                    break;
                }
            }
            int nt = g_n_targets;
            pthread_mutex_unlock(&g_target_mtx);
            fprintf(stderr, "[rtp_send] remove target %s:%d (total=%d)\n", h, p, nt);
            continue;
        }

        if (strcmp(cmd, "codec") == 0) {
            char cs[32] = "mp3"; int br = g_bitrate;
            sscanf(line, "%*s %31s %d", cs, &br);
            Codec nc = (strcmp(cs, "raw") == 0) ? CODEC_RAW : CODEC_MP3;
            pthread_mutex_lock(&g_codec_mtx);
            g_new_codec     = nc;
            g_new_bitrate   = br;
            g_codec_changed = 1;
            pthread_mutex_unlock(&g_codec_mtx);
            continue;
        }

        fprintf(stderr, "[rtp_send] unknown command: %s\n", cmd);
    }
    /* stdin EOF = 부모 프로세스(Node.js)가 종료됨 → 정상 종료 */
    g_quit = 1;
    return NULL;
}

/* ── signal ──────────────────────────────────────────── */
static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ── main ────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);  /* stdout/UDP 파이프 broken 시 크래시 방지 */
    if (mlockall(MCL_CURRENT) != 0)
        fprintf(stderr, "[rtp_send] mlockall failed: %s\n", strerror(errno));

    g_ch       = argc > 1 ? atoi(argv[1]) : 2;
    if (argc > 2) snprintf(g_client_name, sizeof(g_client_name), "%s", argv[2]);
    g_use_rtp  = (argc > 3 && strcmp(argv[3], "rtp") == 0) ? 1 : 0;
    g_out_rate = (argc > 4 && atoi(argv[4]) > 0) ? atoi(argv[4]) : SAMPLE_RATE;
    if (argc > 5 && strcmp(argv[5], "shm") == 0 && argc > 6)
        snprintf(g_shm_name, sizeof(g_shm_name), "%s", argv[6]);
    if (argc > 7) g_codec   = (strcmp(argv[7], "raw") == 0) ? CODEC_RAW : CODEC_MP3;
    if (argc > 8 && atoi(argv[8]) > 0) g_bitrate = atoi(argv[8]);

    if (!g_shm_name[0]) {
        fprintf(stderr, "[rtp_send] shm name required\n");
        return 1;
    }

    /* SSRC: 클라이언트 이름 해시 (재시작 후에도 동일) */
    for (int i = 0; g_client_name[i]; i++)
        g_rtp_ssrc = g_rtp_ssrc * 31u + (unsigned char)g_client_name[i];
    g_rtp_ssrc |= 0x80000000u;

    /* libsamplerate */
    if (g_out_rate != SAMPLE_RATE) {
        int err;
        g_src = src_new(SRC_SINC_FASTEST, g_ch, &err);
        if (!g_src) {
            fprintf(stderr, "[rtp_send] src_new: %s\n", src_strerror(err));
            return 1;
        }
        fprintf(stderr, "[rtp_send] resampler: %d→%d\n", SAMPLE_RATE, g_out_rate);
    }

    /* lame: mp3 코덱일 때만 초기화 */
    if (g_codec == CODEC_MP3) {
        if (!lame_reinit(g_out_rate, g_ch, g_bitrate)) {
            fprintf(stderr, "[rtp_send] lame init failed\n");
            return 1;
        }
    }

    /* UDP 소켓 */
    if (!udp_init()) return 1;

    shm_attach();

    g_reader_run = 1;
    pthread_create(&g_reader_tid, NULL, shm_reader_thread, NULL);

    fprintf(stdout, "[rtp_send] ready\n");
    fflush(stdout);

    pthread_t stdin_tid, stats_tid;
    pthread_create(&stdin_tid, NULL, stdin_thread, NULL);
    pthread_create(&stats_tid, NULL, stats_thread,  NULL);

    while (!g_quit) usleep(50000);

    g_reader_run = 0;
    pthread_join(g_reader_tid, NULL);
    pthread_join(stdin_tid,    NULL);
    pthread_join(stats_tid,    NULL);

    pthread_mutex_lock(&g_lame_mtx);
    if (g_lame) { lame_close(g_lame); g_lame = NULL; }
    pthread_mutex_unlock(&g_lame_mtx);

    if (g_src)      { src_delete(g_src); g_src = NULL; }
    if (g_udp_sock >= 0) { close(g_udp_sock); g_udp_sock = -1; }
    if (g_shm)      { munmap(g_shm, SHMRING_SIZE); g_shm = NULL; }
    if (g_shm_fd >= 0) { close(g_shm_fd); g_shm_fd = -1; }

    fprintf(stderr, "[rtp_send] exiting\n");
    return 0;
}
