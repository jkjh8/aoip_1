/*
 * rtp_recv.c — Lightweight UDP/RTP receiver (GStreamer-free)
 *
 * Dependencies: libsamplerate, minimp3.h (header-only)
 *
 * Flow:
 *   UDP socket → packet queue → decode thread → ShmRing(F32LE@48kHz)
 *
 * Usage:
 *   rtp_recv <port> <channels> <proto> <name> <bufMs> <rate> <enc> <addr> shm <shm_name>
 *
 * Proto: rtp | raw
 * Auto-detect: RTP PT field / raw UDP payload sniff (MP3 sync word)
 *
 * Stderr:
 *   [rtp_recv] ready
 *   stats codec=... bufMs=N packets=N drops=N srcIp=... srcPort=N bitrateKbps=N
 */

#include <lame/lame.h>
#include <samplerate.h>

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

#include "include/shm_ring.h"

/* ── constants ───────────────────────────────────────── */
#define RTP_HDR_MIN      12
#define MAX_PKT_LEN      8192
#define PKT_QUEUE        512     /* power of 2 */
#define PKT_QUEUE_MASK   (PKT_QUEUE - 1)
#define OUT_RATE         48000
#define RESAMPLE_OUT_MAX 8192

/* ── packet queue ────────────────────────────────────── */
typedef struct {
    uint8_t data[MAX_PKT_LEN];
    int     len;
} Pkt;

static Pkt             g_pkts[PKT_QUEUE];
static int             g_pkt_wp = 0;
static int             g_pkt_rp = 0;
static pthread_mutex_t g_pkt_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  g_pkt_cond = PTHREAD_COND_INITIALIZER;

/* ── mode/encoding ───────────────────────────────────── */
typedef enum { PROTO_RTP = 0, PROTO_RAW } ProtoMode;
typedef enum {
    ENC_UNKNOWN = 0,
    ENC_L16,
    ENC_L24,
    ENC_MPA,   /* MPEG Audio (MP3) */
    ENC_PCMU,  /* G.711 μ-law */
    ENC_PCMA   /* G.711 A-law */
} EncMode;

static ProtoMode g_proto    = PROTO_RTP;
static EncMode   g_enc      = ENC_UNKNOWN;
static int       g_in_rate  = 48000;
static int       g_ch       = 2;
static int       g_buf_ms   = 100;
static char      g_bind_addr[64] = "0.0.0.0";

/* ── globals ─────────────────────────────────────────── */
static volatile int g_quit      = 0;
static volatile int g_exit_code = 0;
static volatile int g_detected  = 0;

/* stats */
static char            g_codec[32]   = "unknown";
static atomic_ulong    g_packets     = 0;
static atomic_ulong    g_drops       = 0;
static atomic_ulong    g_udp_bytes   = 0;
static char            g_src_ip[64]  = "";
static int             g_src_port    = 0;
static pthread_mutex_t g_addr_mtx    = PTHREAD_MUTEX_INITIALIZER;

/* ── SHM ─────────────────────────────────────────────── */
static char     g_shm_name[256] = "";
static int      g_shm_fd        = -1;
static ShmRing *g_shm           = NULL;

/* ── decoders ────────────────────────────────────────── */
static hip_t      g_hip       = NULL;   /* LAME MP3 decoder */
static SRC_STATE *g_src_state = NULL;

/* G.711 decode tables */
static int16_t g_ulaw_table[256];
static int16_t g_alaw_table[256];

/* ── G.711 table init ────────────────────────────────── */
static void build_g711_tables(void)
{
    /* μ-law (ITU-T G.711) */
    for (int i = 0; i < 256; i++) {
        int u        = ~i & 0xFF;
        int sign     = (u & 0x80) ? -1 : 1;
        int exponent = (u >> 4) & 0x07;
        int mantissa = u & 0x0F;
        int sample   = ((mantissa << 1) | 1) << (exponent + 2);
        g_ulaw_table[i] = (int16_t)(sign * (sample - 33));
    }
    /* A-law (ITU-T G.711) */
    for (int i = 0; i < 256; i++) {
        int a        = i ^ 0x55;
        int sign     = (a & 0x80) ? 1 : -1;
        int exponent = (a >> 4) & 0x07;
        int mantissa = a & 0x0F;
        int sample   = (exponent == 0)
                       ? ((mantissa << 1) | 1)
                       : ((mantissa | 0x10) << exponent);
        g_alaw_table[i] = (int16_t)(sign * sample * 8);
    }
}

/* ── SHM attach ──────────────────────────────────────── */
static int shm_attach(void)
{
    for (int i = 0; i < 50; i++) {
        g_shm_fd = shm_open(g_shm_name, O_RDWR, 0);
        if (g_shm_fd >= 0) break;
        usleep(100000);
    }
    if (g_shm_fd < 0) {
        fprintf(stderr, "[rtp_recv] shm_open(%s): %s\n", g_shm_name, strerror(errno));
        return 0;
    }
    g_shm = mmap(NULL, SHMRING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (g_shm == MAP_FAILED) {
        fprintf(stderr, "[rtp_recv] mmap: %s\n", strerror(errno));
        close(g_shm_fd); g_shm_fd = -1; g_shm = NULL;
        return 0;
    }
    fprintf(stderr, "[rtp_recv] attached shm %s\n", g_shm_name);
    return 1;
}

/* ── write F32 frames to ShmRing ─────────────────────── */
static void shm_write(const float *buf, int frames)
{
    if (!g_shm || frames <= 0) return;
    for (int f = 0; f < frames; f++) {
        uint32_t wp  = atomic_load_explicit(&g_shm->wp, memory_order_relaxed);
        uint32_t rp  = atomic_load_explicit(&g_shm->rp, memory_order_acquire);
        if ((int32_t)(wp - rp) >= SHM_RING_FRAMES - 1)
            break;  /* ring full: burst overflow 방지 */
        uint32_t idx = wp % (uint32_t)SHM_RING_FRAMES;
        for (int c = 0; c < g_ch && c < SHM_MAX_CH; c++)
            g_shm->buf[idx * SHM_MAX_CH + c] = buf[f * g_ch + c];
        atomic_store_explicit(&g_shm->wp, wp + 1u, memory_order_release);
    }
}

/* ── resample + write ────────────────────────────────── */
static float g_rs_out[RESAMPLE_OUT_MAX * SHM_MAX_CH];

static void resample_and_write(const float *in, int in_frames)
{
    if (!g_src_state) {
        shm_write(in, in_frames);
        return;
    }
    SRC_DATA sd = {
        .data_in       = in,
        .data_out      = g_rs_out,
        .input_frames  = in_frames,
        .output_frames = RESAMPLE_OUT_MAX,
        .src_ratio     = (double)OUT_RATE / g_in_rate,
        .end_of_input  = 0,
    };
    src_process(g_src_state, &sd);
    shm_write(g_rs_out, (int)sd.output_frames_gen);
}

/* ── setup resampler (if rate != OUT_RATE) ───────────── */
static void setup_resampler(void)
{
    if (g_src_state) { src_delete(g_src_state); g_src_state = NULL; }
    if (g_in_rate == OUT_RATE) return;
    int err;
    g_src_state = src_new(SRC_SINC_FASTEST, g_ch, &err);
    if (!g_src_state)
        fprintf(stderr, "[rtp_recv] src_new: %s\n", src_strerror(err));
    else
        fprintf(stderr, "[rtp_recv] resampler: %d→%d\n", g_in_rate, OUT_RATE);
}

/* ── detect encoding from RTP PT ─────────────────────── */
static void detect_rtp_pt(uint8_t pt, const uint8_t *payload, int plen)
{
    switch (pt) {
    case 0:  g_enc = ENC_PCMU; g_in_rate = 8000;
             snprintf(g_codec, sizeof(g_codec), "PCMU"); break;
    case 8:  g_enc = ENC_PCMA; g_in_rate = 8000;
             snprintf(g_codec, sizeof(g_codec), "PCMA"); break;
    case 10: g_enc = ENC_L16;  g_in_rate = 44100;
             snprintf(g_codec, sizeof(g_codec), "L16");  break;
    case 11: g_enc = ENC_L16;  g_in_rate = 44100;
             snprintf(g_codec, sizeof(g_codec), "L16");  break;
    case 14: g_enc = ENC_MPA;  g_in_rate = 48000;
             snprintf(g_codec, sizeof(g_codec), "MPA");
             if (g_hip) { hip_decode_exit(g_hip); }
             g_hip = hip_decode_init();
             break;
    default:
        /* dynamic PT: sniff payload for MP3 sync word (skip 4-byte MPA header) */
        if (plen >= 6) {
            const uint8_t *p = payload;
            int off = 0;
            /* skip RFC 2250 4-byte MPA header if present */
            if (plen >= 4) off = 4;
            if ((p[off] & 0xFF) == 0xFF && (p[off+1] & 0xE0) == 0xE0) {
                g_enc = ENC_MPA; g_in_rate = 48000;
                snprintf(g_codec, sizeof(g_codec), "MPA");
                break;
            }
        }
        /* fallback: guess L16 or L24 by payload size */
        if (plen > 4000) {
            g_enc = ENC_L24;
            snprintf(g_codec, sizeof(g_codec), "L24");
        } else {
            g_enc = ENC_L16;
            snprintf(g_codec, sizeof(g_codec), "L16");
        }
        break;
    }
    fprintf(stderr, "[rtp_recv] RTP PT=%d → enc=%s rate=%d\n", pt, g_codec, g_in_rate);
    setup_resampler();
    g_detected = 1;
}

/* ── detect encoding from raw UDP payload ────────────── */
static void detect_raw(const uint8_t *payload, int len)
{
    /* MP3 sync word */
    if (len >= 4 && payload[0] == 0xFF && (payload[1] & 0xE0) == 0xE0) {
        g_enc = ENC_MPA;
        snprintf(g_codec, sizeof(g_codec), "mp3");
        if (g_hip) { hip_decode_exit(g_hip); }
        g_hip = hip_decode_init();
        fprintf(stderr, "[rtp_recv] raw: detected MP3\n");
    } else if (len > 0 && (len % (g_ch * 3)) == 0 && len > 4000) {
        g_enc = ENC_L24;
        snprintf(g_codec, sizeof(g_codec), "L24");
        fprintf(stderr, "[rtp_recv] raw: detected L24 (payload=%d)\n", len);
    } else {
        g_enc = ENC_L16;
        snprintf(g_codec, sizeof(g_codec), "L16");
        fprintf(stderr, "[rtp_recv] raw: detected L16 (payload=%d)\n", len);
    }
    setup_resampler();
    g_detected = 1;
}

/* ── decode one payload buffer ───────────────────────── */
/* hip decoder: 1152 samples/frame max; G.711/L16/L24: up to 8192 frames */
#define DECODE_BUF_MAX 8192
static float g_dec_buf[DECODE_BUF_MAX * SHM_MAX_CH];

static void decode_payload(const uint8_t *payload, int len)
{
    switch (g_enc) {

    case ENC_L16: {
        int frames = (len / 2) / g_ch;
        if (frames * g_ch * 2 > (int)sizeof(g_dec_buf) / (int)sizeof(float))
            frames = (int)(sizeof(g_dec_buf) / sizeof(float)) / g_ch;
        for (int i = 0; i < frames * g_ch; i++) {
            int16_t s = (int16_t)((payload[i*2] << 8) | payload[i*2+1]);
            g_dec_buf[i] = s / 32768.0f;
        }
        resample_and_write(g_dec_buf, frames);
        break;
    }

    case ENC_L24: {
        int frames = (len / 3) / g_ch;
        if (frames * g_ch > (int)(sizeof(g_dec_buf) / sizeof(float)))
            frames = (int)(sizeof(g_dec_buf) / sizeof(float)) / g_ch;
        for (int i = 0; i < frames * g_ch; i++) {
            int32_t s = ((int32_t)(int8_t)payload[i*3]     << 16)
                      | ((int32_t)payload[i*3+1]            <<  8)
                      |  (int32_t)payload[i*3+2];
            g_dec_buf[i] = s / 8388608.0f;
        }
        resample_and_write(g_dec_buf, frames);
        break;
    }

    case ENC_PCMU:
    case ENC_PCMA: {
        /* G.711: 1 byte per sample, mono usually but respect g_ch */
        const int16_t *tbl = (g_enc == ENC_PCMU) ? g_ulaw_table : g_alaw_table;
        int frames = len / g_ch;
        if (frames * g_ch > (int)(sizeof(g_dec_buf) / sizeof(float)))
            frames = (int)(sizeof(g_dec_buf) / sizeof(float)) / g_ch;
        for (int f = 0; f < frames; f++)
            for (int c = 0; c < g_ch; c++)
                g_dec_buf[f * g_ch + c] = tbl[payload[f * g_ch + c]] / 32768.0f;
        resample_and_write(g_dec_buf, frames);
        break;
    }

    case ENC_MPA: {
        if (!g_hip) break;
        /* MP3: skip RFC 2250 4-byte header (MBZ + Frag_offset) in RTP mode */
        const uint8_t *mp3data = payload;
        int mp3len = len;
        if (g_proto == PROTO_RTP && len >= 4) {
            /* Frag_offset != 0 이면 이 패킷은 이전 프레임의 continuation.
             * hip_decode가 내부적으로 조각을 재조립하므로 그대로 전달. */
            mp3data += 4;
            mp3len  -= 4;
        }
        if (mp3len <= 0) break;

        /* LAME hip decoder: 내부 버퍼를 관리하므로 조각 재조립 불필요.
         * hip_decode1_headers: 한 번에 최대 1 프레임 반환 + header 정보(rate/ch).
         * 첫 호출에 데이터 제공, 이후 len=0으로 내부 버퍼 드레인. */
        static unsigned char s_empty[1] = {0};
        short pcm_l[1152], pcm_r[1152];
        mp3data_struct mp3info;
        unsigned char *feed_buf = (unsigned char *)mp3data;
        size_t         feed_len = (size_t)mp3len;
        int samples;
        while ((samples = hip_decode1_headers(g_hip, feed_buf, feed_len,
                                              pcm_l, pcm_r, &mp3info)) >= 0) {
            /* 첫 호출 이후 내부 버퍼만 드레인 */
            feed_buf = s_empty;
            feed_len = 0;

            if (samples == 0) break;  /* 더 이상 완성된 프레임 없음 */

            /* header_parsed: 처음 헤더를 파싱했을 때 rate/ch 업데이트 */
            if (mp3info.header_parsed) {
                int hz = mp3info.samplerate;
                if (hz > 0 && hz != g_in_rate) {
                    fprintf(stderr, "[rtp_recv] MP3 rate detected: %d→%d\n", g_in_rate, hz);
                    g_in_rate = hz;
                    setup_resampler();
                }
            }

            /* int16 → float, interleave L/R */
            float *out = g_dec_buf;
            if (g_ch == 2) {
                for (int i = 0; i < samples; i++) {
                    out[i*2]   = pcm_l[i] / 32768.0f;
                    out[i*2+1] = pcm_r[i] / 32768.0f;
                }
            } else {
                /* mono: average L+R */
                for (int i = 0; i < samples; i++)
                    out[i] = (pcm_l[i] + pcm_r[i]) / 65536.0f;
            }
            resample_and_write(out, samples);
        }
        break;
    }

    default: break;
    }
}

/* ── decode thread ───────────────────────────────────── */
static void *decode_thread(void *arg)
{
    (void)arg;

    while (!g_quit) {
        pthread_mutex_lock(&g_pkt_mtx);
        while (g_pkt_wp == g_pkt_rp && !g_quit)
            pthread_cond_wait(&g_pkt_cond, &g_pkt_mtx);
        if (g_quit) { pthread_mutex_unlock(&g_pkt_mtx); break; }

        Pkt tmp; /* copy out to minimize lock time */
        tmp = g_pkts[g_pkt_rp & PKT_QUEUE_MASK];
        g_pkt_rp++;
        pthread_mutex_unlock(&g_pkt_mtx);

        const uint8_t *data = tmp.data;
        int            dlen = tmp.len;

        /* ── extract payload ──────────────────────────── */
        const uint8_t *payload = data;
        int            plen    = dlen;

        if (g_proto == PROTO_RTP) {
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

            /* auto-detect on first RTP packet */
            if (!g_detected)
                detect_rtp_pt(pt, payload, plen);

            /* re-detect if PT changed (codec switch) */
            static uint8_t last_pt = 0xFF;
            if (g_detected && pt != last_pt && last_pt != 0xFF) {
                fprintf(stderr, "[rtp_recv] PT changed %d→%d, re-detecting\n", last_pt, pt);
                g_detected = 0;
                detect_rtp_pt(pt, payload, plen);
            }
            last_pt = pt;
        } else {
            /* raw UDP: detect on first packet */
            if (!g_detected)
                detect_raw(payload, plen);
        }

        if (!g_detected) continue;

        decode_payload(payload, plen);
        atomic_fetch_add_explicit(&g_packets, 1, memory_order_relaxed);
    }
    return NULL;
}

/* ── receive thread ──────────────────────────────────── */
static void *recv_thread(void *arg)
{
    int sock = *(int *)arg;
    uint8_t buf[MAX_PKT_LEN];
    struct sockaddr_in from;
    socklen_t fromlen = sizeof(from);

    while (!g_quit) {
        ssize_t n = recvfrom(sock, buf, sizeof(buf), 0,
                             (struct sockaddr *)&from, &fromlen);
        if (n <= 0) {
            if (errno == EAGAIN || errno == EINTR || errno == EWOULDBLOCK) continue;
            if (!g_quit) perror("[rtp_recv] recvfrom");
            break;
        }

        pthread_mutex_lock(&g_addr_mtx);
        inet_ntop(AF_INET, &from.sin_addr, g_src_ip, sizeof(g_src_ip));
        g_src_port = ntohs(from.sin_port);
        pthread_mutex_unlock(&g_addr_mtx);

        atomic_fetch_add_explicit(&g_udp_bytes, (unsigned long)n, memory_order_relaxed);

        pthread_mutex_lock(&g_pkt_mtx);
        if ((g_pkt_wp - g_pkt_rp) >= PKT_QUEUE) {
            atomic_fetch_add_explicit(&g_drops, 1, memory_order_relaxed);
            pthread_mutex_unlock(&g_pkt_mtx);
        } else {
            Pkt *pkt = &g_pkts[g_pkt_wp & PKT_QUEUE_MASK];
            memcpy(pkt->data, buf, (size_t)n);
            pkt->len = (int)n;
            g_pkt_wp++;
            pthread_cond_signal(&g_pkt_cond);
            pthread_mutex_unlock(&g_pkt_mtx);
        }
    }
    return NULL;
}

/* ── stats thread ────────────────────────────────────── */
static void *stats_thread(void *arg)
{
    (void)arg;
    unsigned long prev_bytes = 0;
    while (!g_quit) {
        sleep(2);
        unsigned long cur  = atomic_load(&g_udp_bytes);
        int kbps = (int)((cur - prev_bytes) * 8 / 2 / 1000);
        int has_data = (cur != prev_bytes);
        if (!has_data) {
            pthread_mutex_lock(&g_addr_mtx);
            g_src_ip[0] = '\0'; g_src_port = 0;
            pthread_mutex_unlock(&g_addr_mtx);
        }
        prev_bytes = cur;

        char src_ip[64]; int src_port;
        pthread_mutex_lock(&g_addr_mtx);
        snprintf(src_ip, sizeof(src_ip), "%s", g_src_ip[0] ? g_src_ip : "none");
        src_port = g_src_port;
        pthread_mutex_unlock(&g_addr_mtx);

        int r = fprintf(stderr,
            "stats codec=%s bufMs=%d packets=%lu drops=%lu srcIp=%s srcPort=%d bitrateKbps=%d\n",
            g_codec, g_buf_ms,
            atomic_load(&g_packets), atomic_load(&g_drops),
            src_ip, src_port, kbps);
        fflush(stderr);
        if (r < 0) { g_quit = 1; break; }  /* socket 닫힘(Node.js 재시작) → 종료 */
    }
    return NULL;
}

/* ── Unix socket helpers ─────────────────────────────── */
#include <sys/un.h>

static int unix_connect(const char *path, int retries, int ms)
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

static int read_line_fd(int fd, char *buf, int maxlen)
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

static int cfg_int(const char *s, const char *key, int def)
{
    char pat[64]; int v = def;
    snprintf(pat, sizeof(pat), "%s=%%d", key);
    const char *p = strstr(s, key);
    if (p) sscanf(p, pat, &v);
    return v;
}

static void cfg_str(const char *s, const char *key, char *buf, size_t n, const char *def)
{
    strncpy(buf, def, n); buf[n-1] = '\0';
    char pat[64]; snprintf(pat, sizeof(pat), "%s=%%%zus", key, n-1);
    const char *p = strstr(s, key);
    if (p) sscanf(p, pat, buf);
}

/* ── signal ──────────────────────────────────────────── */
static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ── main ────────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);
    signal(SIGPIPE, SIG_IGN);

    if (mlockall(MCL_CURRENT | MCL_FUTURE) != 0)
        fprintf(stderr, "[rtp_recv] mlockall: %s\n", strerror(errno));

    const char *key = argc > 1 ? argv[1] : "rtp_in";

    char sock_path[256];
    snprintf(sock_path, sizeof(sock_path), "/run/aoip/rtp_recv_%s.sock", key);
    int sfd = unix_connect(sock_path, 30, 100);
    if (sfd < 0) {
        fprintf(stderr, "[rtp_recv] cannot connect to %s\n", sock_path);
        return 1;
    }

    char cfg[512] = "";
    read_line_fd(sfd, cfg, sizeof(cfg));

    int port  = cfg_int(cfg, "port", 5004);
    g_ch      = cfg_int(cfg, "channels", 2);
    if (g_ch < 1) g_ch = 1;
    if (g_ch > SHM_MAX_CH) g_ch = SHM_MAX_CH;
    g_buf_ms  = cfg_int(cfg, "bufMs", 100);
    if (g_buf_ms < 10)   g_buf_ms = 10;
    if (g_buf_ms > 2000) g_buf_ms = 2000;
    g_in_rate = cfg_int(cfg, "rate", 48000);
    if (g_in_rate <= 0)  g_in_rate = 48000;

    char proto_str[16];
    cfg_str(cfg, "proto", proto_str, sizeof(proto_str), "rtp");
    g_proto = (strcmp(proto_str, "raw") == 0) ? PROTO_RAW : PROTO_RTP;
    cfg_str(cfg, "addr", g_bind_addr, sizeof(g_bind_addr), "0.0.0.0");
    cfg_str(cfg, "shm",  g_shm_name,  sizeof(g_shm_name),  "");
    if (!g_shm_name[0])
        snprintf(g_shm_name, sizeof(g_shm_name), "/%s", key);

    /* socket → stderr (stats, ready 등 모든 출력) */
    dup2(sfd, STDERR_FILENO);
    close(sfd);

    if (!shm_attach()) return 1;

    /* jitter buffer: wp를 bufMs만큼 선행 이동 → MP3 같은 대형 패킷에도 무음 없음 */
    if (g_buf_ms > 0) {
        int pre = (int)((long long)g_buf_ms * OUT_RATE / 1000);
        /* 링 버퍼 크기를 초과하지 않도록 안전 마진(1024프레임) 확보 */
        int max_pre = SHM_RING_FRAMES * 3 / 4;  /* 75%: 나머지 25%는 burst 여유 */
        if (pre > max_pre) pre = max_pre;
        uint32_t wp0 = atomic_load_explicit(&g_shm->wp, memory_order_relaxed);
        atomic_store_explicit(&g_shm->wp, wp0 + (uint32_t)pre, memory_order_release);
        fprintf(stderr, "[rtp_recv] jitter buffer: %dms (%d frames pre-buffered)\n", g_buf_ms, pre);
    }

    /* init decoders */
    g_hip = hip_decode_init();
    build_g711_tables();

    /* ── UDP socket ──────────────────────────────────── */
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) { perror("[rtp_recv] socket"); return 1; }

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    /* 1-second receive timeout so recv_thread can check g_quit */
    struct timeval tv = { .tv_sec = 1, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    /* increase OS receive buffer */
    int rcvbuf = 2 * 1024 * 1024;
    setsockopt(sock, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));

    /* 멀티캐스트(224.0.0.0/4) 여부를 먼저 판단 후 바인드 주소 결정 */
    struct in_addr bind_in = { .s_addr = INADDR_ANY };
    int is_multicast = 0;
    if (g_bind_addr[0] && inet_aton(g_bind_addr, &bind_in)) {
        uint32_t ba = ntohl(bind_in.s_addr);
        is_multicast = (ba >= 0xE0000000u && ba <= 0xEFFFFFFFu);
    }

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port   = htons((uint16_t)port),
        /* 멀티캐스트: 그룹 주소로 바인드 → 유니캐스트 패킷 수신 차단
         * 유니캐스트: INADDR_ANY                                       */
        .sin_addr.s_addr = is_multicast ? bind_in.s_addr : INADDR_ANY,
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        perror("[rtp_recv] bind"); close(sock); return 1;
    }

    /* 멀티캐스트 그룹 join */
    if (is_multicast) {
        struct ip_mreq mreq;
        mreq.imr_multiaddr        = bind_in;
        mreq.imr_interface.s_addr = INADDR_ANY;
        if (setsockopt(sock, IPPROTO_IP, IP_ADD_MEMBERSHIP,
                       &mreq, sizeof(mreq)) < 0)
            fprintf(stderr, "[rtp_recv] multicast join failed: %s\n", strerror(errno));
        else
            fprintf(stderr, "[rtp_recv] multicast joined %s\n", g_bind_addr);
    }

    fprintf(stderr, "[rtp_recv] mode=%s port=%d ch=%d bufMs=%d addr=%s\n",
            proto_str, port, g_ch, g_buf_ms, g_bind_addr);

    pthread_t recv_tid, decode_tid, stats_tid;
    pthread_create(&recv_tid,   NULL, recv_thread,   &sock);
    pthread_create(&decode_tid, NULL, decode_thread, NULL);
    pthread_create(&stats_tid,  NULL, stats_thread,  NULL);

    fprintf(stderr, "[rtp_recv] ready\n");
    fflush(stderr);

    while (!g_quit) usleep(50000);

    g_quit = 1;
    pthread_cond_broadcast(&g_pkt_cond);

    pthread_join(recv_tid,   NULL);
    pthread_join(decode_tid, NULL);
    pthread_join(stats_tid,  NULL);

    if (g_hip)      { hip_decode_exit(g_hip); g_hip = NULL; }
    if (g_src_state) { src_delete(g_src_state); g_src_state = NULL; }
    if (g_shm)     { munmap(g_shm, SHMRING_SIZE); g_shm = NULL; }
    if (g_shm_fd >= 0) { close(g_shm_fd); g_shm_fd = -1; }
    close(sock);

    fprintf(stderr, "[rtp_recv] exiting\n");
    return g_exit_code;
}
