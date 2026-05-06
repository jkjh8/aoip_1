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
#include <opus/opus.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <fcntl.h>
#include <poll.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <errno.h>

#include "include/engine_constants.h"
#include "include/ring_buf.h"
#include "include/rtp_recv.h"
#include "include/rtp_utils.h"

/* ── constants ───────────────────────────────────────── */
#define RTP_HDR_MIN      12
#define MAX_PKT_LEN      8192
#define PKT_QUEUE        512
#define PKT_QUEUE_MASK   (PKT_QUEUE - 1)
#define OUT_RATE         48000
#define RESAMPLE_OUT_MAX 8192
#define DECODE_BUF_MAX   8192
/* Opus dynamic PT (must match rtp_send.c OPUS_PT) */
#define OPUS_PT          98
#define OPUS_DEC_FRAMES  5760  /* max 120ms @48kHz */

/* ── types ───────────────────────────────────────────── */
typedef enum { PROTO_RTP = 0, PROTO_RAW } ProtoMode;
typedef enum {
    ENC_UNKNOWN = 0, ENC_L16, ENC_L24, ENC_MPA, ENC_PCMU, ENC_PCMA, ENC_OPUS
} EncMode;

typedef struct { uint8_t data[MAX_PKT_LEN]; int len; } RtpPkt;

struct RtpRecvCtx {
    /* config */
    int       ch, buf_ms, in_rate;
    ProtoMode proto;
    EncMode   enc;
    int       force_codec;   /* 1 = cfg로 codec/rate 고정, PT 자동감지 무시 */
    char      bind_addr[64];
    char      key[64];
    int       sock_fd;   /* Unix socket to Node.js (stats output) */
    int       udp_sock;
    int       is_multicast;
    struct in_addr mcast_addr;

    /* ring buffer (owned by engine) */
    RingBuf  *ring;

    /* state */
    volatile int quit;
    int prio;
    volatile int detected;
    uint8_t   last_rtp_pt;
    char      codec_str[32];

    /* decode error suppression */
    int          dec_err_count;
    int          dec_err_logged;

    /* flood / bad-pkt protection */
    int          flood_threshold;    /* pkt/s 초과 시 flood 백오프 (0=비활성) */
    int          flood_backoff_ms;   /* flood/bad-streak 시 드레인+슬립 시간(ms) */
    int          bad_pkt_threshold;  /* 연속 bad pkt 개수 초과 시 백오프 */
    int8_t       force_pt;           /* -1=auto, 0-127=고정 PT 필터 */
    atomic_int   in_backoff;

    /* stats */
    atomic_ulong packets, drops, udp_bytes;
    char         src_ip[64];
    int          src_port;
    pthread_mutex_t addr_mtx;

    /* decoders */
    hip_t        hip;
    SRC_STATE   *src_state;
    OpusDecoder *opus_dec;
    int16_t      ulaw_table[256];
    int16_t      alaw_table[256];

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
static void _hip_nolog(const char *fmt, va_list ap) { (void)fmt; (void)ap; }

static hip_t _hip_init(void) {
    hip_t h = hip_decode_init();
    if (h) {
        hip_set_errorf(h, _hip_nolog);
        hip_set_debugf(h, _hip_nolog);
        hip_set_msgf  (h, _hip_nolog);
    }
    return h;
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

/* ── detect encoding from RTP PT — returns 1 if supported, 0 to drop ── */
static int detect_rtp_pt(RtpRecvCtx *ctx, uint8_t pt, const uint8_t *payload, int plen)
{
    (void)payload; (void)plen;
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
             ctx->hip = _hip_init();
             break;
    case OPUS_PT: {
        ctx->enc = ENC_OPUS; ctx->in_rate = OUT_RATE;
        snprintf(ctx->codec_str, sizeof(ctx->codec_str), "Opus");
        if (ctx->opus_dec) { opus_decoder_destroy(ctx->opus_dec); ctx->opus_dec = NULL; }
        int err;
        ctx->opus_dec = opus_decoder_create(OUT_RATE, ctx->ch, &err);
        if (!ctx->opus_dec)
            fprintf(stderr, "[rtp_recv:%s] opus_decoder_create: %s\n", ctx->key, opus_strerror(err));
        break;
    }
    default:
        fprintf(stderr, "[rtp_recv:%s] unsupported PT=%d — dropping\n", ctx->key, pt);
        return 0;
    }
    fprintf(stderr, "[rtp_recv:%s] RTP PT=%d → enc=%s rate=%d\n",
            ctx->key, pt, ctx->codec_str, ctx->in_rate);
    setup_resampler(ctx);
    ctx->detected = 1;
    return 1;
}

/* ── detect encoding from raw UDP payload ────────────── */
static void detect_raw(RtpRecvCtx *ctx, const uint8_t *payload, int len)
{
    if (len >= 4 && payload[0] == 0xFF && (payload[1] & 0xE0) == 0xE0) {
        ctx->enc = ENC_MPA;
        snprintf(ctx->codec_str, sizeof(ctx->codec_str), "mp3");
        if (ctx->hip) { hip_decode_exit(ctx->hip); }
        ctx->hip = _hip_init();
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
        int mpa_ok = 0;
        while ((samples = hip_decode1_headers(ctx->hip, feed_buf, feed_len,
                                              pcm_l, pcm_r, &mp3info)) != -1) {
            feed_buf = s_empty;
            feed_len = 0;
            if (samples == 0) break;  /* sync 탐색 중, 아직 데이터 없음 */
            mpa_ok = 1;
            if (mp3info.header_parsed) {
                int hz = mp3info.samplerate;
                if (hz > 0 && hz != ctx->in_rate) {
                    fprintf(stderr, "[rtp_recv:%s] MP3 rate: %d→%d\n", ctx->key, ctx->in_rate, hz);
                    /* flush resampler internal buffer before reinitializing to avoid audio artifacts */
                    if (ctx->src_state) {
                        SRC_DATA flush_sd = {
                            .data_in       = ctx->dec_buf,
                            .data_out      = ctx->rs_out,
                            .input_frames  = 0,
                            .output_frames = RESAMPLE_OUT_MAX,
                            .src_ratio     = (double)OUT_RATE / ctx->in_rate,
                            .end_of_input  = 1,
                        };
                        src_process(ctx->src_state, &flush_sd);
                        ring_write(ctx, ctx->rs_out, (int)flush_sd.output_frames_gen);
                    }
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
        if (mpa_ok) {
            ctx->dec_err_count = 0;
        } else if (samples == -1) {
            /* -1: 실제 디코드 오류 */
            ctx->dec_err_count++;
            if (!ctx->dec_err_logged || ctx->dec_err_count % 1000 == 0) {
                fprintf(stderr, "[rtp_recv:%s] MPA decode error (x%d) — wrong format?\n",
                        ctx->key, ctx->dec_err_count);
                ctx->dec_err_logged = 1;
            }
        }
        /* samples==0: sync 탐색 중 — 정상, 로그 없음 */
        break;
    }

    case ENC_OPUS: {
        if (!ctx->opus_dec) break;
        /* dec_buf holds float, max OPUS_DEC_FRAMES * ch samples */
        int frames = opus_decode_float(ctx->opus_dec, payload, len,
                                       ctx->dec_buf, OPUS_DEC_FRAMES, 0);
        if (frames < 0) {
            ctx->dec_err_count++;
            if (!ctx->dec_err_logged || ctx->dec_err_count % 1000 == 0) {
                fprintf(stderr, "[rtp_recv:%s] opus_decode_float: %s (x%d)\n",
                        ctx->key, opus_strerror(frames), ctx->dec_err_count);
                ctx->dec_err_logged = 1;
            }
            break;
        }
        ctx->dec_err_count = 0;
        /* Opus always outputs at OUT_RATE, no resampling needed */
        ring_write(ctx, ctx->dec_buf, frames);
        break;
    }

    default: break;
    }
}

/* ── decode thread ───────────────────────────────────── */
static void *decode_thread(void *arg)
{
    RtpRecvCtx *ctx = (RtpRecvCtx *)arg;
    if (ctx->prio > 0) {
        struct sched_param sp = { .sched_priority = ctx->prio };
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
    rtp_pin_to_cpu(2);

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
            if (pt >= 72 && pt <= 76) continue;  /* RTCP mux (RFC 5761) — ignore */
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

            if (!ctx->detected) {
                if (!detect_rtp_pt(ctx, pt, payload, plen)) continue;
            }

            if (!ctx->force_codec && ctx->detected && pt != ctx->last_rtp_pt && ctx->last_rtp_pt != 0xFF) {
                ctx->detected = 0;
                if (!detect_rtp_pt(ctx, pt, payload, plen)) { ctx->last_rtp_pt = pt; continue; }
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

/* ── flood/bad-pkt 백오프: 소켓 드레인 후 슬립 ──────── */
static void flood_drain_and_backoff(RtpRecvCtx *ctx, int backoff_ms)
{
    atomic_store(&ctx->in_backoff, 1);
    fprintf(stderr, "[rtp_recv:%s] backoff %dms — draining socket\n",
            ctx->key, backoff_ms);

    struct timespec end_ts;
    clock_gettime(CLOCK_MONOTONIC, &end_ts);
    long ns = end_ts.tv_nsec + (long)backoff_ms * 1000000L;
    end_ts.tv_sec  += ns / 1000000000L;
    end_ts.tv_nsec  = ns % 1000000000L;

    uint8_t drain_buf[MAX_PKT_LEN];
    while (!ctx->quit) {
        ssize_t n;
        /* 소켓 버퍼를 논블럭으로 비움 */
        while ((n = recvfrom(ctx->udp_sock, drain_buf, sizeof(drain_buf),
                             MSG_DONTWAIT, NULL, NULL)) > 0)
            atomic_fetch_add_explicit(&ctx->drops, 1, memory_order_relaxed);

        struct timespec now;
        clock_gettime(CLOCK_MONOTONIC, &now);
        if (now.tv_sec > end_ts.tv_sec ||
            (now.tv_sec == end_ts.tv_sec && now.tv_nsec >= end_ts.tv_nsec))
            break;
        usleep(10000); /* 10ms 간격으로 드레인 반복 */
    }
    atomic_store(&ctx->in_backoff, 0);
}

/* ── receive thread ──────────────────────────────────── */
static void *recv_thread(void *arg)
{
    RtpRecvCtx *ctx = (RtpRecvCtx *)arg;
    if (ctx->prio > 0) {
        struct sched_param sp = { .sched_priority = ctx->prio };
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &sp);
    }
    rtp_pin_to_cpu(2);
    uint8_t buf[MAX_PKT_LEN];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    struct pollfd pfd = { .fd = ctx->udp_sock, .events = POLLIN };

    /* flood 감지용 슬라이딩 윈도우 */
    unsigned long win_pkts = 0;
    int bad_streak = 0;
    struct timespec win_ts;
    clock_gettime(CLOCK_MONOTONIC, &win_ts);

    while (!ctx->quit) {
        int r = poll(&pfd, 1, 100); /* 100ms timeout → quit 플래그 주기적 확인 */
        if (r < 0) {
            if (errno == EINTR) continue;
            if (!ctx->quit) perror("[rtp_recv] poll");
            break;
        }
        if (r == 0) continue; /* timeout */

        ssize_t n = recvfrom(ctx->udp_sock, buf, sizeof(buf), MSG_DONTWAIT,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK) continue;
            if (!ctx->quit) perror("[rtp_recv] recvfrom");
            break;
        }

        /* ── 1. flood 감지: 1초 윈도우 패킷 수 체크 ── */
        if (ctx->flood_threshold > 0) {
            struct timespec now;
            clock_gettime(CLOCK_MONOTONIC, &now);
            long elapsed_ms = (now.tv_sec  - win_ts.tv_sec)  * 1000L
                            + (now.tv_nsec - win_ts.tv_nsec) / 1000000L;
            if (elapsed_ms >= 1000) {
                if (win_pkts > (unsigned long)ctx->flood_threshold) {
                    fprintf(stderr, "[rtp_recv:%s] flood: %lu pkt/s — backoff %dms\n",
                            ctx->key, win_pkts, ctx->flood_backoff_ms);
                    flood_drain_and_backoff(ctx, ctx->flood_backoff_ms);
                    bad_streak = 0;
                }
                win_pkts = 0;
                win_ts   = now;
            }
            win_pkts++;
        }

        /* ── 2. 조기 RTP 패킷 검증 (PROTO_RTP 모드) ── */
        if (ctx->proto == PROTO_RTP) {
            if (n < RTP_HDR_MIN) {
                /* 너무 짧은 패킷 — RTP 헤더 최소 크기 미달 */
                atomic_fetch_add_explicit(&ctx->drops, 1, memory_order_relaxed);
                goto check_bad_streak;
            }
            /* RTP 버전 필드는 반드시 2 */
            if ((buf[0] >> 6) != 2) {
                atomic_fetch_add_explicit(&ctx->drops, 1, memory_order_relaxed);
                goto check_bad_streak;
            }
            /* PT 고정 필터: force_pt 또는 이미 감지된 last_rtp_pt 기준 */
            uint8_t pt = buf[1] & 0x7F;
            int expected_pt = (ctx->force_pt >= 0)
                              ? (int)ctx->force_pt
                              : ((ctx->detected && ctx->last_rtp_pt != 0xFF)
                                 ? (int)ctx->last_rtp_pt : -1);
            if (expected_pt >= 0 && (int)pt != expected_pt
                    && !(pt >= 72 && pt <= 76)) { /* RTCP mux는 허용 (decode_thread에서 drop) */
                atomic_fetch_add_explicit(&ctx->drops, 1, memory_order_relaxed);
                goto check_bad_streak;
            }
        }

        /* 정상 패킷: bad_streak 리셋 */
        bad_streak = 0;
        goto enqueue;

check_bad_streak:
        if (ctx->bad_pkt_threshold > 0 && ++bad_streak >= ctx->bad_pkt_threshold) {
            fprintf(stderr, "[rtp_recv:%s] bad-pkt streak %d — backoff %dms\n",
                    ctx->key, bad_streak, ctx->flood_backoff_ms);
            flood_drain_and_backoff(ctx, ctx->flood_backoff_ms);
            bad_streak = 0;
        }
        continue;

enqueue:
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
    int no_data_count = 0;
    while (!ctx->quit) {
        sleep(2);
        unsigned long cur  = atomic_load(&ctx->udp_bytes);
        int kbps = (int)((cur - prev_bytes) * 8 / 2 / 1000);
        int has_data = (cur != prev_bytes);
        if (!has_data) {
            pthread_mutex_lock(&ctx->addr_mtx);
            ctx->src_ip[0] = '\0'; ctx->src_port = 0;
            pthread_mutex_unlock(&ctx->addr_mtx);
            /* multicast re-join every 6s of silence to recover from stale IGMP state after reboot */
            if (ctx->is_multicast && ++no_data_count >= 3) {
                no_data_count = 0;
                struct ip_mreq mreq;
                mreq.imr_multiaddr        = ctx->mcast_addr;
                mreq.imr_interface.s_addr = INADDR_ANY;
                setsockopt(ctx->udp_sock, IPPROTO_IP, IP_DROP_MEMBERSHIP, &mreq, sizeof(mreq));
                if (setsockopt(ctx->udp_sock, IPPROTO_IP, IP_ADD_MEMBERSHIP, &mreq, sizeof(mreq)) < 0)
                    fprintf(stderr, "[rtp_recv:%s] multicast re-join failed: %s\n", ctx->key, strerror(errno));
                else
                    fprintf(stderr, "[rtp_recv:%s] multicast re-joined %s\n", ctx->key, ctx->bind_addr);
            }
        } else {
            no_data_count = 0;
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
    int sfd = rtp_unix_connect(sock_path, 50, 200);
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
    rtp_read_line(sfd, cfg, sizeof(cfg));
    ctx->ch      = rtp_cfg_int(cfg, "channels", 2);
    if (ctx->ch < 1) ctx->ch = 1;
    if (ctx->ch > MAX_CH) ctx->ch = MAX_CH;
    ctx->buf_ms  = rtp_cfg_int(cfg, "bufMs", 100);
    if (ctx->buf_ms < 10)   ctx->buf_ms = 10;
    if (ctx->buf_ms > 2000) ctx->buf_ms = 2000;
    ctx->in_rate = rtp_cfg_int(cfg, "rate", 0);
    char proto_str[16], codec_str[16];
    rtp_cfg_str(cfg, "proto", proto_str, sizeof(proto_str), "rtp");
    rtp_cfg_str(cfg, "codec", codec_str, sizeof(codec_str), "");
    ctx->proto = (strcmp(proto_str, "raw") == 0) ? PROTO_RAW : PROTO_RTP;
    rtp_cfg_str(cfg, "addr", ctx->bind_addr, sizeof(ctx->bind_addr), "0.0.0.0");

    /* flood / bad-pkt 보호 설정 */
    ctx->flood_threshold  = rtp_cfg_int(cfg, "floodThreshold",  5000); /* 0=비활성 */
    ctx->flood_backoff_ms = rtp_cfg_int(cfg, "floodBackoffMs",   500);
    ctx->bad_pkt_threshold= rtp_cfg_int(cfg, "badPktThreshold",  200);
    int force_pt_val      = rtp_cfg_int(cfg, "pt",               -1);
    ctx->force_pt = (force_pt_val >= 0 && force_pt_val <= 127) ? (int8_t)force_pt_val : -1;

    /* 수동 codec/rate 설정 */
    if (codec_str[0]) {
        if      (strcmp(codec_str, "l16")  == 0) { ctx->enc = ENC_L16;  snprintf(ctx->codec_str, sizeof(ctx->codec_str), "L16");  if (!ctx->in_rate) ctx->in_rate = 48000; }
        else if (strcmp(codec_str, "l24")  == 0) { ctx->enc = ENC_L24;  snprintf(ctx->codec_str, sizeof(ctx->codec_str), "L24");  if (!ctx->in_rate) ctx->in_rate = 48000; }
        else if (strcmp(codec_str, "mpa")  == 0) { ctx->enc = ENC_MPA;  snprintf(ctx->codec_str, sizeof(ctx->codec_str), "MPA");  if (!ctx->in_rate) ctx->in_rate = 48000; }
        else if (strcmp(codec_str, "pcmu") == 0) { ctx->enc = ENC_PCMU; snprintf(ctx->codec_str, sizeof(ctx->codec_str), "PCMU"); if (!ctx->in_rate) ctx->in_rate = 8000;  }
        else if (strcmp(codec_str, "pcma") == 0) { ctx->enc = ENC_PCMA; snprintf(ctx->codec_str, sizeof(ctx->codec_str), "PCMA"); if (!ctx->in_rate) ctx->in_rate = 8000;  }
        else if (strcmp(codec_str, "opus") == 0) { ctx->enc = ENC_OPUS; snprintf(ctx->codec_str, sizeof(ctx->codec_str), "Opus"); if (!ctx->in_rate) ctx->in_rate = 48000; }
        if (ctx->enc != ENC_UNKNOWN) {
            ctx->force_codec = 1;
            ctx->detected    = 1;
            ctx->last_rtp_pt = 0xFF;
            fprintf(stderr, "[rtp_recv:%s] forced codec=%s rate=%d\n", key, ctx->codec_str, ctx->in_rate);
        }
    }
    if (ctx->in_rate <= 0) ctx->in_rate = 48000;

    build_g711_tables(ctx);
    ctx->hip = _hip_init();
    if (ctx->enc == ENC_OPUS) {
        int err;
        ctx->opus_dec = opus_decoder_create(OUT_RATE, ctx->ch, &err);
        if (!ctx->opus_dec)
            fprintf(stderr, "[rtp_recv:%s] opus_decoder_create: %s\n", key, opus_strerror(err));
    }
    setup_resampler(ctx);

    /* UDP socket */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        fprintf(stderr, "[rtp_recv:%s] socket: %s\n", key, strerror(errno));
        free(ctx->pkts); free(ctx); close(sfd);
        return NULL;
    }
    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
    /* 논블럭 소켓: poll()로 타임아웃 제어, SO_RCVTIMEO 불필요 */
    int fl = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, fl | O_NONBLOCK);

    int port = rtp_cfg_int(cfg, "port", 5004);
    struct in_addr bind_in = { .s_addr = INADDR_ANY };
    int is_multicast = 0;
    if (ctx->bind_addr[0] && inet_aton(ctx->bind_addr, &bind_in)) {
        uint32_t ba = ntohl(bind_in.s_addr);
        is_multicast = (ba >= 0xE0000000u && ba <= 0xEFFFFFFFu);
    }
    struct sockaddr_in addr = {
        .sin_family      = AF_INET,
        .sin_port        = htons((uint16_t)port),
        .sin_addr.s_addr = INADDR_ANY,
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
        ctx->is_multicast = 1;
        ctx->mcast_addr   = bind_in;
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
    if (ctx->opus_dec)  { opus_decoder_destroy(ctx->opus_dec); ctx->opus_dec = NULL; }
    if (ctx->src_state) { src_delete(ctx->src_state); ctx->src_state = NULL; }
    if (ctx->udp_sock >= 0) { close(ctx->udp_sock); ctx->udp_sock = -1; }
    if (ctx->sock_fd  >= 0) { close(ctx->sock_fd);  ctx->sock_fd  = -1; }
    pthread_mutex_destroy(&ctx->pkt_mtx);
    pthread_cond_destroy(&ctx->pkt_cond);
    pthread_mutex_destroy(&ctx->addr_mtx);
    free(ctx->pkts);
    free(ctx);
}
