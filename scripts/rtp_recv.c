/*
 * rtp_recv.c — UDP stream receiver → JACK output ports
 *
 * JACK client:  rtp_in
 * JACK ports:   rtp_in:out_1 … rtp_in:out_N  (output/source ports)
 *
 * GStreamer:    udpsrc → decodebin → audioconvert → audioresample → appsink
 * Bridge:       appsink → ring buffer → JACK process callback
 *
 * Usage:   rtp_recv <port> [channels=2] [bufferMs=100]
 *
 * Stdout (parsed by Node.js):
 *   [rtp_recv] ready
 *   ports: rtp_in:out_1, rtp_in:out_2
 *   stats codec=mp3 bufMs=N packets=N drops=N
 */

#include <gst/gst.h>
#include <gst/net/net.h>
#include <gst/app/gstappsink.h>
#include <jack/jack.h>
#include <gio/gio.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>



#define MAX_CH       2
#define RING_FRAMES  16384   /* must be power of 2 */

typedef enum { PROTO_RAW = 0, PROTO_PCM, PROTO_RTP } ProtoMode;
static ProtoMode g_proto = PROTO_RAW;
static int       g_sample_rate_in = 44100;  /* input sample rate (PCM/RTP mode) */
static char      g_encoding_name[32] = "L16"; /* RTP encoding name (L24, L16, OPUS, ...) */
static char      g_bind_address[64]  = "0.0.0.0"; /* bind/multicast address */

/* ── ring buffer ─────────────────────────────────── */
typedef struct {
    float       buf[RING_FRAMES * MAX_CH];
    atomic_uint wp;      /* write pointer (frames) */
    atomic_uint rp;      /* read pointer  (frames) */
    int         ch;
} Ring;

static Ring ring;

static void ring_write(const float *src, int nframes)
{
    unsigned wp = atomic_load_explicit(&ring.wp, memory_order_relaxed);
    unsigned rp = atomic_load_explicit(&ring.rp, memory_order_acquire);
    //if ((int)(RING_FRAMES - (wp - rp)) < nframes) {
    //    atomic_fetch_add_explicit(&ring.rp,
    //        (unsigned)nframes, memory_order_release); /* discard oldest */
    //}
    unsigned free = RING_FRAMES - (wp - rp);
    if ((int)free < nframes) {
        unsigned drop = nframes - free;
        atomic_fetch_add_explicit(&ring.rp, drop, memory_order_release);
    }
    for (int i = 0; i < nframes; i++) {
        unsigned idx = (wp + i) & (RING_FRAMES - 1);
        memcpy(&ring.buf[idx * ring.ch], &src[i * ring.ch],
               (unsigned)ring.ch * sizeof(float));
    }
    atomic_store_explicit(&ring.wp, wp + (unsigned)nframes, memory_order_release);
}

/* returns 1 on success, 0 on underrun */
static int ring_read(float *dst, int nframes)
{
    unsigned rp = atomic_load_explicit(&ring.rp, memory_order_relaxed);
    unsigned wp = atomic_load_explicit(&ring.wp, memory_order_acquire);
    if ((int)(wp - rp) < nframes) return 0;
    for (int i = 0; i < nframes; i++) {
        unsigned idx = (rp + i) & (RING_FRAMES - 1);
        memcpy(&dst[i * ring.ch], &ring.buf[idx * ring.ch],
               (unsigned)ring.ch * sizeof(float));
    }
    atomic_store_explicit(&ring.rp, rp + (unsigned)nframes, memory_order_release);
    return 1;
}

/* ── globals ─────────────────────────────────────── */
static jack_client_t  *g_jclient;
static jack_port_t    *jports[MAX_CH];
static int             g_ch      = 2;
static int             g_rate    = 44100;
static int             g_buf_ms  = 100;   /* pre-fill target in ms */

static volatile int    g_quit = 0;
static volatile int    g_exit_code = 0;
static int             g_auto_detect = 0;  /* 1 = AUTO 모드, 실시간 PT 감지 활성 */
static atomic_int      g_prebuffered = 0;  /* 1 = pre-fill 완료, 재생 중 */
static GstElement     *g_pipeline;

static char            g_codec[64]  = "unknown";
static atomic_ulong    g_packets    = 0;
static atomic_ulong    g_drops      = 0;
static atomic_ulong    g_udp_bytes  = 0;   /* UDP-level bytes */
static char            g_src_ip[64] = "";
static int             g_src_port   = 0;
static pthread_mutex_t g_addr_mtx   = PTHREAD_MUTEX_INITIALIZER;

/* ── JACK process callback ───────────────────────── */
static int jack_process(jack_nframes_t nframes, void *arg)
{
    (void)arg;
    // float tmp[4096 * MAX_CH];
    static float tmp[RING_FRAMES * MAX_CH];
    unsigned wp = atomic_load_explicit(&ring.wp, memory_order_acquire);
    unsigned rp = atomic_load_explicit(&ring.rp, memory_order_relaxed);
    int available = (int)(wp - rp);

    /* 아직 pre-fill 안 됐으면 무음 대기 */
    if (!atomic_load_explicit(&g_prebuffered, memory_order_relaxed)) {
        int prefill = (int)((float)g_buf_ms / 1000.0f * (float)g_rate);
        if (available < prefill) {
            for (int c = 0; c < g_ch; c++)
                memset(jack_port_get_buffer(jports[c], nframes), 0, nframes * sizeof(float));
            return 0;
        }
        atomic_store_explicit(&g_prebuffered, 1, memory_order_relaxed);
    }

    /* pre-fill 완료 → underrun 전까지 계속 재생 */
    if (ring_read(tmp, (int)nframes)) {
        for (int c = 0; c < g_ch; c++) {
            float *out = jack_port_get_buffer(jports[c], nframes);
            for (jack_nframes_t f = 0; f < nframes; f++)
                out[f] = tmp[f * g_ch + c];
        }
    } else {
        /* 실제 underrun: 무음 출력 + pre-fill 재트리거 */
        atomic_store_explicit(&g_prebuffered, 0, memory_order_relaxed);
        atomic_fetch_add_explicit(&g_drops, 1, memory_order_relaxed);
        for (int c = 0; c < g_ch; c++)
            memset(jack_port_get_buffer(jports[c], nframes), 0, nframes * sizeof(float));
    }
    return 0;
}

/* ── GStreamer appsink callback ──────────────────── */
static GstFlowReturn on_new_sample(GstAppSink *sink, gpointer data)
{
    (void)data;
    GstSample *sample = gst_app_sink_pull_sample(sink);
    if (!sample) return GST_FLOW_ERROR;

    GstBuffer *buffer = gst_sample_get_buffer(sample);
    GstMapInfo map;
    if (gst_buffer_map(buffer, &map, GST_MAP_READ)) {
        int nframes = (int)(map.size / (sizeof(float) * (unsigned)g_ch));
        ring_write((const float *)map.data, nframes);
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

    /* only link audio pads */
    GstCaps *caps = gst_pad_get_current_caps(pad);
    if (!caps) caps = gst_pad_query_caps(pad, NULL);
    if (caps) {
        const char *mime = gst_structure_get_name(gst_caps_get_structure(caps, 0));
        if (mime && strstr(mime, "audio")) {
            /* detect codec from upstream tags — use mime for a rough guess */
            if (strstr(mime, "mpeg"))    snprintf(g_codec, sizeof(g_codec), "mp3");
            else if (strstr(mime, "opus"))   snprintf(g_codec, sizeof(g_codec), "opus");
            else if (strstr(mime, "vorbis")) snprintf(g_codec, sizeof(g_codec), "vorbis");
            else if (strstr(mime, "aac"))    snprintf(g_codec, sizeof(g_codec), "aac");
            else if (strstr(mime, "raw"))    snprintf(g_codec, sizeof(g_codec), "raw");
            else snprintf(g_codec, sizeof(g_codec), "%s", mime);

            GstPad *sinkpad = gst_element_get_static_pad(convert, "sink");
            if (sinkpad && !gst_pad_is_linked(sinkpad))
                gst_pad_link(pad, sinkpad);
            if (sinkpad) gst_object_unref(sinkpad);
        }
        gst_caps_unref(caps);
    }
}

/* also catch tag messages for codec info */
static gboolean bus_msg(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus; (void)data;
    switch (GST_MESSAGE_TYPE(msg)) {
    case GST_MESSAGE_ERROR: {
        GError *err = NULL;
        gchar  *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        fprintf(stderr, "[rtp_recv] gst error: %s (%s)\n",
                err->message, dbg ? dbg : "");
        g_error_free(err);
        g_free(dbg);
        g_exit_code = 1;
        g_quit = 1;
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
    default:
        break;
    }
    return TRUE;
}

/* ── UDP pad probe: sender address + byte count ────── */
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

        /* bitrate from UDP-level bytes (encoded stream) */
        unsigned long cur_udp = atomic_load(&g_udp_bytes);
        int bitrate_kbps = (int)((cur_udp - prev_udp) * 8 / 2 / 1000);
        int has_data = (cur_udp != prev_udp);
        prev_udp = cur_udp;

        /* no data this interval → clear sender info + flush ring buffer */
        if (!has_data) {
            pthread_mutex_lock(&g_addr_mtx);
            g_src_ip[0] = '\0';
            g_src_port  = 0;
            pthread_mutex_unlock(&g_addr_mtx);
            snprintf(g_codec, sizeof(g_codec), "unknown");
            /* 버퍼 비우기: rp를 wp로 이동, prebuffered 리셋 */
            unsigned wp_now = atomic_load_explicit(&ring.wp, memory_order_acquire);
            atomic_store_explicit(&ring.rp, wp_now, memory_order_release);
            atomic_store_explicit(&g_prebuffered, 0, memory_order_relaxed);
        }

        /* sender address captured by pad probe */
        char src_ip[64];
        int  src_port;
        pthread_mutex_lock(&g_addr_mtx);
        snprintf(src_ip, sizeof(src_ip), "%s", g_src_ip[0] ? g_src_ip : "none");
        src_port = g_src_port;
        pthread_mutex_unlock(&g_addr_mtx);

        unsigned wp   = atomic_load(&ring.wp);
        unsigned rp   = atomic_load(&ring.rp);
        int buf_ms    = (int)((float)(wp - rp) / (float)g_rate * 1000.0f);
        unsigned long pkts  = atomic_load(&g_packets);
        unsigned long drops = atomic_load(&g_drops);

        /* stats → stderr (Node.js가 stderr pipe로 읽음, 파일 I/O 없음) */
        fprintf(stderr,
            "stats codec=%s bufMs=%d packets=%lu drops=%lu srcIp=%s srcPort=%d bitrateKbps=%d\n",
            g_codec, buf_ms, pkts, drops, src_ip, src_port, bitrate_kbps);
        fflush(stderr);
    }
    return NULL;
}

/* ── signal handler ──────────────────────────────── */
static void on_signal(int sig)
{
    (void)sig;
    g_quit = 1;
}

/* ── main ─────────────────────────────────────────── */
/* ── AUTO 모드: 실시간 PT 변경 감지 프로브 ───────────
 * udpsrc src 패드에 상시 부착. PT가 바뀌면 stderr에
 * "auto-codec: ENCODING RATE" 를 출력하고 exit 2로 종료.
 * Node.js가 stderr을 파싱해 cfg 업데이트 후 즉시 재시작.
 */
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

    /* PT 변경 감지 → encoding 결정 */
    char new_enc[32] = "";
    int  new_rate    = g_sample_rate_in;

    switch (pt) {
        case 0:  snprintf(new_enc, sizeof(new_enc), "PCMU"); new_rate = 8000;  break;
        case 8:  snprintf(new_enc, sizeof(new_enc), "PCMA"); new_rate = 8000;  break;
        case 10: snprintf(new_enc, sizeof(new_enc), "L16");  new_rate = 44100; break; /* L16/44100/2 stereo */
        case 11: snprintf(new_enc, sizeof(new_enc), "L16");  new_rate = 44100; break; /* L16/44100/1 mono */
        case 14: snprintf(new_enc, sizeof(new_enc), "MPA");                    break;
        default: {
            /* Dynamic PT: 페이로드 스니핑 */
            int cc = map.data[0] & 0x0F;
            int has_ext = (map.data[0] >> 4) & 0x1;
            int hdr = 12 + cc * 4;
            if (has_ext && map.size >= (size_t)(hdr + 4)) {
                int ew = (map.data[hdr+2] << 8) | map.data[hdr+3];
                hdr += 4 + ew * 4;
            }
            if (hdr < (int)map.size) {
                const uint8_t *pl = map.data + hdr;
                int plen = (int)map.size - hdr;
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

    /* 현재 설정과 다르면 → stderr 출력 후 재시작 트리거 */
    if (strcmp(new_enc, g_encoding_name) != 0 || new_rate != g_sample_rate_in) {
        fprintf(stderr, "[rtp_recv] auto-codec: %s %d\n", new_enc, new_rate);
        fflush(stderr);
        g_exit_code = 2;
        g_quit      = 1;
    }
    return GST_PAD_PROBE_PASS;
}

/* ── RTP codec auto-detection ────────────────────────
 * 파이프라인 시작 전에 UDP 소켓으로 첫 패킷을 스니핑.
 * PT와 페이로드를 보고 g_encoding_name 을 결정한다.
 * 타임아웃(3초) 내에 패킷이 없으면 기존 g_encoding_name 유지.
 */
static void detect_rtp_codec(int port)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) return;

    int reuse = 1;
    setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));
    setsockopt(sock, SOL_SOCKET, SO_REUSEPORT, &reuse, sizeof(reuse));

    struct timeval tv = { .tv_sec = 0, .tv_usec = 100000 };  /* 100ms */
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

    struct sockaddr_in addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_port        = htons((uint16_t)port);
    addr.sin_addr.s_addr = INADDR_ANY;

    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(sock);
        return;
    }

    uint8_t buf[4096];
    ssize_t len = recv(sock, buf, sizeof(buf), 0);
    close(sock);

    if (len < 12) return; /* RTP 헤더 최소 12바이트 */

    uint8_t pt      = buf[1] & 0x7F;   /* payload type */
    int     cc      = buf[0] & 0x0F;   /* CSRC count   */
    int     has_ext = (buf[0] >> 4) & 0x1;

    fprintf(stderr, "[rtp_recv] auto-detect: PT=%d\n", pt);

    /* Static PT → 바로 결정 */
    switch (pt) {
        case 0:  snprintf(g_encoding_name, sizeof(g_encoding_name), "PCMU"); return;
        case 8:  snprintf(g_encoding_name, sizeof(g_encoding_name), "PCMA"); return;
        case 10: snprintf(g_encoding_name, sizeof(g_encoding_name), "L16");  /* L16/44100/2 stereo */
                 g_sample_rate_in = 44100; return;
        case 11: snprintf(g_encoding_name, sizeof(g_encoding_name), "L16");  /* L16/44100/1 mono */
                 g_sample_rate_in = 44100; return;
        case 14: snprintf(g_encoding_name, sizeof(g_encoding_name), "MPA");  return;
        default: break;
    }

    /* Dynamic PT (96+) → 페이로드 내용으로 판별 */
    int hdr = 12 + cc * 4;
    if (has_ext && len > hdr + 4) {
        int ext_words = (buf[hdr + 2] << 8) | buf[hdr + 3];
        hdr += 4 + ext_words * 4;
    }
    if (hdr >= (int)len) return;

    const uint8_t *payload     = buf + hdr;
    int            payload_len = (int)len - hdr;

    /* MPA(MP3): RFC 2250 — 4바이트 헤더 후 MPEG sync (0xFF 0xEx) */
    if (payload_len >= 6) {
        const uint8_t *mp = payload + 4;
        if (mp[0] == 0xFF && (mp[1] & 0xE0) == 0xE0) {
            snprintf(g_encoding_name, sizeof(g_encoding_name), "MPA");
            fprintf(stderr, "[rtp_recv] auto-detect: MPA(MP3) from payload\n");
            return;
        }
    }

    /* L24 vs L16: 페이로드 크기로 추정
     * L16 stereo 44100Hz 20ms = 44100*0.02*2*2 = 3528 bytes
     * L24 stereo 48000Hz 20ms = 48000*0.02*2*3 = 5760 bytes
     * 4000 bytes 기준으로 구분 */
    if (payload_len > 4000) {
        snprintf(g_encoding_name, sizeof(g_encoding_name), "L24");
        g_sample_rate_in = 48000;
        fprintf(stderr, "[rtp_recv] auto-detect: L24 (payload=%d)\n", payload_len);
    } else {
        snprintf(g_encoding_name, sizeof(g_encoding_name), "L16");
        fprintf(stderr, "[rtp_recv] auto-detect: L16 (payload=%d)\n", payload_len);
    }
}

int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    int port = argc > 1 ? atoi(argv[1]) : 5004;
    g_ch     = argc > 2 ? atoi(argv[2]) : 2;

    /* argv[3]: protocol — raw (default) | pcm | rtp */
    const char *proto_arg = argc > 3 ? argv[3] : "raw";
    if      (strcmp(proto_arg, "pcm") == 0) g_proto = PROTO_PCM;
    else if (strcmp(proto_arg, "rtp") == 0) g_proto = PROTO_RTP;
    else                                    g_proto = PROTO_RAW;

    g_sample_rate_in = argc > 6 ? atoi(argv[6]) : 48000;
    if (g_sample_rate_in <= 0) g_sample_rate_in = 48000;
    /* argv[7]: RTP encoding name (AUTO, L16, L24, MPA, OPUS, PCMA, PCMU) */
    if (g_proto == PROTO_RTP && argc > 7)
        snprintf(g_encoding_name, sizeof(g_encoding_name), "%s", argv[7]);

    /* RTP 모드: 항상 실시간 PT 변경 감지 활성화 (MPA→PCM 등 전환 감지) */
    if (g_proto == PROTO_RTP) {
        g_auto_detect = 1;
        if (strcmp(g_encoding_name, "AUTO") == 0) {
            snprintf(g_encoding_name, sizeof(g_encoding_name), "L16"); /* fallback */
            detect_rtp_codec(port);  /* 데이터 없으면 즉시 반환 (timeout=100ms) */
            fprintf(stderr, "[rtp_recv] AUTO mode: using encoding=%s rate=%d\n",
                    g_encoding_name, g_sample_rate_in);
        }
    }

    char client_name[64];
    snprintf(client_name, sizeof(client_name), "%s", argc > 4 ? argv[4] : "rtp_in");
    g_buf_ms = argc > 5 ? atoi(argv[5]) : 100;
    if (g_buf_ms < 10)  g_buf_ms = 10;
    if (g_buf_ms > 500) g_buf_ms = 500;

    /* argv[8]: bind/multicast address (default 0.0.0.0) */
    if (argc > 8 && argv[8][0] != '\0')
        snprintf(g_bind_address, sizeof(g_bind_address), "%s", argv[8]);

    /* ring buffer init */
    memset(&ring, 0, sizeof(ring));
    ring.ch = g_ch;

    /* ── JACK setup ── */
    g_jclient = jack_client_open(client_name, JackNoStartServer | JackUseExactName, NULL);
    if (!g_jclient) {
        fprintf(stderr, "[rtp_recv] jack_client_open failed\n");
        return 1;
    }
    g_rate = (int)jack_get_sample_rate(g_jclient);

    for (int c = 0; c < g_ch; c++) {
        char name[32];
        snprintf(name, sizeof(name), "out_%d", c + 1);
        jports[c] = jack_port_register(g_jclient, name,
                                       JACK_DEFAULT_AUDIO_TYPE,
                                       JackPortIsOutput, 0);
        if (!jports[c]) {
            fprintf(stderr, "[rtp_recv] jack_port_register %s failed\n", name);
            jack_client_close(g_jclient);
            return 1;
        }
    }

    jack_set_process_callback(g_jclient, jack_process, NULL);
    if (jack_activate(g_jclient)) {
        fprintf(stderr, "[rtp_recv] jack_activate failed\n");
        jack_client_close(g_jclient);
        return 1;
    }

    /* ── GStreamer pipeline ── */
    const char *mode_str = g_proto == PROTO_PCM ? "pcm" :
                           g_proto == PROTO_RTP  ? "rtp" : "raw";
    fprintf(stderr, "[rtp_recv] mode=%s\n", mode_str);
    if (g_proto == PROTO_PCM)
        fprintf(stderr, "[rtp_recv] input rate=%d\n", g_sample_rate_in);
    if (g_proto == PROTO_RTP)
        fprintf(stderr, "[rtp_recv] rtp encoding=%s clock-rate=%d\n",
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
    if (!ok) {
        fprintf(stderr, "[rtp_recv] failed to create GStreamer elements\n");
        jack_client_close(g_jclient);
        return 1;
    }

    g_pipeline = gst_pipeline_new("rtp_recv");

    /* multicast: address="239.x.x.x" joins that group; unicast: "0.0.0.0" binds all ifaces */
    g_object_set(src, "port", port, "address", g_bind_address,
                 "retrieve-sender-address", TRUE, NULL);
    fprintf(stderr, "[rtp_recv] udpsrc port=%d address=%s\n", port, g_bind_address);

    /* capsfilter: JACK용 F32LE interleaved */
    GstCaps *caps = gst_caps_new_simple("audio/x-raw",
        "format",   G_TYPE_STRING, "F32LE",
        "rate",     G_TYPE_INT,    g_rate,
        "channels", G_TYPE_INT,    g_ch,
        "layout",   G_TYPE_STRING, "interleaved",
        NULL);
    g_object_set(caps_flt, "caps", caps, NULL);
    gst_caps_unref(caps);

    /* appsink */
    g_object_set(appsink,
        "emit-signals", TRUE,
        "sync",         FALSE,
        "max-buffers",  8,
        "drop",         TRUE,
        NULL);
    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);

    /* pipeline assembly */
    int link_ok = 1;
    if (g_proto == PROTO_RTP) {
        /*
         * PROTO_RTP: standard RTP (QSys, etc.)
         *
         * AUTO 모드: rtpjitterbuffer → decodebin  (static PT 자동 감지)
         *   PT=14 MPA(MP3), PT=11 L16, PT=0 PCMU, PT=8 PCMA 등
         *
         * 명시 모드: rtpjitterbuffer → rtpXXXdepay [→ decodebin]
         *   dynamic PT(96+) L24/L16/OPUS 등 직접 지정
         */
        GstElement *jitter = gst_element_factory_make("rtpjitterbuffer", "jitter");
        if (!jitter) {
            fprintf(stderr, "[rtp_recv] failed to create rtpjitterbuffer\n");
            gst_object_unref(g_pipeline);
            jack_client_close(g_jclient);
            return 1;
        }
        g_object_set(jitter, "latency", (guint)g_buf_ms, NULL);

        /* AUTO는 main()에서 이미 detect_rtp_codec()으로 g_encoding_name 결정됨 */
        int rtp_needs_decode = (strcmp(g_encoding_name, "MPA")  == 0 ||
                                strcmp(g_encoding_name, "OPUS") == 0);

        const char *depay_name =
            (strcmp(g_encoding_name, "L16")  == 0) ? "rtpL16depay"  :
            (strcmp(g_encoding_name, "MPA")  == 0) ? "rtpmpadepay"  :
            (strcmp(g_encoding_name, "OPUS") == 0) ? "rtpopusdepay" :
            (strcmp(g_encoding_name, "PCMA") == 0) ? "rtppcmadepay" :
            (strcmp(g_encoding_name, "PCMU") == 0) ? "rtppcmudepay" :
                                                      "rtpL24depay"; /* default L24 */

        GstElement *depay      = gst_element_factory_make(depay_name, "depay");
        GstElement *rtp_decode = rtp_needs_decode ?
                                 gst_element_factory_make("decodebin", "rtp_decode") : NULL;

        if (!depay || (rtp_needs_decode && !rtp_decode)) {
            fprintf(stderr, "[rtp_recv] failed to create depay element (%s)\n", depay_name);
            gst_object_unref(g_pipeline);
            jack_client_close(g_jclient);
            return 1;
        }

        char ch_str[8];
        snprintf(ch_str, sizeof(ch_str), "%d", g_ch);
        GstCaps *rtp_caps = gst_caps_new_simple("application/x-rtp",
            "media",           G_TYPE_STRING, "audio",
            "clock-rate",      G_TYPE_INT,    g_sample_rate_in,
            "encoding-name",   G_TYPE_STRING, g_encoding_name,
            "encoding-params", G_TYPE_STRING, ch_str,
            "channels",        G_TYPE_INT,    g_ch,
            NULL);
        g_object_set(src, "caps", rtp_caps, NULL);
        gst_caps_unref(rtp_caps);

        if (rtp_needs_decode) {
            g_signal_connect(rtp_decode, "pad-added", G_CALLBACK(on_pad_added), convert);
            gst_bin_add_many(GST_BIN(g_pipeline),
                src, jitter, depay, rtp_decode, convert, resample, caps_flt, appsink, NULL);
            link_ok = gst_element_link_many(src, jitter, depay, rtp_decode, NULL) &&
                      gst_element_link_many(convert, resample, caps_flt, appsink, NULL);
        } else {
            gst_bin_add_many(GST_BIN(g_pipeline),
                src, jitter, depay, convert, resample, caps_flt, appsink, NULL);
            link_ok = gst_element_link_many(
                src, jitter, depay, convert, resample, caps_flt, appsink, NULL);
        }

    } else if (g_proto == PROTO_PCM) {
        /*
         * PROTO_PCM (raw S16LE over UDP):
         * udpsrc caps(S16LE) → audioconvert → audioresample → capsfilter → appsink
         */
        GstCaps *c = gst_caps_new_simple("audio/x-raw",
            "format",   G_TYPE_STRING, "S16LE",
            "rate",     G_TYPE_INT,    g_sample_rate_in,
            "channels", G_TYPE_INT,    g_ch,
            "layout",   G_TYPE_STRING, "interleaved",
            NULL);
        g_object_set(src, "caps", c, NULL);
        gst_caps_unref(c);

        gst_bin_add_many(GST_BIN(g_pipeline),
            src, convert, resample, caps_flt, appsink, NULL);
        link_ok = gst_element_link_many(
            src, convert, resample, caps_flt, appsink, NULL);
    } else {
        /*
         * PROTO_RAW: udpsrc → decodebin (MP3/Opus/WAV 자동 감지)
         *   → audioconvert → audioresample → capsfilter → appsink
         */
        g_signal_connect(decode, "pad-added", G_CALLBACK(on_pad_added), convert);
        gst_bin_add_many(GST_BIN(g_pipeline),
            src, decode, convert, resample, caps_flt, appsink, NULL);
        link_ok = gst_element_link(src, decode) &&
                  gst_element_link_many(convert, resample, caps_flt, appsink, NULL);
    }
    if (!link_ok) {
        fprintf(stderr, "[rtp_recv] pipeline link failed (mode=%s)\n", mode_str);
        gst_object_unref(g_pipeline);
        jack_client_close(g_jclient);
        return 1;
    }

    GstBus *bus = gst_element_get_bus(g_pipeline);
    gst_bus_add_watch(bus, bus_msg, NULL);
    gst_object_unref(bus);

    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);

    /* pad probes on udpsrc */
    {
        GstPad *src_pad = gst_element_get_static_pad(src, "src");
        if (src_pad) {
            gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, udp_probe, NULL, NULL);
            /* AUTO 모드: PT 변경 실시간 감지 프로브 */
            if (g_auto_detect)
                gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, auto_pt_probe, NULL, NULL);
            gst_object_unref(src_pad);
        }
    }

    /* report ports first, then ready — Node.js uses ready as the trigger */
    /* ports/ready → stderr (JACK이 fd1을 오염시켜도 fd2는 안전) */
    fprintf(stderr, "ports:");
    for (int c = 0; c < g_ch; c++) {
        if (c) fprintf(stderr, ",");
        fprintf(stderr, " %s:out_%d", client_name, c + 1);
    }
    fprintf(stderr, "\n");
    fprintf(stderr, "[rtp_recv] ready\n");
    fflush(stderr);

    /* stats thread */
    pthread_t stats_tid;
    pthread_create(&stats_tid, NULL, stats_thread, NULL);

    /* run GLib main loop */
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
    jack_client_close(g_jclient);

    fprintf(stderr, "[rtp_recv] exiting\n");
    return g_exit_code;
}
