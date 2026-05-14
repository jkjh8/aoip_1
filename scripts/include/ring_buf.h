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

static inline int rb_write(RingBuf *r, const float *src, int n) {
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
    return n;
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

/* ── SlotRing: 슬롯 기반 플래너 링버퍼 (I2S zero-copy 전용) ────────
 * ch_slots[ch][slot*period + f] 레이아웃, 64-byte 정렬.
 * SPSC: 캡처 스레드(writer) ↔ DSP/재생 스레드(reader).
 * acquire → 슬롯 포인터 직접 반환(memcpy 없음) → commit/consume.
 */
typedef struct {
    float      **ch_slots;  /* [channels] 각각 slot_count*period floats, 64-byte 정렬 */
    atomic_uint  wp, rp;    /* slot 인덱스 (frame 인덱스 아님) */
    int          slot_count;
    int          period;    /* MAX_PERIOD_FRAMES 고정으로 초기화, 런타임 g_period_frames 사용 */
    int          channels;
} SlotRing;

static inline int slot_ring_init(SlotRing *r, int slot_count, int period, int ch) {
    r->slot_count = slot_count;
    r->period     = period;
    r->channels   = ch;
    atomic_init(&r->wp, 0);
    atomic_init(&r->rp, 0);
    r->ch_slots = (float **)calloc((size_t)ch, sizeof(float *));
    if (!r->ch_slots) return 0;
    for (int c = 0; c < ch; c++) {
        size_t sz = (size_t)(slot_count * period) * sizeof(float);
        if (posix_memalign((void **)&r->ch_slots[c], 64, sz) != 0) {
            for (int k = 0; k < c; k++) free(r->ch_slots[k]);
            free(r->ch_slots); r->ch_slots = NULL;
            return 0;
        }
        memset(r->ch_slots[c], 0, sz);
    }
    return 1;
}

static inline void slot_ring_destroy(SlotRing *r) {
    if (!r->ch_slots) return;
    for (int c = 0; c < r->channels; c++) free(r->ch_slots[c]);
    free(r->ch_slots);
    r->ch_slots = NULL;
}

static inline int slot_ring_avail(const SlotRing *r) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_acquire);
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_relaxed);
    return (int)(wp - rp);
}

static inline int slot_ring_free_slots(const SlotRing *r) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_relaxed);
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_acquire);
    return r->slot_count - (int)(wp - rp);
}

/* writer: wp 슬롯 포인터 배열을 ptrs에 채움. 공간 없으면 0 반환 */
static inline int slot_ring_acquire_write(SlotRing *r, float **ptrs) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_relaxed);
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_acquire);
    if ((int)(wp - rp) >= r->slot_count) return 0;
    unsigned slot = wp % (unsigned)r->slot_count;
    for (int c = 0; c < r->channels; c++)
        ptrs[c] = &r->ch_slots[c][(int)slot * r->period];
    return 1;
}

static inline void slot_ring_commit_write(SlotRing *r) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_relaxed);
    atomic_store_explicit(&r->wp, wp + 1u, memory_order_release);
}

/* reader: rp 슬롯 포인터 배열을 ptrs에 채움. 데이터 없으면 0 반환 */
static inline int slot_ring_acquire_read(SlotRing *r, float **ptrs) {
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_relaxed);
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_acquire);
    if ((int)(wp - rp) < 1) return 0;
    unsigned slot = rp % (unsigned)r->slot_count;
    for (int c = 0; c < r->channels; c++)
        ptrs[c] = &r->ch_slots[c][(int)slot * r->period];
    return 1;
}

static inline void slot_ring_consume_read(SlotRing *r) {
    unsigned rp = atomic_load_explicit(&r->rp, memory_order_relaxed);
    atomic_store_explicit(&r->rp, rp + 1u, memory_order_release);
}

static inline void slot_ring_reset(SlotRing *r) {
    unsigned wp = atomic_load_explicit(&r->wp, memory_order_relaxed);
    atomic_store_explicit(&r->rp, wp, memory_order_release);
}
