#pragma once
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

/* 락프리 SPSC 링버퍼 — ALSA 캡처/재생 스레드와 DSP 스레드 간 오디오 전달용 */
typedef struct {
    float       *buf;
    atomic_uint  wp, rp;
    int          ring_frames;
    int          channels;
} RingBuf;

static inline void rb_init(RingBuf *r, int frames, int ch) {
    r->buf = calloc((size_t)(frames * ch), sizeof(float));
    atomic_init(&r->wp, 0);
    atomic_init(&r->rp, 0);
    r->ring_frames = frames;
    r->channels    = ch;
}

static inline int rb_avail(const RingBuf *r) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_acquire);
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_relaxed);
    return (int)(wp - rp);
}

static inline int rb_free(const RingBuf *r) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_relaxed);
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_acquire);
    return (int)((unsigned)r->ring_frames - (wp - rp));
}

static inline void rb_write(RingBuf *r, const float *src, int n) {
    unsigned wp        = atomic_load_explicit(&r->wp, memory_order_relaxed);
    unsigned rp        = atomic_load_explicit(&r->rp, memory_order_acquire);
    int      free_frm  = (int)((unsigned)r->ring_frames - (wp - rp));
    if (n > free_frm) n = free_frm;
    for (int i = 0; i < n; i++) {
        unsigned idx = (wp + (unsigned)i) % (unsigned)r->ring_frames;
        memcpy(&r->buf[idx * r->channels], &src[i * r->channels],
               (size_t)r->channels * sizeof(float));
    }
    atomic_store_explicit(&r->wp, wp + (unsigned)n, memory_order_release);
}

static inline int rb_read(RingBuf *r, float *dst, int n) {
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_relaxed);
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_acquire);
    if ((int)(wp - rp) < n) return 0;
    for (int i = 0; i < n; i++) {
        unsigned idx = (rp + (unsigned)i) % (unsigned)r->ring_frames;
        memcpy(&dst[i * r->channels], &r->buf[idx * r->channels],
               (size_t)r->channels * sizeof(float));
    }
    atomic_store_explicit(&r->rp, rp + (unsigned)n, memory_order_release);
    return 1;
}

static inline void rb_reset(RingBuf *r) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_relaxed);
    atomic_store_explicit(&r->rp, wp, memory_order_release);
}
