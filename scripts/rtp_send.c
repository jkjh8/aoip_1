/*
 * rtp_send.c — JACK → GStreamer RTP/UDP sender
 *
 * GStreamer pipeline:
 *   jackaudiosrc → audioconvert → audioresample → capsfilter
 *     → [rtpL16pay|rtpmpapay|rtpopuspay] → tee → udpsink × N
 *
 * JACK ports: <client>:in_1 … <client>:in_N  (GStreamer registers them)
 *
 * Usage:   rtp_send [channels=2] [client=rtp_send] [protocol=raw|rtp] [outRate=0]
 *
 * Stdin commands:
 *   add <host> <port>           — add TX target (rebuilds pipeline)
 *   remove <host> <port>        — remove TX target
 *   codec <mp3|opus|raw> [br]   — set codec / bitrate
 *   quit                        — clean shutdown
 *
 * Stdout (parsed by Node.js):
 *   ports: <client>:in_1, ...
 *   [rtp_send] ready
 *   stats targets=N codec=X bitrateKbps=N bytesSent=N
 */

#include <gst/gst.h>
#include <jack/jack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

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

static GstElement     *g_pipeline = NULL;
static GstElement     *g_jaksrc   = NULL;   /* jackaudiosrc */
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
        g_jaksrc   = NULL;
    }
}

static int pipeline_build(void)
{
    pipeline_stop();

    int jack_rate = 48000;
    {
        jack_client_t *tmp = jack_client_open("_rate_probe",
            JackNoStartServer | JackUseExactName, NULL);
        if (tmp) {
            jack_rate = (int)jack_get_sample_rate(tmp);
            jack_client_close(tmp);
        }
    }
    int out_rate = (g_out_rate > 0) ? g_out_rate : jack_rate;

    GstElement *pipe    = gst_pipeline_new("rtp_send");
    GstElement *src     = gst_element_factory_make("jackaudiosrc",  "src");
    GstElement *cvt     = gst_element_factory_make("audioconvert",  "cvt");
    GstElement *resamp  = (out_rate != jack_rate) ?
                          gst_element_factory_make("audioresample", "resamp") : NULL;

    if (!src || !cvt) goto fail;
    if (out_rate != jack_rate && !resamp) goto fail;

    /* jackaudiosrc: client name, no auto-connect (Node.js handles JACK wiring) */
    g_object_set(src,
        "client-name", g_client_name,
        "connect",     0,             /* JackConnectNone */
        NULL);

    /* build: src → cvt [→ resamp] → encoder/capsfilter → [payloader] → sink(s) */
    /* Force jackaudiosrc to create g_ch JACK ports via caps filter on src→cvt link */
    GstCaps *src_caps = gst_caps_new_simple("audio/x-raw",
        "channels", G_TYPE_INT, g_ch, NULL);

    if (resamp) {
        gst_bin_add_many(GST_BIN(pipe), src, cvt, resamp, NULL);
        if (!gst_element_link_filtered(src, cvt, src_caps)) { gst_caps_unref(src_caps); goto fail; }
        if (!gst_element_link(cvt, resamp)) { gst_caps_unref(src_caps); goto fail; }
    } else {
        gst_bin_add_many(GST_BIN(pipe), src, cvt, NULL);
        if (!gst_element_link_filtered(src, cvt, src_caps)) { gst_caps_unref(src_caps); goto fail; }
    }
    gst_caps_unref(src_caps);
    GstElement *last_cvt = resamp ? resamp : cvt;

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
    g_jaksrc   = src;

    GstStateChangeReturn ret = gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "[rtp_send] pipeline failed to start PLAYING\n");
        gst_object_unref(g_pipeline);
        g_pipeline = NULL; g_jaksrc = NULL;
        return 0;
    }
    fprintf(stderr, "[rtp_send] pipeline built: %s%s rate=%d→%d targets=%d\n",
            g_codec == CODEC_MP3 ? "mp3" : g_codec == CODEC_OPUS ? "opus" : "raw",
            g_use_rtp ? "+rtp" : "",
            jack_rate, out_rate, n_targets);
    return 1;

fail:
    fprintf(stderr, "[rtp_send] pipeline build failed\n");
    gst_object_unref(pipe);
    return 0;
}

/* ── port reporter ───────────────────────────────── */
typedef struct { int count; int send_ready; } PortPollCtx;


static gboolean report_ports_cb(gpointer data)
{
    PortPollCtx *ctx = (PortPollCtx *)data;
    ctx->count++;

    jack_client_t *probe = jack_client_open("_port_probe",
        JackNoStartServer, NULL);
    if (!probe) {
        if (ctx->count < 20) return G_SOURCE_CONTINUE;
        goto fallback;
    }

    {
        char pattern[128];
        snprintf(pattern, sizeof(pattern), "^%s:", g_client_name);
        const char **ports = jack_get_ports(probe, pattern, NULL, 0);
        int nports = 0;
        if (ports) { while (ports[nports]) nports++; }

        if (nports < g_ch && ctx->count < 20) {
            if (ports) jack_free(ports);
            jack_client_close(probe);
            return G_SOURCE_CONTINUE;
        }

        if (nports > 0) {
            fprintf(stdout, "ports:");
            for (int i = 0; i < nports; i++) {
                if (i) fprintf(stdout, ",");
                fprintf(stdout, " %s", ports[i]);
            }
            fprintf(stdout, "\n");
            jack_free(ports);
            jack_client_close(probe);
            goto done;
        }
        if (ports) jack_free(ports);
        jack_client_close(probe);
    }

fallback:
    fprintf(stdout, "ports:");
    for (int c = 0; c < g_ch; c++) {
        if (c) fprintf(stdout, ",");
        fprintf(stdout, " %s:in_src_%d", g_client_name, c + 1);
    }
    fprintf(stdout, "\n");

done:
    if (ctx->send_ready) {
        fprintf(stdout, "[rtp_send] ready\n");
        fprintf(stderr, "[rtp_send] started client=%s ch=%d use_rtp=%d out_rate=%d\n",
                g_client_name, g_ch, g_use_rtp, g_out_rate);
    }
    fflush(stdout);
    g_free(ctx);
    return G_SOURCE_REMOVE;
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
            /* re-report ports so Node.js can reconnect JACK after rebuild */
            { PortPollCtx *ctx = g_new0(PortPollCtx, 1); ctx->send_ready = 0;
              g_timeout_add(100, report_ports_cb, ctx); }
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
            { PortPollCtx *ctx = g_new0(PortPollCtx, 1); ctx->send_ready = 0;
              g_timeout_add(100, report_ports_cb, ctx); }
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
            { PortPollCtx *ctx = g_new0(PortPollCtx, 1); ctx->send_ready = 0;
              g_timeout_add(100, report_ports_cb, ctx); }
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

    pthread_mutex_lock(&pipe_mutex);
    pipeline_build();
    pthread_mutex_unlock(&pipe_mutex);

    /* poll for JACK ports every 100ms until jackaudiosrc registers them */
    { PortPollCtx *ctx = g_new0(PortPollCtx, 1); ctx->send_ready = 1;
      g_timeout_add(100, report_ports_cb, ctx); }

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
