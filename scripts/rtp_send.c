/*
 * rtp_send.c — GStreamer RTP/UDP sender (shared memory input)
 *
 * GStreamer pipeline:
 *   appsrc(F32LE @ out_rate) → audioconvert → capsfilter
 *     → [rtpL16pay|rtpmpapay|rtpopuspay] → multiudpsink
 *
 * 샘플레이트 변환은 reader thread에서 libsamplerate로 처리.
 * (GStreamer audioresample 제거 → CPU 대폭 절감)
 *
 * multiudpsink를 사용해 파이프라인 재빌드 없이 동적으로 대상 추가/제거.
 * 코덱 변경 시에만 pipeline_build() 재호출.
 *
 * ShmRing reader thread: aoip_engine의 rtp_out shm을 폴링,
 *   PERIOD_FRAMES 단위로 appsrc에 공급.
 *
 * Usage:  rtp_send <channels> <client> <proto> <outRate> shm <shm_name>
 *
 * Stdin commands:
 *   add <host> <port>
 *   remove <host> <port>
 *   codec <mp3|opus|raw> [br]
 *   quit
 */

#include <gst/gst.h>
#include <gst/app/gstappsrc.h>
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
#include <errno.h>

#define MAX_TARGETS  16
#define PERIOD_FRAMES 512
#define SAMPLE_RATE   48000

/* ── ShmRing (aoip_engine.c 동일 레이아웃) ──────────── */
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
static int           g_out_rate = 0;
static volatile int  g_quit     = 0;
static char          g_client_name[64] = "rtp_send";

/* shm 입력 */
static char     g_shm_name[256] = "";
static int      g_shm_fd        = -1;
static ShmRing *g_shm           = NULL;

/* GStreamer */
static GstElement     *g_pipeline    = NULL;
static GstAppSrc      *g_appsrc      = NULL;
static GstElement     *g_multiudpsink = NULL;
static pthread_mutex_t pipe_mutex     = PTHREAD_MUTEX_INITIALIZER;

/* libsamplerate (SRC): 48kHz → g_out_rate, reader thread에서 사용 */
static SRC_STATE      *g_src        = NULL;

/* shm reader 스레드 */
static pthread_t       g_reader_tid;
static volatile int    g_reader_run = 0;

/* stats */
static atomic_ulong    g_bytes_sent = 0;

/* ── shm attach ──────────────────────────────────── */
static int shm_attach(void) {
    for (int i = 0; i < 50; i++) {
        g_shm_fd = shm_open(g_shm_name, O_RDWR, 0);
        if (g_shm_fd >= 0) break;
        usleep(100000);
    }
    if (g_shm_fd < 0) {
        fprintf(stderr, "[rtp_send] shm_open(%s) failed: %s\n",
                g_shm_name, strerror(errno));
        return 0;
    }
    g_shm = mmap(NULL, SHMRING_SIZE, PROT_READ | PROT_WRITE, MAP_SHARED, g_shm_fd, 0);
    if (g_shm == MAP_FAILED) {
        fprintf(stderr, "[rtp_send] mmap(%s) failed: %s\n", g_shm_name, strerror(errno));
        close(g_shm_fd); g_shm_fd = -1; g_shm = NULL;
        return 0;
    }
    fprintf(stderr, "[rtp_send] attached shm %s\n", g_shm_name);
    return 1;
}

/* ── udpsink probe: byte count ───────────────────── */
static GstPadProbeReturn udp_out_probe(GstPad *pad, GstPadProbeInfo *info, gpointer data)
{
    (void)pad; (void)data;
    GstBuffer *buf = GST_PAD_PROBE_INFO_BUFFER(info);
    if (buf) atomic_fetch_add_explicit(&g_bytes_sent, gst_buffer_get_size(buf), memory_order_relaxed);
    return GST_PAD_PROBE_OK;
}

/* ── bus handler ─────────────────────────────────── */
static gboolean bus_cb(GstBus *bus, GstMessage *msg, gpointer data)
{
    (void)bus; (void)data;
    if (GST_MESSAGE_TYPE(msg) == GST_MESSAGE_ERROR) {
        GError *err = NULL; gchar *dbg = NULL;
        gst_message_parse_error(msg, &err, &dbg);
        fprintf(stderr, "[rtp_send] gst error: %s (%s)\n", err->message, dbg ? dbg : "");
        g_error_free(err); g_free(dbg);
    }
    return TRUE;
}

/* ── pipeline stop ───────────────────────────────── */
static void pipeline_stop(void)
{
    if (g_pipeline) {
        gst_element_set_state(g_pipeline, GST_STATE_NULL);
        gst_object_unref(g_pipeline);
        g_pipeline     = NULL;
        g_appsrc       = NULL;
        g_multiudpsink = NULL;
    }
}

/* ── pipeline build ──────────────────────────────── */
static int pipeline_build(void)
{
    pipeline_stop();

    /* 샘플레이트 변환은 reader thread(libsamplerate)에서 처리.
     * appsrc에는 이미 out_rate로 변환된 F32LE가 들어옴. */
    int out_rate = (g_out_rate > 0) ? g_out_rate : SAMPLE_RATE;

    GstElement *pipe = gst_pipeline_new("rtp_send");
    GstElement *src  = gst_element_factory_make("appsrc",       "src");
    GstElement *cf   = gst_element_factory_make("capsfilter",   "cf_in");
    GstElement *cvt  = gst_element_factory_make("audioconvert", "cvt");

    if (!src || !cf || !cvt) goto fail;

    /* appsrc caps: F32LE interleaved @ out_rate (이미 변환 완료) */
    GstCaps *in_caps = gst_caps_new_simple("audio/x-raw",
        "format",   G_TYPE_STRING, "F32LE",
        "rate",     G_TYPE_INT,    out_rate,
        "channels", G_TYPE_INT,    g_ch,
        "layout",   G_TYPE_STRING, "interleaved", NULL);
    g_object_set(src, "caps",         in_caps,
                      "format",       GST_FORMAT_TIME,
                      "is-live",      TRUE,
                      "do-timestamp", TRUE,
                      "block",        FALSE,
                      NULL);
    gst_caps_unref(in_caps);
    g_appsrc = GST_APP_SRC(src);
    gst_app_src_set_stream_type(g_appsrc, GST_APP_STREAM_TYPE_STREAM);
    gst_app_src_set_max_bytes(g_appsrc, (guint64)(out_rate * g_ch * sizeof(float) / 2));

    gst_bin_add_many(GST_BIN(pipe), src, cf, cvt, NULL);
    if (!gst_element_link(src, cf))  goto fail;
    if (!gst_element_link(cf,  cvt)) goto fail;
    GstElement *last_cvt = cvt;

    GstElement *enc = NULL, *caps_flt = NULL;
    if (g_codec == CODEC_MP3) {
        enc = gst_element_factory_make("lamemp3enc", "enc");
        if (!enc) goto fail;
        g_object_set(enc, "bitrate", g_bitrate, "cbr", TRUE,
                          "encoding-engine-quality", 0, NULL);  /* 0=fast */
        gst_bin_add(GST_BIN(pipe), enc);
        if (!gst_element_link(last_cvt, enc)) goto fail;
    } else if (g_codec == CODEC_OPUS) {
        enc = gst_element_factory_make("opusenc", "enc");
        if (!enc) goto fail;
        g_object_set(enc, "bitrate", g_bitrate * 1000, NULL);
        gst_bin_add(GST_BIN(pipe), enc);
        if (!gst_element_link(last_cvt, enc)) goto fail;
    } else {
        caps_flt = gst_element_factory_make("capsfilter", "cf");
        if (!caps_flt) goto fail;
        GstCaps *raw = gst_caps_new_simple("audio/x-raw",
            "format",   G_TYPE_STRING, g_use_rtp ? "S16BE" : "S16LE",
            "rate",     G_TYPE_INT,    out_rate,
            "channels", G_TYPE_INT,    g_ch,
            "layout",   G_TYPE_STRING, "interleaved", NULL);
        g_object_set(caps_flt, "caps", raw, NULL);
        gst_caps_unref(raw);
        gst_bin_add(GST_BIN(pipe), caps_flt);
        if (!gst_element_link(last_cvt, caps_flt)) goto fail;
    }
    GstElement *last = enc ? enc : caps_flt;

    if (g_use_rtp) {
        GstElement *pay = NULL;
        if      (g_codec == CODEC_MP3)  pay = gst_element_factory_make("rtpmpapay",  "pay");
        else if (g_codec == CODEC_OPUS) pay = gst_element_factory_make("rtpopuspay", "pay");
        else                            pay = gst_element_factory_make("rtpL16pay",  "pay");
        if (!pay) goto fail;
        if (g_codec == CODEC_RAW) {
            int pt = -1;
            if (out_rate == 44100) { if (g_ch == 2) pt = 10; else if (g_ch == 1) pt = 11; }
            if (pt >= 0) g_object_set(pay, "pt", pt, NULL);
        }
        /* 클라이언트 이름에서 결정론적 SSRC 생성 (재시작 후에도 수신측이 동일 스트림 식별) */
        guint32 ssrc = 0;
        for (int i = 0; g_client_name[i]; i++)
            ssrc = ssrc * 31u + (unsigned char)g_client_name[i];
        ssrc |= 0x80000000u;
        g_object_set(pay, "ssrc", ssrc, NULL);
        gst_bin_add(GST_BIN(pipe), pay);
        if (!gst_element_link(last, pay)) goto fail;
        last = pay;
    }

    /* ── sink: multiudpsink — 파이프라인 재빌드 없이 동적 대상 추가/제거 ── */
    GstElement *sink = gst_element_factory_make("multiudpsink", "sink");
    if (!sink) goto fail;
    g_object_set(sink, "sync", FALSE, "bind-port", 0, NULL);
    gst_bin_add(GST_BIN(pipe), sink);
    if (!gst_element_link(last, sink)) goto fail;

    /* byte count probe */
    GstPad *sp = gst_element_get_static_pad(sink, "sink");
    if (sp) {
        gst_pad_add_probe(sp, GST_PAD_PROBE_TYPE_BUFFER, udp_out_probe, NULL, NULL);
        gst_object_unref(sp);
    }

    GstBus *bus = gst_element_get_bus(pipe);
    if (bus) { gst_bus_add_watch(bus, bus_cb, NULL); gst_object_unref(bus); }

    g_pipeline     = pipe;
    g_multiudpsink = sink;

    GstStateChangeReturn ret = gst_element_set_state(g_pipeline, GST_STATE_PLAYING);
    if (ret == GST_STATE_CHANGE_FAILURE) {
        fprintf(stderr, "[rtp_send] pipeline failed to start PLAYING\n");
        gst_object_unref(g_pipeline); g_pipeline = NULL; g_appsrc = NULL; g_multiudpsink = NULL;
        return 0;
    }

    /* 기존 대상 복원 */
    pthread_mutex_lock(&target_mutex);
    for (int i = 0; i < n_targets; i++)
        g_signal_emit_by_name(g_multiudpsink, "add", targets[i].host, (gint)targets[i].port, NULL);
    int nt = n_targets;
    pthread_mutex_unlock(&target_mutex);

    fprintf(stderr, "[rtp_send] pipeline built: %s%s rate=%d→%d targets=%d\n",
            g_codec == CODEC_MP3 ? "mp3" : g_codec == CODEC_OPUS ? "opus" : "raw",
            g_use_rtp ? "+rtp" : "", SAMPLE_RATE, out_rate, nt);
    return 1;

fail:
    fprintf(stderr, "[rtp_send] pipeline build failed\n");
    gst_object_unref(pipe); g_appsrc = NULL; g_multiudpsink = NULL;
    return 0;
}

/* ── shm reader thread ───────────────────────────── */
static void *shm_reader_thread(void *arg)
{
    (void)arg;

    int out_rate    = (g_out_rate > 0) ? g_out_rate : SAMPLE_RATE;
    int need_resamp = (out_rate != SAMPLE_RATE) && (g_src != NULL);

    /* 입력 버퍼 (F32LE @ 48kHz, PERIOD_FRAMES) */
    float *in_buf = calloc(PERIOD_FRAMES * g_ch, sizeof(float));

    /* 출력 버퍼 (F32LE @ out_rate): 최대 프레임 수 여유 있게 할당 */
    int out_max = need_resamp
        ? (int)((double)PERIOD_FRAMES * out_rate / SAMPLE_RATE * 1.1 + 64)
        : PERIOD_FRAMES;
    float *out_buf = calloc(out_max * g_ch, sizeof(float));

    GstClockTime ts = 0;

    while (g_reader_run) {
        if (!g_shm) {
            if (!shm_attach()) { usleep(100000); continue; }
        }

        ShmRing *ring = g_shm;
        uint32_t wp = atomic_load_explicit(&ring->wp, memory_order_acquire);
        uint32_t rp = atomic_load_explicit(&ring->rp, memory_order_relaxed);

        int got_data = ((int32_t)(wp - rp) >= PERIOD_FRAMES);
        if (got_data) {
            for (int f = 0; f < PERIOD_FRAMES; f++) {
                uint32_t idx = (rp + (uint32_t)f) % (uint32_t)SHM_RING_FRAMES;
                for (int c = 0; c < g_ch && c < SHM_MAX_CH; c++)
                    in_buf[f * g_ch + c] = ring->buf[idx * SHM_MAX_CH + c];
            }
            atomic_store_explicit(&ring->rp, rp + (uint32_t)PERIOD_FRAMES,
                                  memory_order_release);
        } else {
            memset(in_buf, 0, (size_t)(PERIOD_FRAMES * g_ch) * sizeof(float));
            usleep(1000);
        }

        /* libsamplerate로 변환 (필요한 경우) */
        float *push_buf;
        int    push_frames;
        if (need_resamp) {
            SRC_DATA sd = {
                .data_in       = in_buf,
                .data_out      = out_buf,
                .input_frames  = PERIOD_FRAMES,
                .output_frames = out_max,
                .src_ratio     = (double)out_rate / SAMPLE_RATE,
                .end_of_input  = 0,
            };
            src_process(g_src, &sd);
            push_buf    = out_buf;
            push_frames = (int)sd.output_frames_gen;
        } else {
            push_buf    = in_buf;
            push_frames = PERIOD_FRAMES;
        }

        if (push_frames <= 0) continue;

        GstClockTime period_ns = (GstClockTime)(
            (uint64_t)push_frames * 1000000000ULL / (uint64_t)out_rate);
        size_t push_bytes = (size_t)(push_frames * g_ch) * sizeof(float);

        pthread_mutex_lock(&pipe_mutex);
        if (g_appsrc) {
            GstBuffer *gbuf = gst_buffer_new_allocate(NULL, push_bytes, NULL);
            GstMapInfo map;
            if (gst_buffer_map(gbuf, &map, GST_MAP_WRITE)) {
                memcpy(map.data, push_buf, push_bytes);
                gst_buffer_unmap(gbuf, &map);
            }
            GST_BUFFER_PTS(gbuf)      = ts;
            GST_BUFFER_DURATION(gbuf) = period_ns;
            ts += period_ns;
            gst_app_src_push_buffer(g_appsrc, gbuf);
        }
        pthread_mutex_unlock(&pipe_mutex);
    }

    free(in_buf);
    free(out_buf);
    return NULL;
}

/* ── stats thread ────────────────────────────────── */
static void *stats_thread(void *arg)
{
    (void)arg;
    unsigned long prev_bytes = 0;
    while (!g_quit) {
        sleep(2);
        unsigned long cur  = atomic_load(&g_bytes_sent);
        int           kbps = (int)((cur - prev_bytes) * 8 / 2 / 1000);
        prev_bytes = cur;
        const char *cs = g_codec == CODEC_OPUS ? "opus" :
                         g_codec == CODEC_RAW  ? "raw"  : "mp3";
        pthread_mutex_lock(&target_mutex);
        int nt = n_targets;
        pthread_mutex_unlock(&target_mutex);
        fprintf(stdout, "stats targets=%d codec=%s bitrateKbps=%d bytesSent=%lu\n",
                nt, cs, kbps, cur);
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
        while (len > 0 && (line[len-1] == '\n' || line[len-1] == '\r')) line[--len] = '\0';

        char cmd[64], h[128]; int p = 0;
        if (sscanf(line, "%63s", cmd) < 1) continue;
        if (strcmp(cmd, "quit") == 0) { g_quit = 1; break; }

        if (strcmp(cmd, "add") == 0 && sscanf(line, "%*s %127s %d", h, &p) == 2) {
            int found = 0;
            pthread_mutex_lock(&target_mutex);
            for (int i = 0; i < n_targets; i++)
                if (strcmp(targets[i].host, h) == 0 && targets[i].port == p) { found = 1; break; }
            if (!found && n_targets < MAX_TARGETS) {
                snprintf(targets[n_targets].host, sizeof(targets[0].host), "%s", h);
                targets[n_targets++].port = p;
            }
            pthread_mutex_unlock(&target_mutex);
            if (!found) {
                pthread_mutex_lock(&pipe_mutex);
                if (g_multiudpsink)
                    g_signal_emit_by_name(g_multiudpsink, "add", h, (gint)p, NULL);
                pthread_mutex_unlock(&pipe_mutex);
            }
            pthread_mutex_lock(&target_mutex);
            fprintf(stderr, "[rtp_send] add target %s:%d (total=%d)\n", h, p, n_targets);
            pthread_mutex_unlock(&target_mutex);
            continue;
        }

        if (strcmp(cmd, "remove") == 0 && sscanf(line, "%*s %127s %d", h, &p) == 2) {
            int was_there = 0;
            pthread_mutex_lock(&target_mutex);
            for (int i = 0; i < n_targets; i++) {
                if (strcmp(targets[i].host, h) == 0 && targets[i].port == p) {
                    targets[i] = targets[--n_targets]; was_there = 1; break;
                }
            }
            int nt = n_targets;
            pthread_mutex_unlock(&target_mutex);
            if (was_there) {
                pthread_mutex_lock(&pipe_mutex);
                if (g_multiudpsink)
                    g_signal_emit_by_name(g_multiudpsink, "remove", h, (gint)p, NULL);
                pthread_mutex_unlock(&pipe_mutex);
            }
            fprintf(stderr, "[rtp_send] remove target %s:%d (total=%d)\n", h, p, nt);
            continue;
        }

        if (strcmp(cmd, "codec") == 0) {
            char cs[32] = "raw"; int br = g_bitrate;
            sscanf(line, "%*s %31s %d", cs, &br);
            Codec nc = (strcmp(cs, "mp3") == 0) ? CODEC_MP3 :
                       (strcmp(cs, "opus") == 0) ? CODEC_OPUS : CODEC_RAW;
            if (nc == g_codec && br == g_bitrate) {
                fprintf(stderr, "[rtp_send] codec unchanged (%s %d)\n", cs, br); continue;
            }
            g_codec = nc; g_bitrate = br;
            pthread_mutex_lock(&pipe_mutex);
            pipeline_build();   /* 코덱 변경 시에만 재빌드 */
            pthread_mutex_unlock(&pipe_mutex);
            fprintf(stderr, "[rtp_send] codec=%s bitrate=%d\n", cs, br);
            continue;
        }

        fprintf(stderr, "[rtp_send] unknown command: %s\n", cmd);
    }
    return NULL;
}

/* ── signal ──────────────────────────────────────── */
static void on_signal(int sig) { (void)sig; g_quit = 1; }

/* ── main ────────────────────────────────────────── */
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

    if (argc > 5 && strcmp(argv[5], "shm") == 0 && argc > 6)
        snprintf(g_shm_name, sizeof(g_shm_name), "%s", argv[6]);

    if (!g_shm_name[0]) {
        fprintf(stderr, "[rtp_send] shm name required (argv[5]=shm argv[6]=<name>)\n");
        return 1;
    }

    /* libsamplerate 초기화 (out_rate ≠ 48kHz 인 경우에만) */
    if (g_out_rate > 0 && g_out_rate != SAMPLE_RATE) {
        int src_err;
        g_src = src_new(SRC_SINC_FASTEST, g_ch, &src_err);
        if (!g_src) {
            fprintf(stderr, "[rtp_send] src_new failed: %s\n", src_strerror(src_err));
            return 1;
        }
        fprintf(stderr, "[rtp_send] resampler: %d→%d (libsamplerate SRC_SINC_FASTEST)\n",
                SAMPLE_RATE, g_out_rate);
    }

    shm_attach();

    g_reader_run = 1;
    pthread_create(&g_reader_tid, NULL, shm_reader_thread, NULL);

    pthread_mutex_lock(&pipe_mutex);
    pipeline_build();
    pthread_mutex_unlock(&pipe_mutex);

    fprintf(stdout, "[rtp_send] ready\n");
    fflush(stdout);

    pthread_t stdin_tid, stats_tid;
    pthread_create(&stdin_tid, NULL, stdin_thread, NULL);
    pthread_create(&stats_tid, NULL, stats_thread,  NULL);

    GMainLoop *loop = g_main_loop_new(NULL, FALSE);
    while (!g_quit) {
        g_main_context_iteration(NULL, FALSE);
        usleep(1000);
    }
    g_main_loop_unref(loop);

    g_reader_run = 0;
    pthread_join(g_reader_tid, NULL);
    pthread_join(stdin_tid,    NULL);
    pthread_join(stats_tid,    NULL);

    pthread_mutex_lock(&pipe_mutex);
    pipeline_stop();
    pthread_mutex_unlock(&pipe_mutex);

    if (g_shm)      { munmap(g_shm, SHMRING_SIZE); g_shm = NULL; }
    if (g_shm_fd >= 0) { close(g_shm_fd); g_shm_fd = -1; }
    if (g_src)      { src_delete(g_src); g_src = NULL; }

    fprintf(stderr, "[rtp_send] exiting\n");
    return 0;
}
