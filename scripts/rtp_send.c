/*
 * rtp_send.c — JACK input ports → UDP stream sender
 *
 * JACK client:  rtp_send
 * JACK ports:   rtp_send:in_1 … rtp_send:in_N  (input/sink ports)
 *
 * JACK → ring buffer → GStreamer appsrc → encoder → tee → udpsink × N
 *
 * Codecs:  mp3 (lamemp3enc), opus (opusenc), raw (pcm, no encoding)
 *
 * Usage:   rtp_send [channels=2]
 *
 * Stdin commands:
 *   add <host> <port>           — add TX target (rebuilds pipeline)
 *   remove <host> <port>        — remove TX target
 *   codec <mp3|opus|raw> [br]   — set codec / bitrate
 *   quit                        — clean shutdown
 *
 * Stdout (parsed by Node.js):
 *   [rtp_send] ready
 *   ports: rtp_send:in_1, rtp_send:in_2
 */

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
#include <jack/jack.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdatomic.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>
#include <fcntl.h>

static FILE *g_out = NULL;
static FILE *g_in  = NULL;

#define MAX_CH       8
#define MAX_TARGETS  16
#define RING_FRAMES  16384   /* must be power of 2 */

/* ── ring buffer (JACK → GStreamer) ─────────────── */
typedef struct {
    float       buf[RING_FRAMES * MAX_CH];
    atomic_uint wp;
    atomic_uint rp;
    int         ch;
} Ring;

static Ring ring;

static void ring_write(const float *src, int nframes)
{
    unsigned wp = atomic_load_explicit(&ring.wp, memory_order_relaxed);
    unsigned rp = atomic_load_explicit(&ring.rp, memory_order_acquire);
    if ((int)(RING_FRAMES - (wp - rp)) < nframes) {
        /* drop oldest to make room */
        atomic_fetch_add_explicit(&ring.rp, (unsigned)nframes, memory_order_release);
    }
    for (int i = 0; i < nframes; i++) {
        unsigned idx = (wp + i) & (RING_FRAMES - 1);
        memcpy(&ring.buf[idx * ring.ch], &src[i * ring.ch],
               (unsigned)ring.ch * sizeof(float));
    }
    atomic_store_explicit(&ring.wp, wp + (unsigned)nframes, memory_order_release);
}

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

/* ── target list ─────────────────────────────────── */
typedef struct { char host[128]; int port; } Target;

static Target   targets[MAX_TARGETS];
static int      n_targets = 0;
static pthread_mutex_t target_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── codec config ────────────────────────────────── */
typedef enum { CODEC_MP3, CODEC_OPUS, CODEC_RAW } Codec;

static Codec  g_codec   = CODEC_MP3;
static int    g_bitrate = 320;

/* ── globals ─────────────────────────────────────── */
static jack_client_t *jclient;
static jack_port_t   *jports[MAX_CH];
static int            g_ch      = 2;
static int            g_rate    = 48000;
static int            g_use_rtp = 0;   /* 1 = wrap with RTP payloader */
static volatile int   g_quit    = 0;

static GstElement    *g_pipeline = NULL;
static GstElement    *g_appsrc   = NULL;
static pthread_mutex_t pipe_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── JACK process callback ───────────────────────── */
static float jack_tmp[4096 * MAX_CH];

static int jack_process(jack_nframes_t nframes, void *arg)
{
    (void)arg;
    float *bufs[MAX_CH];
    for (int c = 0; c < g_ch; c++)
        bufs[c] = (float *)jack_port_get_buffer(jports[c], nframes);
    for (jack_nframes_t f = 0; f < nframes; f++)
        for (int c = 0; c < g_ch; c++)
            jack_tmp[f * g_ch + c] = bufs[c][f];
    ring_write(jack_tmp, (int)nframes);
    return 0;
}

/* ── pipeline builder ────────────────────────────── */
static void pipeline_stop(void)
{
    if (g_pipeline) {
        gst_element_set_state(g_pipeline, GST_STATE_NULL);
        gst_object_unref(g_pipeline);
        g_pipeline = NULL;
        g_appsrc   = NULL;
    }
}

static int pipeline_build(void)
{
    pipeline_stop();

    GstElement *pipe = gst_pipeline_new("rtp_send");

    /* appsrc */
    GstElement *appsrc = gst_element_factory_make("appsrc",       "src");
    GstElement *cvt    = gst_element_factory_make("audioconvert", "cvt");

    if (!appsrc || !cvt) goto fail;

    /* appsrc caps: F32LE from JACK */
    GstCaps *src_caps = gst_caps_new_simple("audio/x-raw",
        "format",   G_TYPE_STRING, "F32LE",
        "rate",     G_TYPE_INT,    g_rate,
        "channels", G_TYPE_INT,    g_ch,
        "layout",   G_TYPE_STRING, "interleaved",
        NULL);
    g_object_set(appsrc,
        "caps",        src_caps,
        "format",      GST_FORMAT_TIME,
        "is-live",     TRUE,
        "block",       FALSE,
        "max-bytes",   (guint64)(g_rate * g_ch * sizeof(float) / 4),
        NULL);
    gst_caps_unref(src_caps);

    gst_bin_add_many(GST_BIN(pipe), appsrc, cvt, NULL);
    if (!gst_element_link(appsrc, cvt)) goto fail;

    /* encoder */
    GstElement *enc = NULL;
    GstElement *caps_flt = NULL;

    if (g_codec == CODEC_MP3) {
        enc = gst_element_factory_make("lamemp3enc", "enc");
        if (!enc) goto fail;
        g_object_set(enc, "bitrate", g_bitrate, "cbr", TRUE, NULL);
        gst_bin_add(GST_BIN(pipe), enc);
        if (!gst_element_link(cvt, enc)) goto fail;
    } else if (g_codec == CODEC_OPUS) {
        enc = gst_element_factory_make("opusenc", "enc");
        if (!enc) goto fail;
        g_object_set(enc, "bitrate", g_bitrate * 1000, NULL);
        gst_bin_add(GST_BIN(pipe), enc);
        if (!gst_element_link(cvt, enc)) goto fail;
    } else {
        /* raw PCM: S16LE for network */
        caps_flt = gst_element_factory_make("capsfilter", "cf");
        if (!caps_flt) goto fail;
        GstCaps *raw = gst_caps_new_simple("audio/x-raw",
            "format",   G_TYPE_STRING, g_use_rtp ? "S16BE" : "S16LE",
            "rate",     G_TYPE_INT,    g_rate,
            "channels", G_TYPE_INT,    g_ch,
            "layout",   G_TYPE_STRING, "interleaved",
            NULL);
        g_object_set(caps_flt, "caps", raw, NULL);
        gst_caps_unref(raw);
        gst_bin_add(GST_BIN(pipe), caps_flt);
        if (!gst_element_link(cvt, caps_flt)) goto fail;
    }

    /* previous element (encoder or capsfilter) */
    GstElement *last_enc = enc ? enc : caps_flt;

    /* RTP payloader */
    if (g_use_rtp) {
        GstElement *pay = NULL;
        if      (g_codec == CODEC_MP3)  pay = gst_element_factory_make("rtpmpapay",  "pay");
        else if (g_codec == CODEC_OPUS) pay = gst_element_factory_make("rtpopuspay", "pay");
        else                            pay = gst_element_factory_make("rtpL16pay",  "pay");
        if (!pay) goto fail;
        gst_bin_add(GST_BIN(pipe), pay);
        if (!gst_element_link(last_enc, pay)) goto fail;
        last_enc = pay;
    }

    if (n_targets == 0) {
        /* no targets: drain audio to fakesink */
        GstElement *fake = gst_element_factory_make("fakesink", "fake");
        if (!fake) goto fail;
        g_object_set(fake, "sync", FALSE, NULL);
        gst_bin_add(GST_BIN(pipe), fake);
        if (!gst_element_link(last_enc, fake)) goto fail;
    } else if (n_targets == 1) {
        GstElement *sink = gst_element_factory_make("udpsink", "sink0");
        if (!sink) goto fail;
        g_object_set(sink,
            "host",  targets[0].host,
            "port",  targets[0].port,
            "sync",  FALSE,
            NULL);
        gst_bin_add(GST_BIN(pipe), sink);
        if (!gst_element_link(last_enc, sink)) goto fail;
    } else {
        /* tee + queue + udpsink per target */
        GstElement *tee = gst_element_factory_make("tee", "tee");
        if (!tee) goto fail;
        gst_bin_add(GST_BIN(pipe), tee);
        if (!gst_element_link(last_enc, tee)) goto fail;

        for (int i = 0; i < n_targets; i++) {
            char qname[32], sname[32];
            snprintf(qname, sizeof(qname), "q%d", i);
            snprintf(sname, sizeof(sname), "sink%d", i);
            GstElement *q    = gst_element_factory_make("queue",   qname);
            GstElement *sink = gst_element_factory_make("udpsink", sname);
            if (!q || !sink) goto fail;
            g_object_set(sink,
                "host",  targets[i].host,
                "port",  targets[i].port,
                "sync",  FALSE,
                NULL);
            gst_bin_add_many(GST_BIN(pipe), q, sink, NULL);
            if (!gst_element_link_many(tee, q, sink, NULL)) goto fail;
        }
    }

    GstBus *bus = gst_element_get_bus(pipe);
    gst_object_unref(bus);

    g_pipeline = pipe;
    g_appsrc   = appsrc;

    gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
    return 1;

fail:
    fprintf(stderr, "[rtp_send] pipeline build failed\n");
    gst_object_unref(pipe);
    return 0;
}

/* ── GStreamer push thread ───────────────────────── */
static void *push_thread(void *arg)
{
    (void)arg;
    /* push 256-frame chunks from ring buffer to appsrc */
    const int  chunk  = 256;
    float      tmp[256 * MAX_CH];
    GstClockTime ts   = 0;
    GstClockTime dur  = gst_util_uint64_scale(chunk, GST_SECOND, (guint64)g_rate);

    while (!g_quit) {
        pthread_mutex_lock(&pipe_mutex);
        GstElement *src = g_appsrc;
        pthread_mutex_unlock(&pipe_mutex);

        if (!src) { usleep(2000); continue; }

        if (ring_read(tmp, chunk)) {
            GstBuffer *buf = gst_buffer_new_allocate(
                NULL, (gsize)(chunk * g_ch * sizeof(float)), NULL);
            GstMapInfo map;
            gst_buffer_map(buf, &map, GST_MAP_WRITE);
            memcpy(map.data, tmp, (size_t)(chunk * g_ch) * sizeof(float));
            gst_buffer_unmap(buf, &map);

            GST_BUFFER_PTS(buf)      = ts;
            GST_BUFFER_DURATION(buf) = dur;
            ts += dur;

            gst_app_src_push_buffer(GST_APP_SRC(src), buf);
        } else {
            usleep(1000);
        }
    }
    return NULL;
}

/* ── stdin command thread ────────────────────────── */
static void *stdin_thread(void *arg)
{
    (void)arg;
    char line[256];
    while (!g_quit && fgets(line, sizeof(line), g_in)) {
        /* strip newline */
        size_t len = strlen(line);
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r'))
            line[--len] = '\0';

        char cmd[64], h[128];
        int  p = 0;

        if (sscanf(line, "%63s", cmd) < 1) continue;

        if (strcmp(cmd, "quit") == 0) {
            g_quit = 1;
            break;
        }

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
            fprintf(g_out, "ok\n");
            fflush(g_out);
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
            fprintf(g_out, "ok\n");
            fflush(g_out);
            continue;
        }

        if (strcmp(cmd, "codec") == 0) {
            char codec_str[32] = "mp3";
            int  br = g_bitrate;
            sscanf(line, "%*s %31s %d", codec_str, &br);
            if      (strcmp(codec_str, "opus") == 0) g_codec = CODEC_OPUS;
            else if (strcmp(codec_str, "raw")  == 0) g_codec = CODEC_RAW;
            else                                      g_codec = CODEC_MP3;
            g_bitrate = br;
            pthread_mutex_lock(&pipe_mutex);
            pipeline_build();
            pthread_mutex_unlock(&pipe_mutex);
            fprintf(g_out, "ok\n");
            fflush(g_out);
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
    /* argv[4]: 출력 fd, argv[5]: 입력 fd (Node.js가 전달) */
    if (argc > 4 && atoi(argv[4]) > 2) g_out = fdopen(atoi(argv[4]), "w");
    if (!g_out) g_out = stdout;
    if (argc > 5 && atoi(argv[5]) > 2) g_in  = fdopen(atoi(argv[5]), "r");
    if (!g_in)  g_in  = stdin;

    gst_init(&argc, &argv);
    signal(SIGTERM, on_signal);
    signal(SIGINT,  on_signal);

    g_ch = argc > 1 ? atoi(argv[1]) : 2;
    char client_name[64];
    snprintf(client_name, sizeof(client_name), "%s", argc > 2 ? argv[2] : "rtp_send");
    g_use_rtp = (argc > 3 && strcmp(argv[3], "rtp") == 0) ? 1 : 0;

    memset(&ring, 0, sizeof(ring));
    ring.ch = g_ch;

    /* ── JACK setup ── */
    jclient = jack_client_open(client_name, JackNoStartServer | JackUseExactName, NULL);
    if (!jclient) {
        fprintf(stderr, "[rtp_send] jack_client_open failed\n");
        return 1;
    }
    g_rate = (int)jack_get_sample_rate(jclient);

    for (int c = 0; c < g_ch; c++) {
        char name[32];
        snprintf(name, sizeof(name), "in_%d", c + 1);
        jports[c] = jack_port_register(jclient, name,
                                       JACK_DEFAULT_AUDIO_TYPE,
                                       JackPortIsInput, 0);
        if (!jports[c]) {
            fprintf(stderr, "[rtp_send] jack_port_register %s failed\n", name);
            jack_client_close(jclient);
            return 1;
        }
    }

    jack_set_process_callback(jclient, jack_process, NULL);
    if (jack_activate(jclient)) {
        fprintf(stderr, "[rtp_send] jack_activate failed\n");
        jack_client_close(jclient);
        return 1;
    }

    /* ── GStreamer pipeline (empty: fakesink) ── */
    pthread_mutex_lock(&pipe_mutex);
    pipeline_build();
    pthread_mutex_unlock(&pipe_mutex);

    /* ports first, then ready */
    fprintf(g_out, "ports:");
    for (int c = 0; c < g_ch; c++) {
        if (c) fprintf(g_out, ",");
        fprintf(g_out, " %s:in_%d", client_name, c + 1);
    }
    fprintf(g_out, "\n");
    fprintf(g_out, "[rtp_send] ready\n");
    fflush(g_out);

    /* threads */
    pthread_t push_tid, stdin_tid;
    pthread_create(&push_tid,  NULL, push_thread,  NULL);
    pthread_create(&stdin_tid, NULL, stdin_thread, NULL);

    /* main loop */
    while (!g_quit) {
        g_main_context_iteration(NULL, FALSE);
        usleep(5000);
    }

    pthread_join(stdin_tid, NULL);
    pthread_join(push_tid,  NULL);

    pthread_mutex_lock(&pipe_mutex);
    pipeline_stop();
    pthread_mutex_unlock(&pipe_mutex);

    jack_client_close(jclient);
    fprintf(stderr, "[rtp_send] exiting\n");
    return 0;
}
