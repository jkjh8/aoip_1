/*
 * rtp_send.c — Lightweight RTP/UDP sender (context-based, no main)
 *
 * Compiled into aoip_engine. Entry points:
 *   rtp_send_start(ring, key, sock_path) → RtpSendCtx *
 *   rtp_send_stop(ctx)
 *
 * Flow:
 *   SHM(F32LE@48k) → libsamplerate → lame/L16 → RTP 패킷 → UDP sendto()
 *
 * Connects to Node.js Unix socket for initial config, commands, and stats.
 */
#define _GNU_SOURCE
#include <samplerate.h>
#include <lame/lame.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>
#include <math.h>

#include "include/ring_buf.h"
#include "include/rtp_send.h"

/* ── constants ───────────────────────────────────────── */
#define RS_MAX_TARGETS   16
#define RS_PERIOD_FRAMES 512
#define RS_SAMPLE_RATE   48000
#define LAME_GRAN        1152
#define MP3_ACC_MAX      (LAME_GRAN * 4)
#define MP3_BUF_SIZE     (LAME_GRAN * 5 / 4 + 7200)
#define RTP_HDR_SIZE     12
#define MPA_HDR_SIZE     4
#define MAX_PKT_SIZE     1472
#define L16_FRAMES_PER_PKT 256

/* ── types ───────────────────────────────────────────── */
typedef enum { CODEC_MP3, CODEC_RAW } RsCodec;

typedef struct {
    char             host[128];
    int              port;
    struct sockaddr_in addr;
} RsTarget;

struct RtpSendCtx {
    /* config */
    int     ch, out_rate, bitrate;
    int     use_rtp;
    RsCodec codec;
    char    key[64];
    int     sock_fd;   /* Unix socket to Node.js (bidirectional) */
    int     udp_sock;

    /* ring buffer (owned by engine) */
    RingBuf *ring;

    /* targets */
    RsTarget        targets[RS_MAX_TARGETS];
    int             n_targets;
    pthread_mutex_t target_mtx;

    /* lame */
    lame_t          lame;
    pthread_mutex_t lame_mtx;

    /* codec change flag */
    volatile int    codec_changed;
    RsCodec         new_codec;
    int             new_bitrate;
    pthread_mutex_t codec_mtx;

    /* MP3 accumulation buffer */
    float    mp3_acc[MP3_ACC_MAX * 2];
    int      mp3_acc_frames;

    /* RTP state */
    uint16_t rtp_seq;
    uint32_t rtp_ts;
    uint32_t rtp_ssrc;

    /* libsamplerate */
    SRC_STATE *src;

    /* stats */
    atomic_ulong bytes_sent;

    /* state */
    volatile int quit;
    volatile int reader_run;
    int prio;

    /* threads */
    pthread_t reader_tid, stdin_tid, stats_tid;
};

/* ── helpers ─────────────────────────────────────────── */
static int rs_unix_connect(const char *path, int retries, int ms)
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

static int rs_read_line(int fd, char *buf, int maxlen)
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

static int rs_cfg_int(const char *s, const char *key, int def)
{
    char pat[64]; int v = def;
    snprintf(pat, sizeof(pat), "%s=%%d", key);
    const char *p = strstr(s, key);
    if (p) sscanf(p, pat, &v);
    return v;
}

static void rs_cfg_str(const char *s, const char *key, char *buf, size_t n, const char *def)
{
    strncpy(buf, def, n); buf[n-1] = '\0';
    char pat[64]; snprintf(pat, sizeof(pat), "%s=%%%zus", key, n-1);
    const char *p = strstr(s, key);
    if (p) sscanf(p, pat, buf);
}

/* ── lame init ───────────────────────────────────────── */
static int lame_reinit(RtpSendCtx *ctx)
{
    if (ctx->lame) { lame_close(ctx->lame); ctx->lame = NULL; }
    ctx->lame = lame_init();
    if (!ctx->lame) return 0;
    lame_set_in_samplerate(ctx->lame, ctx->out_rate);
    lame_set_num_channels(ctx->lame, ctx->ch);
    lame_set_out_samplerate(ctx->lame, ctx->out_rate);
    lame_set_brate(ctx->lame, ctx->bitrate);
    lame_set_quality(ctx->lame, 7);
    lame_set_VBR(ctx->lame, vbr_off);
    lame_set_bWriteVbrTag(ctx->lame, 0);
    lame_set_disable_reservoir(ctx->lame, 1);
    if (lame_init_params(ctx->lame) < 0) {
        lame_close(ctx->lame); ctx->lame = NULL; return 0;
    }
    fprintf(stderr, "[rtp_send:%s] lame: %dHz %dch %dkbps\n",
            ctx->key, ctx->out_rate, ctx->ch, ctx->bitrate);
    return 1;
}

/* ── UDP socket init ─────────────────────────────────── */
static int udp_init(RtpSendCtx *ctx)
{
    if (ctx->udp_sock >= 0) { close(ctx->udp_sock); ctx->udp_sock = -1; }
    ctx->udp_sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (ctx->udp_sock < 0) {
        fprintf(stderr, "[rtp_send:%s] socket: %s\n", ctx->key, strerror(errno));
        return 0;
    }
    int ttl = 15;
    setsockopt(ctx->udp_sock, IPPROTO_IP, IP_MULTICAST_TTL, &ttl, sizeof(ttl));
    setsockopt(ctx->udp_sock, IPPROTO_IP, IP_TTL,           &ttl, sizeof(ttl));
    return 1;
}

/* ── RTP packet send ─────────────────────────────────── */
static void rtp_send_packet(RtpSendCtx *ctx, const uint8_t *payload, int payload_len, int marker)
{
    if (ctx->udp_sock < 0 || payload_len <= 0) return;
    uint8_t pt;
    if (ctx->codec == CODEC_MP3) {
        pt = 14;
    } else {
        pt = (ctx->ch == 1) ? 11 : 10;
        if (ctx->out_rate != 44100) pt = 96;
    }
    uint8_t pkt[RTP_HDR_SIZE + MPA_HDR_SIZE + MAX_PKT_SIZE];
    int hdr_extra = (ctx->codec == CODEC_MP3) ? MPA_HDR_SIZE : 0;
    int pkt_len   = RTP_HDR_SIZE + hdr_extra + payload_len;
    pkt[0]  = 0x80;
    pkt[1]  = (marker ? 0x80 : 0) | (pt & 0x7f);
    pkt[2]  = ctx->rtp_seq >> 8;
    pkt[3]  = ctx->rtp_seq & 0xff;
    pkt[4]  = ctx->rtp_ts >> 24;
    pkt[5]  = (ctx->rtp_ts >> 16) & 0xff;
    pkt[6]  = (ctx->rtp_ts >> 8)  & 0xff;
    pkt[7]  = ctx->rtp_ts & 0xff;
    pkt[8]  = ctx->rtp_ssrc >> 24;
    pkt[9]  = (ctx->rtp_ssrc >> 16) & 0xff;
    pkt[10] = (ctx->rtp_ssrc >> 8)  & 0xff;
    pkt[11] = ctx->rtp_ssrc & 0xff;
    ctx->rtp_seq++;
    if (hdr_extra) memset(pkt + RTP_HDR_SIZE, 0, hdr_extra);
    memcpy(pkt + RTP_HDR_SIZE + hdr_extra, payload, payload_len);

    pthread_mutex_lock(&ctx->target_mtx);
    for (int i = 0; i < ctx->n_targets; i++) {
        ssize_t sent = sendto(ctx->udp_sock, pkt, pkt_len, 0,
                              (struct sockaddr *)&ctx->targets[i].addr,
                              sizeof(ctx->targets[i].addr));
        if (sent > 0)
            atomic_fetch_add_explicit(&ctx->bytes_sent, (unsigned long)sent, memory_order_relaxed);
    }
    pthread_mutex_unlock(&ctx->target_mtx);
}

/* ── float → S16BE ───────────────────────────────────── */
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

/* ── send L16 ────────────────────────────────────────── */
static void send_raw_l16(RtpSendCtx *ctx, const float *buf, int frames)
{
    int frames_sent = 0;
    while (frames_sent < frames) {
        int chunk = frames - frames_sent;
        if (chunk > L16_FRAMES_PER_PKT) chunk = L16_FRAMES_PER_PKT;
        int samples = chunk * ctx->ch;
        uint8_t pcm[L16_FRAMES_PER_PKT * 8 * 2];
        f32_to_s16be(buf + frames_sent * ctx->ch, pcm, samples);
        rtp_send_packet(ctx, pcm, samples * 2, (frames_sent == 0) ? 1 : 0);
        ctx->rtp_ts += (uint32_t)chunk;
        frames_sent += chunk;
    }
}

/* ── send MP3 ────────────────────────────────────────── */
static void send_mp3(RtpSendCtx *ctx, const float *buf, int frames)
{
    int space = MP3_ACC_MAX - ctx->mp3_acc_frames;
    if (frames > space) frames = space;
    memcpy(ctx->mp3_acc + ctx->mp3_acc_frames * ctx->ch, buf,
           (size_t)(frames * ctx->ch) * sizeof(float));
    ctx->mp3_acc_frames += frames;

    int ch2 = (ctx->ch >= 2);
    float left[LAME_GRAN], right[LAME_GRAN];
    while (ctx->mp3_acc_frames >= LAME_GRAN) {
        const float *src = ctx->mp3_acc;
        for (int f = 0; f < LAME_GRAN; f++) {
            left[f]  = src[f * ctx->ch];
            right[f] = ch2 ? src[f * ctx->ch + 1] : src[f * ctx->ch];
        }
        uint8_t mp3buf[MP3_BUF_SIZE];
        int mp3len = 0;
        pthread_mutex_lock(&ctx->lame_mtx);
        if (ctx->lame)
            mp3len = lame_encode_buffer_ieee_float(
                         ctx->lame, left, right, LAME_GRAN, mp3buf, sizeof(mp3buf));
        pthread_mutex_unlock(&ctx->lame_mtx);
        ctx->mp3_acc_frames -= LAME_GRAN;
        if (ctx->mp3_acc_frames > 0)
            memmove(ctx->mp3_acc, ctx->mp3_acc + LAME_GRAN * ctx->ch,
                    (size_t)(ctx->mp3_acc_frames * ctx->ch) * sizeof(float));
        if (mp3len <= 0) continue;
        uint32_t ts_inc = (uint32_t)((uint64_t)LAME_GRAN * 90000 / (uint64_t)ctx->out_rate);
        rtp_send_packet(ctx, mp3buf, mp3len, 1);
        ctx->rtp_ts += ts_inc;
    }
}

/* ── shm reader thread ───────────────────────────────── */
static void *shm_reader_thread(void *arg)
{
    RtpSendCtx *ctx = (RtpSendCtx *)arg;
    if (ctx->prio > 0) {
        struct sched_param sp = { .sched_priority = ctx->prio };
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
    int out_rate  = ctx->out_rate;
    int need_src  = (out_rate != RS_SAMPLE_RATE) && (ctx->src != NULL);

    float * volatile in_buf  = calloc(RS_PERIOD_FRAMES * ctx->ch, sizeof(float));
    int out_max              = need_src
        ? (int)((double)RS_PERIOD_FRAMES * out_rate / RS_SAMPLE_RATE * 1.1 + 64)
        : RS_PERIOD_FRAMES;
    float * volatile out_buf = calloc(out_max * ctx->ch, sizeof(float));

    if (!in_buf || !out_buf) {
        fprintf(stderr, "[rtp_send:%s] calloc failed\n", ctx->key);
        free(in_buf); free(out_buf);
        ctx->quit = 1;
        return NULL;
    }

    while (ctx->reader_run) {
        if (ctx->codec_changed) {
            pthread_mutex_lock(&ctx->codec_mtx);
            RsCodec nc = ctx->new_codec;
            int     nb = ctx->new_bitrate;
            ctx->codec_changed = 0;
            pthread_mutex_unlock(&ctx->codec_mtx);
            ctx->codec   = nc;
            ctx->bitrate = nb;
            ctx->mp3_acc_frames = 0;
            if (ctx->codec == CODEC_MP3) {
                pthread_mutex_lock(&ctx->lame_mtx);
                lame_reinit(ctx);
                pthread_mutex_unlock(&ctx->lame_mtx);
            }
            fprintf(stderr, "[rtp_send:%s] codec=%s bitrate=%d\n", ctx->key,
                    ctx->codec == CODEC_MP3 ? "mp3" : "raw", ctx->bitrate);
        }

        if (!ctx->ring) { usleep(10000); continue; }

        if (rb_avail(ctx->ring) < RS_PERIOD_FRAMES) {
            usleep(1000);
            continue;
        }
        rb_read(ctx->ring, in_buf, RS_PERIOD_FRAMES);

        float *send_buf;
        int    send_frames;
        if (need_src) {
            SRC_DATA sd = {
                .data_in       = in_buf,
                .data_out      = out_buf,
                .input_frames  = RS_PERIOD_FRAMES,
                .output_frames = out_max,
                .src_ratio     = (double)out_rate / RS_SAMPLE_RATE,
                .end_of_input  = 0,
            };
            src_process(ctx->src, &sd);
            send_buf    = out_buf;
            send_frames = (int)sd.output_frames_gen;
        } else {
            send_buf    = in_buf;
            send_frames = RS_PERIOD_FRAMES;
        }

        if (send_frames <= 0 || ctx->n_targets == 0) continue;

        if (ctx->codec == CODEC_MP3)
            send_mp3(ctx, send_buf, send_frames);
        else
            send_raw_l16(ctx, send_buf, send_frames);
    }

    free(in_buf);
    free(out_buf);
    return NULL;
}

/* ── stats thread ────────────────────────────────────── */
static void *rs_stats_thread(void *arg)
{
    RtpSendCtx *ctx = (RtpSendCtx *)arg;
    unsigned long prev = 0;
    while (!ctx->quit) {
        sleep(2);
        unsigned long cur  = atomic_load(&ctx->bytes_sent);
        int           kbps = (int)((cur - prev) * 8 / 2 / 1000);
        prev = cur;
        pthread_mutex_lock(&ctx->target_mtx);
        int nt = ctx->n_targets;
        pthread_mutex_unlock(&ctx->target_mtx);
        dprintf(ctx->sock_fd,
                "stats targets=%d codec=%s bitrateKbps=%d bytesSent=%lu\n",
                nt, ctx->codec == CODEC_MP3 ? "mp3" : "raw", kbps, cur);
    }
    return NULL;
}

/* ── stdin (command) thread ──────────────────────────── */
static void *rs_stdin_thread(void *arg)
{
    RtpSendCtx *ctx = (RtpSendCtx *)arg;
    char line[256];
    FILE *f = fdopen(dup(ctx->sock_fd), "r");
    if (!f) { ctx->quit = 1; return NULL; }

    while (!ctx->quit && fgets(line, sizeof(line), f) != NULL) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        char cmd[64]; int p = 0; char h[128];
        if (sscanf(line, "%63s", cmd) < 1) continue;

        if (strcmp(cmd, "quit") == 0) { ctx->quit = 1; break; }

        if (strcmp(cmd, "add") == 0 && sscanf(line, "%*s %127s %d", h, &p) == 2) {
            pthread_mutex_lock(&ctx->target_mtx);
            int found = 0;
            for (int i = 0; i < ctx->n_targets; i++)
                if (strcmp(ctx->targets[i].host, h) == 0 && ctx->targets[i].port == p)
                    { found = 1; break; }
            if (!found && ctx->n_targets < RS_MAX_TARGETS) {
                RsTarget *t = &ctx->targets[ctx->n_targets++];
                snprintf(t->host, sizeof(t->host), "%s", h);
                t->port = p;
                memset(&t->addr, 0, sizeof(t->addr));
                t->addr.sin_family = AF_INET;
                t->addr.sin_port   = htons(p);
                inet_aton(h, &t->addr.sin_addr);
            }
            int nt = ctx->n_targets;
            pthread_mutex_unlock(&ctx->target_mtx);
            fprintf(stderr, "[rtp_send:%s] add target %s:%d (total=%d)\n", ctx->key, h, p, nt);
            continue;
        }

        if (strcmp(cmd, "remove") == 0 && sscanf(line, "%*s %127s %d", h, &p) == 2) {
            pthread_mutex_lock(&ctx->target_mtx);
            for (int i = 0; i < ctx->n_targets; i++) {
                if (strcmp(ctx->targets[i].host, h) == 0 && ctx->targets[i].port == p) {
                    ctx->targets[i] = ctx->targets[--ctx->n_targets];
                    break;
                }
            }
            int nt = ctx->n_targets;
            pthread_mutex_unlock(&ctx->target_mtx);
            fprintf(stderr, "[rtp_send:%s] remove target %s:%d (total=%d)\n", ctx->key, h, p, nt);
            continue;
        }

        if (strcmp(cmd, "codec") == 0) {
            char cs[32] = "mp3"; int br = ctx->bitrate;
            sscanf(line, "%*s %31s %d", cs, &br);
            RsCodec nc = (strcmp(cs, "raw") == 0) ? CODEC_RAW : CODEC_MP3;
            pthread_mutex_lock(&ctx->codec_mtx);
            ctx->new_codec     = nc;
            ctx->new_bitrate   = br;
            ctx->codec_changed = 1;
            pthread_mutex_unlock(&ctx->codec_mtx);
            continue;
        }
    }
    fclose(f);
    ctx->quit = 1;
    return NULL;
}

/* ── public API ──────────────────────────────────────── */
RtpSendCtx *rtp_send_start(RingBuf *ring, const char *key, const char *sock_path, int prio)
{
    int sfd = rs_unix_connect(sock_path, 50, 200);
    if (sfd < 0) {
        fprintf(stderr, "[rtp_send:%s] cannot connect to %s\n", key, sock_path);
        return NULL;
    }

    RtpSendCtx *ctx = calloc(1, sizeof(RtpSendCtx));
    if (!ctx) { close(sfd); return NULL; }

    ctx->ring     = ring;
    ctx->sock_fd  = sfd;
    ctx->udp_sock = -1;
    ctx->prio     = prio;
    snprintf(ctx->key, sizeof(ctx->key), "%s", key);

    pthread_mutex_init(&ctx->target_mtx, NULL);
    pthread_mutex_init(&ctx->lame_mtx,   NULL);
    pthread_mutex_init(&ctx->codec_mtx,  NULL);

    /* Read config from Node.js */
    char cfg[512] = "";
    rs_read_line(sfd, cfg, sizeof(cfg));
    ctx->ch = rs_cfg_int(cfg, "channels", 2);
    char proto_str[16], codec_str[16];
    rs_cfg_str(cfg, "proto",  proto_str,  sizeof(proto_str),  "rtp");
    rs_cfg_str(cfg, "codec",  codec_str,  sizeof(codec_str),  "raw");
    ctx->use_rtp  = (strcmp(proto_str, "rtp") == 0) ? 1 : 0;
    ctx->codec    = (strcmp(codec_str, "raw") == 0) ? CODEC_RAW : CODEC_MP3;
    ctx->out_rate = rs_cfg_int(cfg, "rate",    RS_SAMPLE_RATE);
    ctx->bitrate  = rs_cfg_int(cfg, "bitrate", 320);
    if (ctx->out_rate <= 0) ctx->out_rate = RS_SAMPLE_RATE;
    if (ctx->bitrate  <= 0) ctx->bitrate  = 320;

    /* SSRC from key hash */
    for (int i = 0; key[i]; i++)
        ctx->rtp_ssrc = ctx->rtp_ssrc * 31u + (unsigned char)key[i];
    ctx->rtp_ssrc |= 0x80000000u;

    /* libsamplerate */
    if (ctx->out_rate != RS_SAMPLE_RATE) {
        int err;
        ctx->src = src_new(SRC_SINC_FASTEST, ctx->ch, &err);
        if (!ctx->src) {
            fprintf(stderr, "[rtp_send:%s] src_new: %s\n", key, src_strerror(err));
            free(ctx); close(sfd);
            return NULL;
        }
        fprintf(stderr, "[rtp_send:%s] resampler: %d→%d\n", key, RS_SAMPLE_RATE, ctx->out_rate);
    }

    /* lame */
    if (ctx->codec == CODEC_MP3) {
        if (!lame_reinit(ctx)) {
            fprintf(stderr, "[rtp_send:%s] lame init failed\n", key);
            if (ctx->src) src_delete(ctx->src);
            free(ctx); close(sfd);
            return NULL;
        }
    }

    if (!udp_init(ctx)) {
        if (ctx->lame) lame_close(ctx->lame);
        if (ctx->src)  src_delete(ctx->src);
        free(ctx); close(sfd);
        return NULL;
    }

    ctx->reader_run = 1;
    pthread_create(&ctx->reader_tid, NULL, shm_reader_thread, ctx);

    dprintf(sfd, "[rtp_send] ready\n");
    fprintf(stderr, "[rtp_send:%s] started\n", key);

    pthread_create(&ctx->stdin_tid, NULL, rs_stdin_thread, ctx);
    pthread_create(&ctx->stats_tid, NULL, rs_stats_thread, ctx);

    return ctx;
}

void rtp_send_stop(RtpSendCtx *ctx)
{
    if (!ctx) return;
    ctx->quit       = 1;
    ctx->reader_run = 0;
    pthread_join(ctx->reader_tid, NULL);
    pthread_join(ctx->stdin_tid,  NULL);
    pthread_join(ctx->stats_tid,  NULL);

    pthread_mutex_lock(&ctx->lame_mtx);
    if (ctx->lame) { lame_close(ctx->lame); ctx->lame = NULL; }
    pthread_mutex_unlock(&ctx->lame_mtx);

    if (ctx->src)      { src_delete(ctx->src); ctx->src = NULL; }
    if (ctx->udp_sock >= 0) { close(ctx->udp_sock); ctx->udp_sock = -1; }
    if (ctx->sock_fd  >= 0) { close(ctx->sock_fd);  ctx->sock_fd  = -1; }

    pthread_mutex_destroy(&ctx->target_mtx);
    pthread_mutex_destroy(&ctx->lame_mtx);
    pthread_mutex_destroy(&ctx->codec_mtx);
    free(ctx);
}
