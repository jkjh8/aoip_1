/*
 * rtp_recv.c — UDP stream receiver → POSIX shared memory ring (ShmRing)
 *
 * GStreamer:  udpsrc → [depay] → audioconvert → audioresample → appsink
 * Bridge:     appsink → ShmRing(F32LE interleaved) → aoip_engine
 *
 * Usage:  rtp_recv <port> <channels> <proto> <name> <bufMs> <rate> <enc> <addr> shm <shm_name>
 *
 * Stderr:
 *   [rtp_recv] ready
 *   stats codec=... bufMs=N packets=N drops=N ...
 *
 * ShmRing 레이아웃 (aoip_engine.c 와 동일):
 *   wp, rp  : _Atomic uint32_t  — 단조 증가 frame 카운터
 *   channels: int32_t
 *   ring_frames: int32_t
 *   _pad    : 48 bytes
 *   buf[]   : float[SHM_RING_FRAMES * SHM_MAX_CH]
 */

#include <gst/gst.h>
#include <gst/net/net.h>
#include <gst/app/gstappsink.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

/* ── ShmRing (aoip_engine.c 와 동일한 레이아웃) ──────── */
#define SHM_RING_FRAMES  16384
#define SHM_MAX_CH       8

typedef struct {
    _Atomic uint32_t wp;
    _Atomic uint32_t rp;
    int32_t  channels;
    int32_t  ring_frames;
    uint8_t  _pad[48];
    float    buf[SHM_RING_FRAMES * SHM_MAX_CH];
} ShmRing;

#define SHMRING_SIZE ((size_t)sizeof(ShmRing))

/* ── globals ─────────────────────────────────────── */
static int             g_ch      = 2;
static int             g_rate    = 48000;
static int             g_buf_ms  = 100;

static volatile int    g_quit      = 0;
static volatile int    g_exit_code = 0;
static int             g_auto_detect = 0;
static GstElement     *g_pipeline;

typedef enum { PROTO_RAW = 0, PROTO_PCM, PROTO_RTP } ProtoMode;
static ProtoMode g_proto = PROTO_RAW;
static int       g_sample_rate_in = 48000;
static char      g_encoding_name[32] = "L16";
static char      g_bind_address[64]  = "0.0.0.0";

static char g_codec[64]  = "unknown";
static atomic_ulong g_packets   = 0;
static atomic_ulong g_drops     = 0;
static atomic_ulong g_udp_bytes = 0;
static char g_src_ip[64] = "";
static int  g_src_port   = 0;
static pthread_mutex_t g_addr_mtx = PTHREAD_MUTEX_INITIALIZER;

/* ── 공유 메모리 출력 ─────────────────────────────── */
static char     g_shm_name[256] = "";
static int      g_shm_fd        = -1;
static ShmRing *g_shm           = NULL;

static int shm_attach(void) {
    /* aoip_engine이 이미 shm_open+ftruncate 완료했을 때까지 재시도 (최대 5초) */
    for (int i = 0; i < 50; i++) {
        g_shm_fd = shm_open(g_shm_name, O_RDWR, 0);
        if (g_shm_fd >= 0) break;
        usleep(100000);
    }
    if (g_shm_fd < 0) {
        fprintf(stderr, "[rtp_recv] shm_open(%s) failed: %s\n",
                g_shm_name, strerror(errno));
        return 0;
    }
    g_shm = mmap(NULL, SHMRING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (g_shm == MAP_FAILED) {
        fprintf(stderr, "[rtp_recv] mmap(%s) failed: %s\n", g_shm_name, strerror(errno));
        close(g_shm_fd); g_shm_fd = -1; g_shm = NULL;
        return 0;
    }
    fprintf(stderr, "[rtp_recv] attached shm %s\n", g_shm_name);
    return 1;
}

/* ── appsink callback: F32LE interleaved → ShmRing ── */
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer data)
{
    (void)data;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        if (g_shm) {
            int frames = (int)(map.size / (size_t)(g_ch * sizeof(float)));
            const float *src = (const float *)map.data;

            for (int f = 0; f < frames; f++) {
                uint32_t wp  = atomic_load_explicit(&g_shm->wp, memory_order_relaxed);
                uint32_t idx = wp % (uint32_t)SHM_RING_FRAMES;
                for (int c = 0; c < g_ch && c < SHM_MAX_CH; c++)
                    g_shm->buf[idx * SHM_MAX_CH + c] = src[f * g_ch + c];
                atomic_store_explicit(&g_shm->wp, wp + 1u, memory_order_release);
            }
        }
        atomic_fetch_add_explicit(&g_packets, 1, memory_order_relaxed);
        gst_buffer_unmap(buffer, &map);
    }
    gst_sample_unref(sample);
    return GST_FLOW_OK;
}

/* ── decodebin pad-added ─────────────────────────── */
static void on_pad_added(GstElement *el, GstPad *pad, gpointer data)
{
    (void)el;
    GstElement *convert = (GstElement *)data;
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, NULL);
    if (caps) {
        const char *mime = gst_structure_get_name(gst_caps_get_structure(caps, 0));
        if (mime && strstr(mime, "audio")) {
            if (strstr(mime, "mpeg"))         snprintf(g_codec, sizeof(g_codec), "mp3");
            else if (strstr(mime, "opus"))    snprintf(g_codec, sizeof(g_codec), "opus");
            else if (strstr(mime, "vorbis"))  snprintf(g_codec, sizeof(g_codec), "vorbis");
            else if (strstr(mime, "aac"))     snprintf(g_codec, sizeof(g_codec), "aac");
            else if (strstr(mime, "raw"))     snprintf(g_codec, sizeof(g_codec), "raw");
            else snprintf(g_codec, sizeof(g_codec), "%s", mime);

            GstPad *sinkpad = gst_element_get_static_pad(convert, "sink");
            if (sinkpad && !gst_pad_is_linked(sinkpad))
                gst_pad_link(pad, sinkpad);
            if (sinkpad) gst_object_unref(sinkpad);
        }
        gst_caps_unref(caps);
    }
}

static gboolean bus_msg(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus; (void)data;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL; gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        fprintf(stderr, "[rtp_recv] gst error: %s (%s)\n",
                err->message, dbg ? dbg : "");
        g_error_free(err); g_free(dbg);
        g_exit_code = 1; g_quit = 1;
        break;
    }
    case GST_MESSAGE_TAG: {
        GstTagList *tags = NULL;
        gst_message_parse_tag(msg, &tags);
        if (tags) {
            gchar *codec_str = NULL;
            if (gst_tag_list_get_string(tags, GST_TAG_AUDIO_CODEC, &codec_str) && codec_str) {
                snprintf(g_codec, sizeof(g_codec), "%s", codec_str);
                g_free(codec_str);
            }
            gst_tag_list_unref(tags);
        }
        break;
    }
    default: break;
    }
    return TRUE;
}

/* ── UDP pad probe ───────────────────────────────── */
static GstPadProbeReturn udp_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    (void)pad; (void)data;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_OK;

    atomic_fetch_add_explicit(&g_udp_bytes, gst_buffer_get_size(buf), memory_order_relaxed);

    GstNetAddressMeta *meta = gst_buffer_get_net_address_meta(buf);
    if (meta && meta->addr && G_IS_INET_SOCKET_ADDRESS(meta->addr)) {
        GInetAddress *inet = g_inet_socket_address_get_address(G_INET_SOCKET_ADDRESS(meta->addr));
        gchar *ip  = g_inet_address_to_string(inet);
        int    prt = (int)g_inet_socket_address_get_port(G_INET_SOCKET_ADDRESS(meta->addr));
        pthread_mutex_lock(&g_addr_mtx);
        snprintf(g_src_ip, sizeof(g_src_ip), "%s", ip);
        g_src_port = prt;
        pthread_mutex_unlock(&g_addr_mtx);
        g_free(ip);
    }
    return GST_PAD_PROBE_OK;
}

/* ── stats thread ────────────────────────────────── */
static void *stats_thread(void *arg)
{
    (void)arg;
    unsigned long prev_udp = 0;
    while (!g_quit) {
        sleep(2);
        unsigned long cur_udp = atomic_load(&g_udp_bytes);
        int bitrate_kbps = (int)((cur_udp - prev_udp) * 8 / 2 / 1000);
        int has_data = (cur_udp != prev_udp);
        prev_udp = cur_udp;
        if (!has_data) {
            pthread_mutex_lock(&g_addr_mtx);
            g_src_ip[0] = '\0'; g_src_port = 0;
            pthread_mutex_unlock(&g_addr_mtx);
            snprintf(g_codec, sizeof(g_codec), "unknown");
        }
        char src_ip[64]; int src_port;
        pthread_mutex_lock(&g_addr_mtx);
        snprintf(src_ip, sizeof(src_ip), "%s", g_src_ip[0] ? g_src_ip : "none");
        src_port = g_src_port;
        pthread_mutex_unlock(&g_addr_mtx);
        unsigned long pkts  = atomic_load(&g_packets);
        unsigned long drops = atomic_load(&g_drops);
        fprintf(stderr,
            "stats codec=%s bufMs=0 packets=%lu drops=%lu srcIp=%s srcPort=%d bitrateKbps=%d\n",
            g_codec, pkts, drops, src_ip, src_port, bitrate_kbps);
        fflush(stderr);
    }
    return NULL;
}

/* ── signal handler ──────────────────────────────── */
static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ── AUTO 모드: 실시간 PT 변경 감지 프로브 ─────────── */
static GstPadProbeReturn auto_pt_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    (void)pad; (void)data;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (!buf) return GST_PAD_PROBE_PASS;
    GstMapInfo map;
    if (!gst_buffer_map(buf, &map, GST_MAP_READ)) return GST_PAD_PROBE_PASS;
    static uint8_t last_pt = 0xFF;
    if (map.size < 12) { gst_buffer_unmap(buf, &map); return GST_PAD_PROBE_PASS; }
    uint8_t pt = map.data[1] & 0x7F;
    if (pt == last_pt) { gst_buffer_unmap(buf, &map); return GST_PAD_PROBE_PASS; }
    last_pt = pt;
    char new_enc[32] = ""; int new_rate = g_sample_rate_in;
    switch (pt) {
        case 0:  snprintf(new_enc, sizeof(new_enc), "PCMU"); new_rate = 8000;  break;
        case 8:  snprintf(new_enc, sizeof(new_enc), "PCMA"); new_rate = 8000;  break;
        case 10: snprintf(new_enc, sizeof(new_enc), "L16");  new_rate = 44100; break;
        case 11: snprintf(new_enc, sizeof(new_enc), "L16");  new_rate = 44100; break;
        case 14: snprintf(new_enc, sizeof(new_enc), "MPA");                    break;
        default: {
            int cc = map.data[0] & 0x0F, has_ext = (map.data[0] >> 4) & 0x1;
            int hdr = 12 + cc * 4;
            if (has_ext && map.size >= (size_t)(hdr + 4)) {
                int ew = (map.data[hdr+2] << 8) | map.data[hdr+3]; hdr += 4 + ew * 4;
            }
            if (hdr < (int)map.size) {
                const uint8_t *pl = map.data + hdr; int plen = (int)map.size - hdr;
                if (plen >= 6 && pl[4] == 0xFF && (pl[5] & 0xE0) == 0xE0)
                    snprintf(new_enc, sizeof(new_enc), "MPA");
                else
                    snprintf(new_enc, sizeof(new_enc), plen > 4000 ? "L24" : "L16");
            }
            break;
        }
    }
    gst_buffer_unmap(buf, &map);
    if (!new_enc[0]) return GST_PAD_PROBE_PASS;
    if (strcmp(new_enc, g_encoding_name) != 0 || new_rate != g_sample_rate_in) {
        fprintf(stderr, "[rtp_recv] auto-codec: %s %d\n", new_enc, new_rate);
        fflush(stderr);
        g_exit_code = 2; g_quit = 1;
    }
    return GST_PAD_PROBE_PASS;
}

/* ── RTP codec auto-detection ────────────────────── */
static void detect_rtp_codec(int port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;
    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));
    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    struct sockaddr_in addr; memset(&addr, 0, sizeof(addr));
    addr.sin_family = AF_INET; addr.sin_port = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) { close(sock); return; }
    uint8_t buf[4096];
    ssize_t len = recv(sock, buf, sizeof(buf), 0);
    close(sock);
    if (len < 12) return;
    uint8_t pt = buf[1] & 0x7F, cc = buf[0] & 0x0F;
    int has_ext = (buf[0] >> 4) & 0x1;
    fprintf(stderr, "[rtp_recv] auto-detect: PT=%d\n", pt);
    switch (pt) {
        case 0:  snprintf(g_encoding_name, sizeof(g_encoding_name), "PCMU"); return;
        case 8:  snprintf(g_encoding_name, sizeof(g_encoding_name), "PCMA"); return;
        case 10: snprintf(g_encoding_name, sizeof(g_encoding_name), "L16"); g_sample_rate_in = 44100; return;
        case 11: snprintf(g_encoding_name, sizeof(g_encoding_name), "L16"); g_sample_rate_in = 44100; return;
        case 14: snprintf(g_encoding_name, sizeof(g_encoding_name), "MPA"); return;
        default: break;
    }
    int hdr = 12 + cc * 4;
    if (has_ext && len > hdr + 4) { int ew = (buf[hdr+2]<<8)|buf[hdr+3]; hdr += 4 + ew*4; }
    if (hdr >= (int)len) return;
    const uint8_t *payload = buf + hdr; int payload_len = (int)len - hdr;
    if (payload_len >= 6 && payload[4] == 0xFF && (payload[5] & 0xE0) == 0xE0) {
        snprintf(g_encoding_name, sizeof(g_encoding_name), "MPA");
        fprintf(stderr, "[rtp_recv] auto-detect: MPA(MP3) from payload\n"); return;
    }
    if (payload_len > 4000) {
        snprintf(g_encoding_name, sizeof(g_encoding_name), "L24");
        g_sample_rate_in = 48000;
        fprintf(stderr, "[rtp_recv] auto-detect: L24 (payload=%d)\n", payload_len);
    } else {
        snprintf(g_encoding_name, sizeof(g_encoding_name), "L16");
        fprintf(stderr, "[rtp_recv] auto-detect: L16 (payload=%d)\n", payload_len);
    }
}

/* ── main ─────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    int port = argc > 1 ? atoi(argv[1]) : 5004;
    g_ch     = argc > 2 ? atoi(argv[2]) : 2;

    const char *proto_arg = argc > 3 ? argv[3] : "raw";
    if      (strcmp(proto_arg, "pcm") == 0) g_proto = PROTO_PCM;
    else if (strcmp(proto_arg, "rtp") == 0) g_proto = PROTO_RTP;
    else                                    g_proto = PROTO_RAW;

    g_sample_rate_in = argc > 6 ? atoi(argv[6]) : 48000;
    if (g_sample_rate_in <= 0) g_sample_rate_in = 48000;

    if (g_proto == PROTO_RTP && argc > 7)
        snprintf(g_encoding_name, sizeof(g_encoding_name), "%s", argv[7]);

    if (g_proto == PROTO_RTP) {
        g_auto_detect = 1;
        if (strcmp(g_encoding_name, "AUTO") == 0) {
            snprintf(g_encoding_name, sizeof(g_encoding_name), "L16");
            detect_rtp_codec(port);
            fprintf(stderr, "[rtp_recv] AUTO mode: using encoding=%s rate=%d\n",
                    g_encoding_name, g_sample_rate_in);
        }
    }

    char client_name[64];
    snprintf(client_name, sizeof(client_name), "%s", argc > 4 ? argv[4] : "rtp_in");
    g_buf_ms = argc > 5 ? atoi(argv[5]) : 100;
    if (g_buf_ms < 10)  g_buf_ms = 10;
    if (g_buf_ms > 500) g_buf_ms = 500;

    if (argc > 8 && argv[8][0] != '\0')
        snprintf(g_bind_address, sizeof(g_bind_address), "%s", argv[8]);

    /* argv[9]="shm"  argv[10]=shm_name */
    if (argc > 9 && strcmp(argv[9], "shm") == 0 && argc > 10)
        snprintf(g_shm_name, sizeof(g_shm_name), "%s", argv[10]);

    if (!g_shm_name[0]) {
        fprintf(stderr, "[rtp_recv] shm name required (argv[9]=shm argv[10]=<name>)\n");
        return 1;
    }

    if (!shm_attach()) return 1;

    g_rate = g_sample_rate_in > 0 ? g_sample_rate_in : 48000;
    fprintf(stderr, "[rtp_recv] shm mode → %s\n", g_shm_name);

    const char *mode_str = g_proto == PROTO_PCM ? "pcm" :
                           g_proto == PROTO_RTP  ? "rtp" : "raw";
    fprintf(stderr, "[rtp_recv] mode=%s\n", mode_str);
    if (g_proto == PROTO_PCM) fprintf(stderr, "[rtp_recv] input rate=%d\n", g_sample_rate_in);
    if (g_proto == PROTO_RTP) fprintf(stderr, "[rtp_recv] rtp encoding=%s clock-rate=%d\n",
            g_encoding_name, g_sample_rate_in);

    GstElement *src      = gst_element_factory_make("udpsrc",        "src");
    GstElement *convert  = gst_element_factory_make("audioconvert",  "convert");
    GstElement *resample = gst_element_factory_make("audioresample", "resample");
    GstElement *caps_flt = gst_element_factory_make("capsfilter",    "caps");
    GstElement *appsink  = gst_element_factory_make("appsink",       "sink");
    GstElement *decode   = (g_proto == PROTO_RAW) ?
                           gst_element_factory_make("decodebin", "decode") : NULL;

    if (g_proto == PROTO_PCM) snprintf(g_codec, sizeof(g_codec), "pcm-s16le");
    if (g_proto == PROTO_RTP) snprintf(g_codec, sizeof(g_codec), "%s", g_encoding_name);

    int ok = src && convert && resample && caps_flt && appsink;
    if (g_proto == PROTO_RAW) ok = ok && decode;
    if (!ok) { fprintf(stderr, "[rtp_recv] failed to create GStreamer elements\n"); return 1; }

    g_pipeline = gst_pipeline_new("rtp_recv");

    g_object_set(src, "port", port, "address", g_bind_address,
                 "retrieve-sender-address", TRUE, NULL);
    fprintf(stderr, "[rtp_recv] udpsrc port=%d address=%s\n", port, g_bind_address);

    /* capsfilter: F32LE interleaved @ 48kHz */
    GstCaps *caps = gst_caps_new_simple("audio/x-raw",
        "format",   G_TYPE_STRING, "F32LE",
        "rate",     G_TYPE_INT,    48000,   /* aoip_engine は常に 48kHz */
        "channels", G_TYPE_INT,    g_ch,
        "layout",   G_TYPE_STRING, "interleaved", NULL);
    g_object_set(caps_flt, "caps", caps, NULL);
    gst_caps_unref(caps);

    g_object_set(appsink, "emit-signals", TRUE, "sync", FALSE,
                 "max-buffers", 8, "drop", TRUE, NULL);
    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    int link_ok = 1;
    if (g_proto == PROTO_RTP) {
        GstElement *jitter = gst_element_factory_make("rtpjitterbuffer", "jitter");
        if (!jitter) { fprintf(stderr, "[rtp_recv] failed to create rtpjitterbuffer\n"); gst_object_unref(g_pipeline); return 1; }
        g_object_set(jitter, "latency", (guint)g_buf_ms, NULL);
        int rtp_needs_decode = (strcmp(g_encoding_name, "MPA")  == 0 ||
                                strcmp(g_encoding_name, "OPUS") == 0);
        const char *depay_name =
            (strcmp(g_encoding_name, "L16")  == 0) ? "rtpL16depay"  :
            (strcmp(g_encoding_name, "MPA")  == 0) ? "rtpmpadepay"  :
            (strcmp(g_encoding_name, "OPUS") == 0) ? "rtpopusdepay" :
            (strcmp(g_encoding_name, "PCMA") == 0) ? "rtppcmadepay" :
            (strcmp(g_encoding_name, "PCMU") == 0) ? "rtppcmudepay" :
                                                      "rtpL24depay";
        GstElement *depay = gst_element_factory_make(depay_name, "depay");
        GstElement *rtp_decode = rtp_needs_decode ?
                                 gst_element_factory_make("decodebin", "rtp_decode") : NULL;
        if (!depay || (rtp_needs_decode && !rtp_decode)) {
            fprintf(stderr, "[rtp_recv] failed to create depay (%s)\n", depay_name);
            gst_object_unref(g_pipeline); return 1;
        }
        char ch_str[8]; snprintf(ch_str, sizeof(ch_str), "%d", g_ch);
        GstCaps *rtp_caps = gst_caps_new_simple("application/x-rtp",
            "media", G_TYPE_STRING, "audio",
            "clock-rate", G_TYPE_INT, g_sample_rate_in,
            "encoding-name", G_TYPE_STRING, g_encoding_name,
            "encoding-params", G_TYPE_STRING, ch_str,
            "channels", G_TYPE_INT, g_ch, NULL);
        g_object_set(src, "caps", rtp_caps, NULL);
        gst_caps_unref(rtp_caps);
        if (rtp_needs_decode) {
            g_signal_connect(rtp_decode, "pad-added", G_CALLBACK(on_pad_added), convert);
            gst_bin_add_many(GST_BIN(g_pipeline), src, jitter, depay, rtp_decode, convert, resample, caps_flt, appsink, NULL);
            link_ok = gst_element_link_many(src, jitter, depay, rtp_decode, NULL) &&
                      gst_element_link_many(convert, resample, caps_flt, appsink, NULL);
        } else {
            gst_bin_add_many(GST_BIN(g_pipeline), src, jitter, depay, convert, resample, caps_flt, appsink, NULL);
            link_ok = gst_element_link_many(src, jitter, depay, convert, resample, caps_flt, appsink, NULL);
        }
    } else if (g_proto == PROTO_PCM) {
        GstCaps *c = gst_caps_new_simple("audio/x-raw",
            "format", G_TYPE_STRING, "S16LE",
            "rate", G_TYPE_INT, g_sample_rate_in,
            "channels", G_TYPE_INT, g_ch,
            "layout", G_TYPE_STRING, "interleaved", NULL);
        g_object_set(src, "caps", c, NULL); gst_caps_unref(c);
        gst_bin_add_many(GST_BIN(g_pipeline), src, convert, resample, caps_flt, appsink, NULL);
        link_ok = gst_element_link_many(src, convert, resample, caps_flt, appsink, NULL);
    } else {
        g_signal_connect(decode, "pad-added", G_CALLBACK(on_pad_added), convert);
        gst_bin_add_many(GST_BIN(g_pipeline), src, decode, convert, resample, caps_flt, appsink, NULL);
        link_ok = gst_element_link(src, decode) &&
                  gst_element_link_many(convert, resample, caps_flt, appsink, NULL);
    }
    if (!link_ok) {
        fprintf(stderr, "[rtp_recv] pipeline link failed (mode=%s)\n", mode_str);
        gst_object_unref(g_pipeline); return 1;
    }

    GstBus *bus = gst_element_get_bus(g_pipeline);
    gst_bus_add_watch(bus, bus_msg, NULL);
    gst_object_unref(bus);

    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);

    {
        GstPad *src_pad = gst_element_get_static_pad(src, "src");
        if (src_pad) {
            gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, udp_probe, NULL, NULL);
            if (g_auto_detect)
                gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, auto_pt_probe, NULL, NULL);
            gst_object_unref(src_pad);
        }
    }

    fprintf(stderr, "[rtp_recv] ready\n");
    fflush(stderr);

    pthread_t stats_tid;
    pthread_create(&stats_tid, NULL, stats_thread, NULL);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    while (!g_quit) {
        g_main_context_iteration(NULL, FALSE);
        usleep(1000);
    }
    g_main_loop_unref(loop);

    g_quit = 1;
    pthread_join(stats_tid, NULL);

    gst_element_set_state(g_pipeline, GST_STATE_NULL);
    gst_object_unref(g_pipeline);

    if (g_shm) { munmap(g_shm, SHMRING_SIZE); g_shm = NULL; }
    if (g_shm_fd >= 0) { close(g_shm_fd); g_shm_fd = -1; }
    /* shm_unlink은 aoip_engine(owner)이 담당 */

    fprintf(stderr, "[rtp_recv] exiting\n");
    return g_exit_code;
}
