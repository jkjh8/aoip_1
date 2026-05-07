#pragma once
/*
 * dsp_neon.h — gain/routing/level NEON SIMD 최적화
 *
 * 64-byte 정렬 보장(posix_memalign) 덕분에 정렬 패널티 없이 동작.
 * __ARM_NEON 미정의 시 스칼라 폴백으로 컴파일 가능.
 */

#ifdef __ARM_NEON
#include <arm_neon.h>

/* gain linear ramp: buf[i] *= (start + step*(i+1)), start→tgt 1 period 내 완료 */
static inline void gain_ramp_neon(float *buf, float start, float tgt, int frames) {
    float step = (tgt - start) / (float)frames;
    float32x4_t lane_init = { step, 2.0f*step, 3.0f*step, 4.0f*step };
    float32x4_t stride    = vdupq_n_f32(4.0f * step);
    float32x4_t gains     = vaddq_f32(vdupq_n_f32(start), lane_init);
    int i = 0;
    for (; i <= frames - 4; i += 4) {
        vst1q_f32(buf + i, vmulq_f32(vld1q_f32(buf + i), gains));
        gains = vaddq_f32(gains, stride);
    }
    float cur = start + step * (float)i;
    for (; i < frames; i++) { cur += step; buf[i] *= cur; }
}

/* 2채널 인터리빙: [ch0[i], ch1[i]] → tmp[i*2, i*2+1]  (vst2q 4-wide) */
static inline void interleave_2ch_neon(const float *ch0, const float *ch1,
                                       float *tmp, int frames) {
    int i = 0;
    for (; i <= frames - 4; i += 4) {
        float32x4x2_t v;
        v.val[0] = vld1q_f32(ch0 + i);
        v.val[1] = vld1q_f32(ch1 + i);
        vst2q_f32(tmp + i * 2, v);
    }
    for (; i < frames; i++) {
        tmp[i * 2]     = ch0[i];
        tmp[i * 2 + 1] = ch1[i];
    }
}

/* gain 적용: buf[i] *= gain  (4-wide NEON) */
static inline void gain_apply_neon(float *buf, float gain, int frames) {
    float32x4_t g = vdupq_n_f32(gain);
    int i = 0;
    for (; i <= frames - 4; i += 4)
        vst1q_f32(buf + i, vmulq_f32(vld1q_f32(buf + i), g));
    for (; i < frames; i++) buf[i] *= gain;
}

/* routing 누적: out[i] += in[i] * gain  (4-wide NEON FMA) */
static inline void route_add_neon(float *restrict out,
                                   const float *restrict in,
                                   float gain, int frames) {
    float32x4_t g = vdupq_n_f32(gain);
    int i = 0;
    for (; i <= frames - 4; i += 4)
        vst1q_f32(out + i, vmlaq_f32(vld1q_f32(out + i), vld1q_f32(in + i), g));
    for (; i < frames; i++) out[i] += in[i] * gain;
}

/* peak 레벨 측정: max(abs(buf[i]))  (4-wide NEON) */
static inline float level_peak_neon(const float *buf, int frames) {
    float32x4_t mx = vdupq_n_f32(0.0f);
    int i = 0;
    for (; i <= frames - 4; i += 4)
        mx = vmaxq_f32(mx, vabsq_f32(vld1q_f32(buf + i)));
    float peak = vmaxvq_f32(mx);
    for (; i < frames; i++) { float a = buf[i] < 0.0f ? -buf[i] : buf[i]; if (a > peak) peak = a; }
    return peak;
}

#else  /* 스칼라 폴백 */

static inline void gain_ramp_neon(float *buf, float start, float tgt, int frames) {
    float step = (tgt - start) / (float)frames;
    float cur  = start;
    for (int i = 0; i < frames; i++) { cur += step; buf[i] *= cur; }
}

static inline void interleave_2ch_neon(const float *ch0, const float *ch1,
                                       float *tmp, int frames) {
    for (int i = 0; i < frames; i++) {
        tmp[i * 2]     = ch0[i];
        tmp[i * 2 + 1] = ch1[i];
    }
}

static inline void gain_apply_neon(float *buf, float gain, int frames) {
    for (int i = 0; i < frames; i++) buf[i] *= gain;
}

static inline void route_add_neon(float *restrict out,
                                   const float *restrict in,
                                   float gain, int frames) {
    for (int i = 0; i < frames; i++) out[i] += in[i] * gain;
}

static inline float level_peak_neon(const float *buf, int frames) {
    float peak = 0.0f;
    for (int i = 0; i < frames; i++) {
        float a = buf[i] < 0.0f ? -buf[i] : buf[i];
        if (a > peak) peak = a;
    }
    return peak;
}

#endif /* __ARM_NEON */
