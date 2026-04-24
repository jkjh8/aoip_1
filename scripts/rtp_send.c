/*
 * rtp_send.c — GStreamer RTP/UDP sender (pipe mode, FIFO input)
 *
 * GStreamer pipeline:
 *   fdsrc(F32LE) → audioconvert → [audioresample] → capsfilter
 *     → [rtpL16pay|rtpmpapay|rtpopuspay] → tee → udpsink × N
 *
 * Usage:   rtp_send <channels> <client> <proto> <outRate> pipe <fifo_path>
 *
 * Stdin commands:
 *   add <host> <port>           — add TX target (rebuilds pipeline)
 *   remove <host> <port>        — remove TX target
 *   codec <mp3|opus|raw> [br]   — set codec / bitrate
 *   quit                        — clean shutdown
 *
 * Stderr:
 *   [rtp_send] ready
 *   stats targets=N codec=X bitrateKbps=N bytesSent=N
 */

#include <gst/gst.h>
#include <gst/base/gstbasesrc.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

#define MAX_TARGETS 16

/* ── target list ─────────────────────────────────── */
typedef struct { char host[128]; int port; } Target;
static Target          targets[MAX_TARGETS];
static int             n_targets = 0;
static pthread_mutex_t target_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── codec config ────────────────────────────────── */
typedef enum { CODEC_MP3, CODEC_OPUS, CODEC_RAW } Codec;
static Codec  g_codec   = CODEC_RAW;
static int    g_bitrate = 320;

/* ── globals ─────────────────────────────────────── */
static int           g_ch       = 2;
static int           g_use_rtp  = 0;
static int           g_out_rate = 0;   /* 0 = keep JACK rate */
static volatile int  g_quit     = 0;
static char          g_client_name[64] = "rtp_send";

/* pipe 입력 모드: jackaudiosrc 대신 named FIFO에서 F32LE 읽기 */
static char g_input_fifo[256] = "";
static int  g_input_fd        = -1;

static GstElement     *g_pipeline = NULL;
static pthread_mutex_t pipe_mutex  = PTHREAD_MUTEX_INITIALIZER;

/* stats: bytes passing through udpsink sink pad */
static atomic_ulong    g_bytes_sent = 0;

/* ── GStreamer bus handler ────────────────────────── */
static gboolean bus_cb(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus; (void)data;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *err = NULL; gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        fprintf(stderr, "[rtp_send] gst error: %s (%s)\n",
                err->message, dbg ? dbg : "");
        g_error_free(err); g_free(dbg);
    }
    return TRUE;
}

/* udpsink sink pad probe: count actual output bytes */
static GstPadProbeReturn udp_out_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    (void)pad; (void)data;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buf)
        atomic_fetch_add_explicit(&g_bytes_sent, gst_buffer_get_size(buf),
                                  memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

/* ── pipeline builder ────────────────────────────── */
static void pipeline_stop(void)
{
    if (g_pipeline) {
        gst_element_set_state(g_pipeline, GST_STATE_NULL);
        gst_object_unref(g_pipeline);
        g_pipeline = NULL;
    }
    /* fdsrc may close the fd when the pipeline is unreffed (versions without
     * close-fd property).  Reset so pipeline_build() always reopens it. */
    if (g_input_fifo[0] && g_input_fd >= 0) {
        close(g_input_fd);
        g_input_fd = -1;
    }
}

static int pipeline_build(void)
{
    pipeline_stop();

    int in_rate  = 48000;   /* aoip_engine → FIFO → F32LE @ 48kHz */
    int out_rate = (g_out_rate > 0) ? g_out_rate : in_rate;

    GstElement *pipe   = gst_pipeline_new("rtp_send");
    GstElement *cvt    = gst_element_factory_make("audioconvert",  "cvt");
    GstElement *resamp = (out_rate != in_rate) ?
                         gst_element_factory_make("audioresample", "resamp") : NULL;

    if (!cvt) goto fail;
    if (out_rate != in_rate && !resamp) goto fail;

    /* fdsrc → capsfilter(F32LE) → cvt [→ resamp] */
    if (g_input_fd < 0) {
        g_input_fd = open(g_input_fifo, O_RDONLY);
        if (g_input_fd < 0) { perror("[rtp_send] open input fifo"); goto fail; }
    }
    GstElement *src = gst_element_factory_make("fdsrc",     "src");
    GstElement *cf  = gst_element_factory_make("capsfilter", "cf_in");
    if (!src || !cf) goto fail;

    GstCaps *in_caps = gst_caps_new_simple("audio/x-raw",
        "format",   G_TYPE_STRING, "F32LE",
        "rate",     G_TYPE_INT,    in_rate,
        "channels", G_TYPE_INT,    g_ch,
        "layout",   G_TYPE_STRING, "interleaved", NULL);
    g_object_set(src, "fd", g_input_fd, NULL);
    gst_base_src_set_do_timestamp(GST_BASE_SRC(src), TRUE);
    gst_base_src_set_live(GST_BASE_SRC(src), TRUE);
    g_object_set(cf, "caps", in_caps, NULL);
    gst_caps_unref(in_caps);

    GstElement *last_cvt;
    if (resamp) {
        gst_bin_add_many(GST_BIN(pipe), src, cf, cvt, resamp, NULL);
        if (!gst_element_link(src, cf))     goto fail;
        if (!gst_element_link(cf,  cvt))    goto fail;
        if (!gst_element_link(cvt, resamp)) goto fail;
    } else {
        gst_bin_add_many(GST_BIN(pipe), src, cf, cvt, NULL);
        if (!gst_element_link(src, cf))  goto fail;
        if (!gst_element_link(cf,  cvt)) goto fail;
    }
    last_cvt = resamp ? resamp : cvt;

    GstElement *enc      = NULL;
    GstElement *caps_flt = NULL;

    if (g_codec == CODEC_MP3) {
        enc = gst_element_factory_make("lamemp3enc", "enc");
        if (!enc) goto fail;
        g_object_set(enc, "bitrate", g_bitrate, "cbr", TRUE, NULL);
        gst_bin_add(GST_BIN(pipe), enc);
        if (!gst_element_link(last_cvt, enc)) goto fail;
    } else if (g_codec == CODEC_OPUS) {
        enc = gst_element_factory_make("opusenc", "enc");
        if (!enc) goto fail;
        g_object_set(enc, "bitrate", g_bitrate * 1000, NULL);
        gst_bin_add(GST_BIN(pipe), enc);
        if (!gst_element_link(last_cvt, enc)) goto fail;
    } else {
        /* raw PCM: S16BE for RTP (RFC 3551), S16LE for plain UDP */
        caps_flt = gst_element_factory_make("capsfilter", "cf");
        if (!caps_flt) goto fail;
        GstCaps *raw = gst_caps_new_simple("audio/x-raw",
            "format",   G_TYPE_STRING, g_use_rtp ? "S16BE" : "S16LE",
            "rate",     G_TYPE_INT,    out_rate,
            "channels", G_TYPE_INT,    g_ch,
            "layout",   G_TYPE_STRING, "interleaved",
            NULL);
        g_object_set(caps_flt, "caps", raw, NULL);
        gst_caps_unref(raw);
        gst_bin_add(GST_BIN(pipe), caps_flt);
        if (!gst_element_link(last_cvt, caps_flt)) goto fail;
    }
    GstElement *last = enc ? enc : caps_flt;

    /* RTP payloader */
    if (g_use_rtp) {
        GstElement *pay = NULL;
        if      (g_codec == CODEC_MP3)  pay = gst_element_factory_make("rtpmpapay",  "pay");
        else if (g_codec == CODEC_OPUS) pay = gst_element_factory_make("rtpopuspay", "pay");
        else                            pay = gst_element_factory_make("rtpL16pay",  "pay");
        if (!pay) goto fail;
        /* RFC 3551 Table 4 static payload types for L16:
         *   PT=10: L16/44100/2  (stereo)
         *   PT=11: L16/44100/1  (mono)
         * Choose based on channel count and rate; fall back to dynamic (96) otherwise. */
        if (g_codec == CODEC_RAW) {
            int pt = -1;
            if (out_rate == 44100) {
                if      (g_ch == 2) pt = 10;
                else if (g_ch == 1) pt = 11;
            }
            if (pt >= 0) g_object_set(pay, "pt", pt, NULL);
        }
        gst_bin_add(GST_BIN(pipe), pay);
        if (!gst_element_link(last, pay)) goto fail;
        last = pay;
    }

    /* sink(s) */
    if (n_targets == 0) {
        GstElement *fake = gst_element_factory_make("fakesink", "fake");
        if (!fake) goto fail;
        g_object_set(fake, "sync", FALSE, NULL);
        gst_bin_add(GST_BIN(pipe), fake);
        if (!gst_element_link(last, fake)) goto fail;
    } else if (n_targets == 1) {
        GstElement *sink = gst_element_factory_make("udpsink", "sink0");
        if (!sink) goto fail;
        g_object_set(sink, "host", targets[0].host, "port", targets[0].port,
                     "sync", FALSE, NULL);
        gst_bin_add(GST_BIN(pipe), sink);
        if (!gst_element_link(last, sink)) goto fail;
        GstPad *sp = gst_element_get_static_pad(sink, "sink");
        if (sp) { gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER, udp_out_probe, NULL, NULL); gst_object_unref(sp); }
    } else {
        GstElement *tee = gst_element_factory_make("tee", "tee");
        if (!tee) goto fail;
        gst_bin_add(GST_BIN(pipe), tee);
        if (!gst_element_link(last, tee)) goto fail;
        for (int i = 0; i < n_targets; i++) {
            char qname[32], sname[32];
            snprintf(qname, sizeof(qname), "q%d", i);
            snprintf(sname, sizeof(sname), "sink%d", i);
            GstElement *q    = gst_element_factory_make("queue",   qname);
            GstElement *sink = gst_element_factory_make("udpsink", sname);
            if (!q || !sink) goto fail;
            g_object_set(sink, "host", targets[i].host, "port", targets[i].port,
                         "sync", FALSE, NULL);
            gst_bin_add_many(GST_BIN(pipe), q, sink, NULL);
            if (!gst_element_link_many(tee, q, sink, NULL)) goto fail;
            GstPad *sp = gst_element_get_static_pad(sink, "sink");
            if (sp) { gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER, udp_out_probe, NULL, NULL); gst_object_unref(sp); }
        }
    }

    GstBus *bus = gst_element_get_bus(pipe);
    if (bus) { gst_bus_add_watch(bus, bus_cb, NULL); gst_object_unref(bus); }

    g_pipeline = pipe;
    /* g_jaksrc는 각 모드 블록 안에서 이미 설정됨 */

    GstStateChangeReturn ret = gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "[rtp_send] pipeline failed to start PLAYING\n");
        gst_object_unref(g_pipeline);
        g_pipeline = NULL;
        return 0;
    }
    fprintf(stderr, "[rtp_send] pipeline built: %s%s rate=%d→%d targets=%d\n",
            g_codec == CODEC_MP3 ? "mp3" : g_codec == CODEC_OPUS ? "opus" : "raw",
            g_use_rtp ? "+rtp" : "",
            in_rate, out_rate, n_targets);
    return 1;

fail:
    fprintf(stderr, "[rtp_send] pipeline build failed\n");
    gst_object_unref(pipe);
    return 0;
}

/* ── stats thread (~2 Hz) ────────────────────────── */
static void *stats_thread(void *arg)
{
    (void)arg;
    unsigned long prev_bytes = 0;
    while (!g_quit) {
        sleep(2);
        unsigned long cur     = atomic_load(&g_bytes_sent);
        int           kbps    = (int)((cur - prev_bytes) * 8 / 2 / 1000);
        prev_bytes            = cur;
        const char *codec_str = g_codec == CODEC_OPUS ? "opus" :
                                g_codec == CODEC_RAW  ? "raw"  : "mp3";
        pthread_mutex_lock(&target_mutex);
        int nt = n_targets;
        pthread_mutex_unlock(&target_mutex);
        fprintf(stdout, "stats targets=%d codec=%s bitrateKbps=%d bytesSent=%lu\n",
                nt, codec_str, kbps, cur);
        fflush(stdout);
    }
    return NULL;
}

/* ── stdin command thread ────────────────────────── */
static void *stdin_thread(void *arg)
{
    (void)arg;
    char line[256];
    while (!g_quit && fgets(line, sizeof(line), stdin)) {
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        char cmd[64], h[128];
        int  p = 0;
        if (sscanf(line, "%63s", cmd) < 1) continue;

        if (strcmp(cmd, "quit") == 0) { g_quit = 1; break; }

        if (strcmp(cmd, "add") == 0 && sscanf(line, "%*s %127s %d", h, &p) == 2) {
            pthread_mutex_lock(&target_mutex);
            int found = 0;
            for (int i = 0; i < n_targets; i++)
                if (strcmp(targets[i].host, h) == 0 && targets[i].port == p)
                    { found = 1; break; }
            if (!found && n_targets < MAX_TARGETS) {
                snprintf(targets[n_targets].host, sizeof(targets[0].host), "%s", h);
                targets[n_targets].port = p;
                n_targets++;
            }
            pthread_mutex_unlock(&target_mutex);
            pthread_mutex_lock(&pipe_mutex);
            pipeline_build();
            pthread_mutex_unlock(&pipe_mutex);
            fprintf(stderr, "[rtp_send] add target %s:%d (total=%d)\n", h, p, n_targets);
            continue;
        }

        if (strcmp(cmd, "remove") == 0 && sscanf(line, "%*s %127s %d", h, &p) == 2) {
            pthread_mutex_lock(&target_mutex);
            for (int i = 0; i < n_targets; i++) {
                if (strcmp(targets[i].host, h) == 0 && targets[i].port == p) {
                    targets[i] = targets[--n_targets];
                    break;
                }
            }
            pthread_mutex_unlock(&target_mutex);
            pthread_mutex_lock(&pipe_mutex);
            pipeline_build();
            pthread_mutex_unlock(&pipe_mutex);
            fprintf(stderr, "[rtp_send] remove target %s:%d (total=%d)\n", h, p, n_targets);
            continue;
        }

        if (strcmp(cmd, "codec") == 0) {
            char codec_str[32] = "raw";
            int  br = g_bitrate;
            sscanf(line, "%*s %31s %d", codec_str, &br);
            Codec new_codec;
            if      (strcmp(codec_str, "mp3")  == 0) new_codec = CODEC_MP3;
            else if (strcmp(codec_str, "opus") == 0) new_codec = CODEC_OPUS;
            else                                      new_codec = CODEC_RAW;
            /* skip rebuild if nothing changed */
            if (new_codec == g_codec && br == g_bitrate) {
                fprintf(stderr, "[rtp_send] codec unchanged (%s %d)\n", codec_str, br);
                continue;
            }
            g_codec   = new_codec;
            g_bitrate = br;
            pthread_mutex_lock(&pipe_mutex);
            pipeline_build();
            pthread_mutex_unlock(&pipe_mutex);
            fprintf(stderr, "[rtp_send] codec=%s bitrate=%d\n", codec_str, br);
            continue;
        }

        fprintf(stderr, "[rtp_send] unknown command: %s\n", cmd);
    }
    return NULL;
}

/* ── signal ──────────────────────────────────────── */
static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ── main ─────────────────────────────────────────── */
int main(int argc, char *argv[])
{
    gst_init(&argc, &argv);
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    g_ch       = argc > 1 ? atoi(argv[1]) : 2;
    if (argc > 2) snprintf(g_client_name, sizeof(g_client_name), "%s", argv[2]);
    g_use_rtp  = (argc > 3 && strcmp(argv[3], "rtp") == 0) ? 1 : 0;
    g_out_rate = (argc > 4) ? atoi(argv[4]) : 0;
    if (g_use_rtp) g_codec = CODEC_RAW;
    /* pipe 모드: argv[5]="pipe"  argv[6]=fifo_path */
    if (argc > 5 && strcmp(argv[5], "pipe") == 0 && argc > 6)
        snprintf(g_input_fifo, sizeof(g_input_fifo), "%s", argv[6]);

    if (!g_input_fifo[0]) {
        fprintf(stderr, "[rtp_send] fifo path required (argv[5]=pipe argv[6]=<path>)\n");
        return 1;
    }

    pthread_mutex_lock(&pipe_mutex);
    pipeline_build();
    pthread_mutex_unlock(&pipe_mutex);

    fprintf(stderr, "[rtp_send] ready\n");

    pthread_t stdin_tid, stats_tid;
    pthread_create(&stdin_tid, NULL, stdin_thread, NULL);
    pthread_create(&stats_tid, NULL, stats_thread, NULL);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    while (!g_quit) {
        g_main_context_iteration(NULL, FALSE);
        usleep(5000);
    }
    g_main_loop_unref(loop);

    pthread_join(stdin_tid, NULL);
    pthread_join(stats_tid, NULL);

    pthread_mutex_lock(&pipe_mutex);
    pipeline_stop();
    pthread_mutex_unlock(&pipe_mutex);

    fprintf(stderr, "[rtp_send] exiting\n");
    return 0;
}
