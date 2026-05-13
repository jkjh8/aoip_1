/*
 * rtp_recv.c — Lightweight UDP/RTP receiver (context-based, no main)
 *
 * Compiled into aoip_engine. Entry points:
 *   rtp_recv_start(ring, key, sock_path) → RtpRecvCtx *
 *   rtp_recv_stop(ctx)
 *
 * Flow:
 *   UDP socket → packet queue → decode thread → ShmRing(F32LE@48kHz)
 *
 * Connects to Node.js Unix socket for initial config and periodic stats.
 */
#define _GNU_SOURCE
#include <lame/lame.h>
#include <samplerate.h>

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

#include "include/engine_constants.h"
#include "include/ring_buf.h"
#include "include/rtp_recv.h"

/* ── constants ───────────────────────────────────────── */
#define RTP_HDR_MIN      12
#define MAX_PKT_LEN      8192
#define PKT_QUEUE        512
#define PKT_QUEUE_MASK   (PKT_QUEUE - 1)
#define OUT_RATE         48000
#define RESAMPLE_OUT_MAX 8192
#define DECODE_BUF_MAX   8192

/* ── types ───────────────────────────────────────────── */
typedef enum { PROTO_RTP = 0, PROTO_RAW } ProtoMode;
typedef enum {
    ENC_UNKNOWN = 0, ENC_L16, ENC_L24, ENC_MPA, ENC_PCMU, ENC_PCMA
} EncMode;

typedef struct { uint8_t data[MAX_PKT_LEN]; int len; } RtpPkt;

struct RtpRecvCtx {
    /* config */
    int       ch, buf_ms, in_rate;
    ProtoMode proto;
    EncMode   enc;
    char      bind_addr[64];
    char      key[64];
    int       sock_fd;   /* Unix socket to Node.js (stats output) */
    int       udp_sock;

    /* ring buffer (owned by engine) */
    RingBuf  *ring;

    /* state */
    volatile int quit;
    int prio;
    volatile int detected;
    uint8_t   last_rtp_pt;
    char      codec_str[32];

    /* stats */
    atomic_ulong packets, drops, udp_bytes;
    char         src_ip[64];
    int          src_port;
    pthread_mutex_t addr_mtx;

    /* decoders */
    hip_t      hip;
    SRC_STATE *src_state;
    int16_t    ulaw_table[256];
    int16_t    alaw_table[256];

    /* decode/resample buffers */
    float rs_out[RESAMPLE_OUT_MAX * MAX_CH];
    float dec_buf[DECODE_BUF_MAX  * MAX_CH];

    /* packet queue */
    RtpPkt         *pkts;
    int             pkt_wp, pkt_rp;
    pthread_mutex_t pkt_mtx;
    pthread_cond_t  pkt_cond;

    /* threads */
    pthread_t recv_tid, decode_tid, stats_tid;
};

/* ── helpers ─────────────────────────────────────────── */
static int rr_unix_connect(const char *path, int retries, int ms)
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

static int rr_read_line(int fd, char *buf, int maxlen)
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

static int rr_cfg_int(const char *s, const char *key, int def)
{
    char pat[64]; int v = def;
    snprintf(pat, sizeof(pat), "%s=%%d", key);
    const char *p = strstr(s, key);
    if (p) sscanf(p, pat, &v);
    return v;
}

static void rr_cfg_str(const char *s, const char *key, char *buf, size_t n, const char *def)
{
    strncpy(buf, def, n); buf[n-1] = '\0';
    char pat[64]; snprintf(pat, sizeof(pat), "%s=%%%zus", key, n-1);
    const char *p = strstr(s, key);
    if (p) sscanf(p, pat, buf);
}

/* ── G.711 table init ────────────────────────────────── */
static void build_g711_tables(RtpRecvCtx *ctx)
{
    for (int i = 0; i < 256; i++) {
        int u        = ~i & 0xFF;
        int sign     = (u & 0x80) ? -1 : 1;
        int exponent = (u >> 4) & 0x07;
        int mantissa = u & 0x0F;
        int sample   = ((mantissa << 1) | 1) << (exponent + 2);
        ctx->ulaw_table[i] = (int16_t)(sign * (sample - 33));
    }
    for (int i = 0; i < 256; i++) {
        int a        = i ^ 0x55;
        int sign     = (a & 0x80) ? 1 : -1;
        int exponent = (a >> 4) & 0x07;
        int mantissa = a & 0x0F;
        int sample   = (exponent == 0)
                       ? ((mantissa << 1) | 1)
                       : ((mantissa | 0x10) << exponent);
        ctx->alaw_table[i] = (int16_t)(sign * sample * 8);
    }
}

/* ── write F32 frames to ring ────────────────────────── */
static void ring_write(RtpRecvCtx *ctx, const float *buf, int frames)
{
    if (!ctx->ring || frames <= 0) return;
    rb_write(ctx->ring, buf, frames);
}

/* ── resample + write ────────────────────────────────── */
static void resample_and_write(RtpRecvCtx *ctx, const float *in, int in_frames)
{
    if (!ctx->src_state) {
        ring_write(ctx, in, in_frames);
        return;
    }
    SRC_DATA sd = {
        .data_in       = in,
        .data_out      = ctx->rs_out,
        .input_frames  = in_frames,
        .output_frames = RESAMPLE_OUT_MAX,
        .src_ratio     = (double)OUT_RATE / ctx->in_rate,
        .end_of_input  = 0,
    };
    src_process(ctx->src_state, &sd);
    ring_write(ctx, ctx->rs_out, (int)sd.output_frames_gen);
}

/* ── setup resampler ─────────────────────────────────── */
static void setup_resampler(RtpRecvCtx *ctx)
{
    if (ctx->src_state) { src_delete(ctx->src_state); ctx->src_state = NULL; }
    if (ctx->in_rate == OUT_RATE) return;
    int err;
    ctx->src_state = src_new(SRC_SINC_FASTEST, ctx->ch, &err);
    if (!ctx->src_state)
        fprintf(stderr, "[rtp_recv:%s] src_new: %s\n", ctx->key, src_strerror(err));
    else
        fprintf(stderr, "[rtp_recv:%s] resampler: %d→%d\n", ctx->key, ctx->in_rate, OUT_RATE);
}

/* ── detect encoding from RTP PT ─────────────────────── */
static void detect_rtp_pt(RtpRecvCtx *ctx, uint8_t pt, const uint8_t *payload, int plen)
{
    switch (pt) {
    case 0:  ctx->enc = ENC_PCMU; ctx->in_rate = 8000;
             snprintf(ctx->codec_str, sizeof(ctx->codec_str), "PCMU"); break;
    case 8:  ctx->enc = ENC_PCMA; ctx->in_rate = 8000;
             snprintf(ctx->codec_str, sizeof(ctx->codec_str), "PCMA"); break;
    case 10: ctx->enc = ENC_L16;  ctx->in_rate = 44100;
             snprintf(ctx->codec_str, sizeof(ctx->codec_str), "L16");  break;
    case 11: ctx->enc = ENC_L16;  ctx->in_rate = 44100;
             snprintf(ctx->codec_str, sizeof(ctx->codec_str), "L16");  break;
    case 14: ctx->enc = ENC_MPA;  ctx->in_rate = 48000;
             snprintf(ctx->codec_str, sizeof(ctx->codec_str), "MPA");
             if (ctx->hip) { hip_decode_exit(ctx->hip); }
             ctx->hip = hip_decode_init();
             break;
    default:
        if (plen >= 6) {
            const uint8_t *p = payload;
            int off = (plen >= 4) ? 4 : 0;
            if ((p[off] & 0xFF) == 0xFF && (p[off+1] & 0xE0) == 0xE0) {
                ctx->enc = ENC_MPA; ctx->in_rate = 48000;
                snprintf(ctx->codec_str, sizeof(ctx->codec_str), "MPA");
                break;
            }
        }
        if (plen > 4000) {
            ctx->enc = ENC_L24;
            snprintf(ctx->codec_str, sizeof(ctx->codec_str), "L24");
        } else {
            ctx->enc = ENC_L16;
            snprintf(ctx->codec_str, sizeof(ctx->codec_str), "L16");
        }
        break;
    }
    fprintf(stderr, "[rtp_recv:%s] RTP PT=%d → enc=%s rate=%d\n",
            ctx->key, pt, ctx->codec_str, ctx->in_rate);
    setup_resampler(ctx);
    ctx->detected = 1;
}

/* ── detect encoding from raw UDP payload ────────────── */
static void detect_raw(RtpRecvCtx *ctx, const uint8_t *payload, int len)
{
    if (len >= 4 && payload[0] == 0xFF && (payload[1] & 0xE0) == 0xE0) {
        ctx->enc = ENC_MPA;
        snprintf(ctx->codec_str, sizeof(ctx->codec_str), "mp3");
        if (ctx->hip) { hip_decode_exit(ctx->hip); }
        ctx->hip = hip_decode_init();
        fprintf(stderr, "[rtp_recv:%s] raw: detected MP3\n", ctx->key);
    } else if (len > 0 && (len % (ctx->ch * 3)) == 0 && len > 4000) {
        ctx->enc = ENC_L24;
        snprintf(ctx->codec_str, sizeof(ctx->codec_str), "L24");
        fprintf(stderr, "[rtp_recv:%s] raw: detected L24 (payload=%d)\n", ctx->key, len);
    } else {
        ctx->enc = ENC_L16;
        snprintf(ctx->codec_str, sizeof(ctx->codec_str), "L16");
        fprintf(stderr, "[rtp_recv:%s] raw: detected L16 (payload=%d)\n", ctx->key, len);
    }
    setup_resampler(ctx);
    ctx->detected = 1;
}

/* ── decode one payload buffer ───────────────────────── */
static void decode_payload(RtpRecvCtx *ctx, const uint8_t *payload, int len)
{
    switch (ctx->enc) {

    case ENC_L16: {
        int frames = (len / 2) / ctx->ch;
        if (frames * ctx->ch * 2 > (int)sizeof(ctx->dec_buf) / (int)sizeof(float))
            frames = (int)(sizeof(ctx->dec_buf) / sizeof(float)) / ctx->ch;
        for (int i = 0; i < frames * ctx->ch; i++) {
            int16_t s = (int16_t)((payload[i*2] << 8) | payload[i*2+1]);
            ctx->dec_buf[i] = s / 32768.0f;
        }
        resample_and_write(ctx, ctx->dec_buf, frames);
        break;
    }

    case ENC_L24: {
        int frames = (len / 3) / ctx->ch;
        if (frames * ctx->ch > (int)(sizeof(ctx->dec_buf) / sizeof(float)))
            frames = (int)(sizeof(ctx->dec_buf) / sizeof(float)) / ctx->ch;
        for (int i = 0; i < frames * ctx->ch; i++) {
            int32_t s = ((int32_t)(int8_t)payload[i*3]     << 16)
                      | ((int32_t)payload[i*3+1]            <<  8)
                      |  (int32_t)payload[i*3+2];
            ctx->dec_buf[i] = s / 8388608.0f;
        }
        resample_and_write(ctx, ctx->dec_buf, frames);
        break;
    }

    case ENC_PCMU:
    case ENC_PCMA: {
        const int16_t *tbl = (ctx->enc == ENC_PCMU) ? ctx->ulaw_table : ctx->alaw_table;
        int frames = len / ctx->ch;
        if (frames * ctx->ch > (int)(sizeof(ctx->dec_buf) / sizeof(float)))
            frames = (int)(sizeof(ctx->dec_buf) / sizeof(float)) / ctx->ch;
        for (int f = 0; f < frames; f++)
            for (int c = 0; c < ctx->ch; c++)
                ctx->dec_buf[f * ctx->ch + c] = tbl[payload[f * ctx->ch + c]] / 32768.0f;
        resample_and_write(ctx, ctx->dec_buf, frames);
        break;
    }

    case ENC_MPA: {
        if (!ctx->hip) break;
        const uint8_t *mp3data = payload;
        int mp3len = len;
        if (ctx->proto == PROTO_RTP && len >= 4) {
            mp3data += 4;
            mp3len  -= 4;
        }
        if (mp3len <= 0) break;

        static unsigned char s_empty[1] = {0};
        short pcm_l[1152], pcm_r[1152];
        mp3data_struct mp3info;
        unsigned char *feed_buf = (unsigned char *)mp3data;
        size_t         feed_len = (size_t)mp3len;
        int samples;
        while ((samples = hip_decode1_headers(ctx->hip, feed_buf, feed_len,
                                              pcm_l, pcm_r, &mp3info)) >= 0) {
            feed_buf = s_empty;
            feed_len = 0;
            if (samples == 0) break;
            if (mp3info.header_parsed) {
                int hz = mp3info.samplerate;
                if (hz > 0 && hz != ctx->in_rate) {
                    fprintf(stderr, "[rtp_recv:%s] MP3 rate: %d→%d\n", ctx->key, ctx->in_rate, hz);
                    ctx->in_rate = hz;
                    setup_resampler(ctx);
                }
            }
            float *out = ctx->dec_buf;
            if (ctx->ch == 2) {
                for (int i = 0; i < samples; i++) {
                    out[i*2]   = pcm_l[i] / 32768.0f;
                    out[i*2+1] = pcm_r[i] / 32768.0f;
                }
            } else {
                for (int i = 0; i < samples; i++)
                    out[i] = (pcm_l[i] + pcm_r[i]) / 65536.0f;
            }
            resample_and_write(ctx, out, samples);
        }
        break;
    }

    default: break;
    }
}

/* ── decode thread ───────────────────────────────────── */
static void *decode_thread(void *arg)
{
    RtpRecvCtx *ctx = (RtpRecvCtx *)arg;

    while (!ctx->quit) {
        pthread_mutex_lock(&ctx->pkt_mtx);
        while (ctx->pkt_wp == ctx->pkt_rp && !ctx->quit)
            pthread_cond_wait(&ctx->pkt_cond, &ctx->pkt_mtx);
        if (ctx->quit) { pthread_mutex_unlock(&ctx->pkt_mtx); break; }

        RtpPkt tmp = ctx->pkts[ctx->pkt_rp & PKT_QUEUE_MASK];
        ctx->pkt_rp++;
        pthread_mutex_unlock(&ctx->pkt_mtx);

        const uint8_t *data = tmp.data;
        int            dlen = tmp.len;
        const uint8_t *payload = data;
        int            plen    = dlen;

        if (ctx->proto == PROTO_RTP) {
            if (dlen < RTP_HDR_MIN) continue;
            uint8_t pt      = data[1] & 0x7F;
            int     cc      = data[0] & 0x0F;
            int     has_ext = (data[0] >> 4) & 0x1;
            int     hdr     = RTP_HDR_MIN + cc * 4;
            if (has_ext && dlen >= hdr + 4) {
                int ew = ((int)data[hdr+2] << 8) | data[hdr+3];
                hdr += 4 + ew * 4;
            }
            if (hdr >= dlen) continue;
            payload = data + hdr;
            plen    = dlen - hdr;

            if (!ctx->detected)
                detect_rtp_pt(ctx, pt, payload, plen);

            if (ctx->detected && pt != ctx->last_rtp_pt && ctx->last_rtp_pt != 0xFF) {
                fprintf(stderr, "[rtp_recv:%s] PT changed %d→%d\n", ctx->key, ctx->last_rtp_pt, pt);
                ctx->detected = 0;
                detect_rtp_pt(ctx, pt, payload, plen);
            }
            ctx->last_rtp_pt = pt;
        } else {
            if (!ctx->detected) detect_raw(ctx, payload, plen);
        }

        if (!ctx->detected) continue;
        decode_payload(ctx, payload, plen);
        atomic_fetch_add_explicit(&ctx->packets, 1, memory_order_relaxed);
    }
    return NULL;
}

/* ── receive thread ──────────────────────────────────── */
static void *recv_thread(void *arg)
{
    RtpRecvCtx *ctx = (RtpRecvCtx *)arg;
    if (ctx->prio > 0) {
        struct sched_param sp = { .sched_priority = ctx->prio };
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
    uint8_t buf[MAX_PKT_LEN];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    while (!ctx->quit) {
        ssize_t n = recvfrom(ctx->udp_sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK) continue;
            if (!ctx->quit) perror("[rtp_recv] recvfrom");
            break;
        }

        pthread_mutex_lock(&ctx->addr_mtx);
        inet_ntop(AF_INET, &from.sin_addr, ctx->src_ip, sizeof(ctx->src_ip));
        ctx->src_port = ntohs(from.sin_port);
        pthread_mutex_unlock(&ctx->addr_mtx);

        atomic_fetch_add_explicit(&ctx->udp_bytes, (unsigned long)n, memory_order_relaxed);

        pthread_mutex_lock(&ctx->pkt_mtx);
        if ((ctx->pkt_wp - ctx->pkt_rp) >= PKT_QUEUE) {
            atomic_fetch_add_explicit(&ctx->drops, 1, memory_order_relaxed);
            pthread_mutex_unlock(&ctx->pkt_mtx);
        } else {
            RtpPkt *pkt = &ctx->pkts[ctx->pkt_wp & PKT_QUEUE_MASK];
            memcpy(pkt->data, buf, (size_t)n);
            pkt->len = (int)n;
            ctx->pkt_wp++;
            pthread_cond_signal(&ctx->pkt_cond);
            pthread_mutex_unlock(&ctx->pkt_mtx);
        }
    }
    return NULL;
}

/* ── stats thread ────────────────────────────────────── */
static void *stats_thread(void *arg)
{
    RtpRecvCtx *ctx = (RtpRecvCtx *)arg;
    unsigned long prev_bytes = 0;
    while (!ctx->quit) {
        sleep(2);
        unsigned long cur  = atomic_load(&ctx->udp_bytes);
        int kbps = (int)((cur - prev_bytes) * 8 / 2 / 1000);
        int has_data = (cur != prev_bytes);
        if (!has_data) {
            pthread_mutex_lock(&ctx->addr_mtx);
            ctx->src_ip[0] = '\0'; ctx->src_port = 0;
            pthread_mutex_unlock(&ctx->addr_mtx);
        }
        prev_bytes = cur;

        char src_ip[64]; int src_port;
        pthread_mutex_lock(&ctx->addr_mtx);
        snprintf(src_ip, sizeof(src_ip), "%s", ctx->src_ip[0] ? ctx->src_ip : "none");
        src_port = ctx->src_port;
        pthread_mutex_unlock(&ctx->addr_mtx);

        int r = dprintf(ctx->sock_fd,
            "stats codec=%s bufMs=%d packets=%lu drops=%lu srcIp=%s srcPort=%d bitrateKbps=%d\n",
            ctx->codec_str, ctx->buf_ms,
            atomic_load(&ctx->packets), atomic_load(&ctx->drops),
            src_ip, src_port, kbps);
        if (r < 0) { ctx->quit = 1; break; }
    }
    return NULL;
}

/* ── public API ──────────────────────────────────────── */
RtpRecvCtx *rtp_recv_start(RingBuf *ring, const char *key, const char *sock_path, int prio)
{
    int sfd = rr_unix_connect(sock_path, 50, 200);
    if (sfd < 0) {
        fprintf(stderr, "[rtp_recv:%s] cannot connect to %s\n", key, sock_path);
        return NULL;
    }

    RtpRecvCtx *ctx = calloc(1, sizeof(RtpRecvCtx));
    if (!ctx) { close(sfd); return NULL; }
    ctx->pkts = malloc(sizeof(RtpPkt) * PKT_QUEUE);
    if (!ctx->pkts) { free(ctx); close(sfd); return NULL; }
    ctx->prio = prio;

    ctx->ring        = ring;
    ctx->sock_fd     = sfd;
    ctx->last_rtp_pt = 0xFF;
    ctx->udp_sock    = -1;
    snprintf(ctx->key, sizeof(ctx->key), "%s", key);
    strcpy(ctx->codec_str, "unknown");

    pthread_mutex_init(&ctx->pkt_mtx,  NULL);
    pthread_cond_init(&ctx->pkt_cond,  NULL);
    pthread_mutex_init(&ctx->addr_mtx, NULL);

    /* Read config line from Node.js */
    char cfg[512] = "";
    rr_read_line(sfd, cfg, sizeof(cfg));
    ctx->ch      = rr_cfg_int(cfg, "channels", 2);
    if (ctx->ch < 1) ctx->ch = 1;
    if (ctx->ch > MAX_CH) ctx->ch = MAX_CH;
    ctx->buf_ms  = rr_cfg_int(cfg, "bufMs", 100);
    if (ctx->buf_ms < 10)   ctx->buf_ms = 10;
    if (ctx->buf_ms > 2000) ctx->buf_ms = 2000;
    ctx->in_rate = rr_cfg_int(cfg, "rate", 48000);
    if (ctx->in_rate <= 0)  ctx->in_rate = 48000;
    char proto_str[16];
    rr_cfg_str(cfg, "proto", proto_str, sizeof(proto_str), "rtp");
    ctx->proto = (strcmp(proto_str, "raw") == 0) ? PROTO_RAW : PROTO_RTP;
    rr_cfg_str(cfg, "addr", ctx->bind_addr, sizeof(ctx->bind_addr), "0.0.0.0");

    build_g711_tables(ctx);
    ctx->hip = hip_decode_init();
    setup_resampler(ctx);

    /* UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        fprintf(stderr, "[rtp_recv:%s] socket: %s\n", key, strerror(errno));
        free(ctx->pkts); free(ctx); close(sfd);
        return NULL;
    }
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    int port = rr_cfg_int(cfg, "port", 5004);
    struct in_addr bind_in = { .s_addr = INADDR_ANY };
    int is_multicast = 0;
    if (ctx->bind_addr[0] && inet_aton(ctx->bind_addr, &bind_in)) {
        uint32_t ba = ntohl(bind_in.s_addr);
        is_multicast = (ba >= 0xE0000000u && ba <= 0xEFFFFFFFu);
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = is_multicast ? bind_in.s_addr : INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        fprintf(stderr, "[rtp_recv:%s] bind: %s\n", key, strerror(errno));
        close(sock); free(ctx->pkts); free(ctx); close(sfd);
        return NULL;
    }
    if (is_multicast) {
        struct ip_mreq mreq;
        mreq.imr_multiaddr        = bind_in;
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
            fprintf(stderr, "[rtp_recv:%s] multicast join failed: %s\n", key, strerror(errno));
        else
            fprintf(stderr, "[rtp_recv:%s] multicast joined %s\n", key, ctx->bind_addr);
    }
    ctx->udp_sock = sock;

    fprintf(stderr, "[rtp_recv:%s] mode=%s port=%d ch=%d bufMs=%d addr=%s\n",
            key, proto_str, port, ctx->ch, ctx->buf_ms, ctx->bind_addr);

    pthread_create(&ctx->recv_tid,   NULL, recv_thread,   ctx);
    pthread_create(&ctx->decode_tid, NULL, decode_thread, ctx);
    pthread_create(&ctx->stats_tid,  NULL, stats_thread,  ctx);

    dprintf(sfd, "[rtp_recv] ready\n");
    fprintf(stderr, "[rtp_recv:%s] started\n", key);
    return ctx;
}

void rtp_recv_stop(RtpRecvCtx *ctx)
{
    if (!ctx) return;
    ctx->quit = 1;
    pthread_cond_broadcast(&ctx->pkt_cond);
    pthread_join(ctx->recv_tid,   NULL);
    pthread_join(ctx->decode_tid, NULL);
    pthread_join(ctx->stats_tid,  NULL);

    if (ctx->hip)       { hip_decode_exit(ctx->hip); ctx->hip = NULL; }
    if (ctx->src_state) { src_delete(ctx->src_state); ctx->src_state = NULL; }
    if (ctx->udp_sock >= 0) { close(ctx->udp_sock); ctx->udp_sock = -1; }
    if (ctx->sock_fd  >= 0) { close(ctx->sock_fd);  ctx->sock_fd  = -1; }
    pthread_mutex_destroy(&ctx->pkt_mtx);
    pthread_cond_destroy(&ctx->pkt_cond);
    pthread_mutex_destroy(&ctx->addr_mtx);
    free(ctx->pkts);
    free(ctx);
}
