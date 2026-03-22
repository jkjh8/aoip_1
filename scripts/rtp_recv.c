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

#define MAX_CH       8
#define RING_FRAMES  16384   /* must be power of 2 */

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
    if ((int)(RING_FRAMES - (wp - rp)) < nframes) {
        atomic_fetch_add_explicit(&ring.rp,
            (unsigned)nframes, memory_order_release); /* discard oldest */
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
static int             g_ch   = 2;
static int             g_rate = 48000;

static volatile int    g_quit = 0;
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
    float tmp[4096 * MAX_CH];
    if (ring_read(tmp, (int)nframes)) {
        for (int c = 0; c < g_ch; c++) {
            float *out = jack_port_get_buffer(jports[c], nframes);
            for (jack_nframes_t f = 0; f < nframes; f++)
                out[f] = tmp[f * g_ch + c];
        }
    } else {
        /* underrun → silence */
        for (int c = 0; c < g_ch; c++) {
            float *out = jack_port_get_buffer(jports[c], nframes);
            memset(out, 0, nframes * sizeof(float));
        }
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
    unsigned long prev_udp  = 0;
    unsigned long prev_pkts = 0;
    int           stall_cnt = 0;

    while (!g_quit) {
        sleep(2);

        /* bitrate from UDP-level bytes (encoded stream) */
        unsigned long cur_udp = atomic_load(&g_udp_bytes);
        int bitrate_kbps = (int)((cur_udp - prev_udp) * 8 / 2 / 1000);
        prev_udp = cur_udp;

        /* stall detection: if packets frozen after stream was active, exit for restart */
        unsigned long cur_pkts_snap = atomic_load(&g_packets);
        if (prev_pkts > 0 && cur_pkts_snap == prev_pkts) {
            if (++stall_cnt >= 5) {   /* 5 × 2s = 10s */
                fprintf(stderr, "[rtp_recv] stream stall detected — exiting for restart\n");
                fflush(stderr);
                g_quit = 1;
                break;
            }
        } else {
            stall_cnt = 0;
        }
        prev_pkts = cur_pkts_snap;

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

        fprintf(stdout,
            "stats codec=%s bufMs=%d packets=%lu drops=%lu"
            " srcIp=%s srcPort=%d bitrateKbps=%d\n",
            g_codec, buf_ms, pkts, drops,
            src_ip[0] ? src_ip : "none", src_port, bitrate_kbps);
        fflush(stdout);
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
int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    int port    = argc > 1 ? atoi(argv[1]) : 10001;
    g_ch        = argc > 2 ? atoi(argv[2]) : 2;
    int rtp_mode = (argc > 3 && strcmp(argv[3], "rtp") == 0) ? 1 : 0;

    /* ring buffer init */
    memset(&ring, 0, sizeof(ring));
    ring.ch = g_ch;

    /* ── JACK setup ── */
    g_jclient = jack_client_open("rtp_in", JackNoStartServer, NULL);
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
    GstElement *src      = gst_element_factory_make("udpsrc",        "src");
    GstElement *depay    = rtp_mode ? gst_element_factory_make("rtpmpadepay", "depay") : NULL;
    GstElement *decode   = gst_element_factory_make("decodebin",     "decode");
    GstElement *convert  = gst_element_factory_make("audioconvert",  "convert");
    GstElement *resample = gst_element_factory_make("audioresample", "resample");
    GstElement *caps_flt = gst_element_factory_make("capsfilter",    "caps");
    GstElement *appsink  = gst_element_factory_make("appsink",       "sink");

    if (!src || (rtp_mode && !depay) || !decode || !convert || !resample || !caps_flt || !appsink) {
        fprintf(stderr, "[rtp_recv] failed to create GStreamer elements\n");
        jack_client_close(g_jclient);
        return 1;
    }

    g_pipeline = gst_pipeline_new("rtp_recv");
    fprintf(stderr, "[rtp_recv] mode=%s\n", rtp_mode ? "rtp" : "raw");

    /* udpsrc */
    g_object_set(src, "port", port, "retrieve-sender-address", TRUE, NULL);
    if (rtp_mode) {
        GstCaps *rtp_caps = gst_caps_new_simple("application/x-rtp",
            "media",         G_TYPE_STRING, "audio",
            "clock-rate",    G_TYPE_INT,    90000,
            "encoding-name", G_TYPE_STRING, "MPA",
            NULL);
        g_object_set(src, "caps", rtp_caps, NULL);
        gst_caps_unref(rtp_caps);
    }

    /* capsfilter: F32LE interleaved to match JACK */
    GstCaps *caps = gst_caps_new_simple("audio/x-raw",
        "format",   G_TYPE_STRING, "F32LE",
        "rate",     G_TYPE_INT,    g_rate,
        "channels", G_TYPE_INT,    g_ch,
        "layout",   G_TYPE_STRING, "interleaved",
        NULL);
    g_object_set(caps_flt, "caps", caps, NULL);
    gst_caps_unref(caps);

    /* appsink: pull mode, no sync */
    g_object_set(appsink,
        "emit-signals", TRUE,
        "sync",         FALSE,
        "max-buffers",  8,
        "drop",         TRUE,
        NULL);
    g_signal_connect(appsink, "new-sample", G_CALLBACK(on_new_sample), NULL);
    g_signal_connect(decode,  "pad-added",  G_CALLBACK(on_pad_added),  convert);

    if (rtp_mode) {
        gst_bin_add_many(GST_BIN(g_pipeline),
            src, depay, decode, convert, resample, caps_flt, appsink, NULL);
        if (!gst_element_link(src, depay) ||
            !gst_element_link(depay, decode) ||
            !gst_element_link_many(convert, resample, caps_flt, appsink, NULL)) {
            fprintf(stderr, "[rtp_recv] pipeline link failed (rtp mode)\n");
            gst_object_unref(g_pipeline);
            jack_client_close(g_jclient);
            return 1;
        }
    } else {
        gst_bin_add_many(GST_BIN(g_pipeline),
            src, decode, convert, resample, caps_flt, appsink, NULL);
        if (!gst_element_link(src, decode) ||
            !gst_element_link_many(convert, resample, caps_flt, appsink, NULL)) {
            fprintf(stderr, "[rtp_recv] pipeline link failed (raw mode)\n");
            gst_object_unref(g_pipeline);
            jack_client_close(g_jclient);
            return 1;
        }
    }

    GstBus *bus = gst_element_get_bus(g_pipeline);
    gst_bus_add_watch(bus, bus_msg, NULL);
    gst_object_unref(bus);

    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);

    /* pad probe on udpsrc for sender address + UDP byte counting */
    {
        GstPad *src_pad = gst_element_get_static_pad(src, "src");
        if (src_pad) {
            gst_pad_add_probe(src_pad, GST_PAD_PROBE_TYPE_BUFFER, udp_probe, NULL, NULL);
            gst_object_unref(src_pad);
        }
    }

    /* report ready */
    fprintf(stdout, "[rtp_recv] ready\n");
    fprintf(stdout, "ports:");
    for (int c = 0; c < g_ch; c++) {
        if (c) fprintf(stdout, ",");
        fprintf(stdout, " rtp_in:out_%d", c + 1);
    }
    fprintf(stdout, "\n");
    fflush(stdout);

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
    return 0;
}
