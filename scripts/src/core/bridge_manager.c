#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <pthread.h>
#include <unistd.h>
#include <stdatomic.h>
#include "include/bridge_manager.h"
#include "include/engine_globals.h"
#include "include/dsp_src.h"
#include "include/ring_buf.h"
#include "include/rtp_recv.h"
#include "include/rtp_send.h"
#include "include/clk2.h"

/* ── ALSA 브릿지 start/stop ──────────────────────────────────────── */
void bridge_start(Device *d)
{
    if (!d->is_i2s) {
        int err;
        if (d->mode != 2) {
            d->cap_src   = src_new(SRC_SINC_FASTEST, d->channels, &err);
            d->clk_accum = 0;
            double ratio_hint = d->is_ravenna
                ? atomic_load_explicit(&g_ravenna_ratio_hint, memory_order_relaxed)
                : 1.0;
            d->cap_pi = (PiState){ .ratio=ratio_hint, .kp=RATIO_KP, .ki=RATIO_KI,
                                   .min=RATIO_MIN, .max=RATIO_MAX };
            if (d->is_ravenna) {
                atomic_store_explicit(&d->ravenna_ptp_locked, 0, memory_order_relaxed);
                atomic_store_explicit(&d->ravenna_flush, 0, memory_order_relaxed);
                d->ravenna_prebuf_count = 0;
                d->cap_prebuf_ready = 0;
            }
        }
        if (d->mode != 1) {
            d->play_src = src_new(SRC_SINC_FASTEST, d->channels, &err);
            d->play_pi  = (PiState){ .ratio=1.0, .kp=RATIO_KP, .ki=RATIO_KI,
                                     .min=RATIO_MIN, .max=RATIO_MAX };
        }
    }
    device_start(d);
}

void bridge_stop(Device *d)
{
    device_stop(d);
    if (d->cap_src)  { src_delete(d->cap_src);  d->cap_src  = NULL; }
    if (d->play_src) { src_delete(d->play_src); d->play_src = NULL; }
}

/* ── RTP 스트림 open/close ───────────────────────────────────────── */
typedef struct {
    RingBuf  *ring;
    char      key[32];
    char      sock_path[256];
    int       is_send;
    RtpStream *buf;
} RtpLaunchArg;

static void *rtp_launch_thread(void *arg)
{
    RtpLaunchArg *la = (RtpLaunchArg *)arg;
    /* I2S 베이스라인 보정 완료 대기 (최대 10s). RAVENNA 캡처/재생 스레드와 동일 이유 —
     * 클럭 안정 전에 RTP 송수신을 시작하면 SRC 비율 추정이 흔들리고 초기 burst 가 깨짐. */
    for (int _w = 0; _w < 100; _w++) {
        if (atomic_load_explicit(&g_clk_ready, memory_order_acquire)) break;
        if (g_quit) { free(la); return NULL; }
        usleep(100000);
    }
    if (la->is_send)
        la->buf->rtp_ctx = rtp_send_start(la->ring, la->key, la->sock_path, g_prio_rtp);
    else
        la->buf->rtp_ctx = rtp_recv_start(la->ring, la->key, la->sock_path, g_prio_rtp);
    free(la);
    return NULL;
}

int rtp_stream_open(RtpStream *r, int is_out)
{
    rb_init(&r->ring, RTP_RING_FRAMES, r->channels);
    if (!r->ring.buf) {
        fprintf(stderr, "[aoip_engine] rtp_%s '%s': ring alloc failed\n",
                is_out ? "out" : "in", r->name);
        return 0;
    }
    r->is_send = is_out;
    r->rtp_ctx = NULL;

    if (!is_out) {
        int err;
        r->rtp_src   = src_new(SRC_SINC_FASTEST, r->channels, &err);
        r->rtp_pi    = (PiState){ .ratio=1.0, .kp=RTP_RATIO_KP, .ki=RTP_RATIO_KI,
                                  .min=RTP_RATIO_MIN, .max=RTP_RATIO_MAX };
        r->rtp_underrun = 0;
        r->prebuffering = 1;
    }

    fprintf(stderr, "[aoip_engine] rtp_%s '%s' ch=%d ch_start=%d\n",
            is_out ? "out" : "in", r->name, r->channels, r->ch_start);

    RtpLaunchArg *la = malloc(sizeof(RtpLaunchArg));
    if (!la) {
        fprintf(stderr, "[aoip_engine] rtp_%s '%s': malloc failed\n",
                is_out ? "out" : "in", r->name);
        free(r->ring.buf); r->ring.buf = NULL;
        return 0;
    }
    la->ring    = &r->ring;
    la->is_send = is_out;
    la->buf     = r;
    snprintf(la->key, sizeof(la->key), "%s", r->name);
    snprintf(la->sock_path, sizeof(la->sock_path),
             "/run/aoip/%s_%s.sock",
             is_out ? "rtp_send" : "rtp_recv", r->name);

    pthread_t tid;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setdetachstate(&attr, PTHREAD_CREATE_DETACHED);
    pthread_create(&tid, &attr, rtp_launch_thread, la);
    pthread_attr_destroy(&attr);
    return 1;
}

void rtp_stream_close(RtpStream *r)
{
    if (r->rtp_ctx) {
        if (r->is_send) rtp_send_stop((RtpSendCtx *)r->rtp_ctx);
        else            rtp_recv_stop((RtpRecvCtx *)r->rtp_ctx);
        r->rtp_ctx = NULL;
    }
    if (r->rtp_src) { src_delete(r->rtp_src); r->rtp_src = NULL; }
    free(r->ring.buf); r->ring.buf = NULL;
    r->enabled = 0;
}
