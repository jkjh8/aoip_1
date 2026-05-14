#pragma once
/*
 * dsp_neon.h — gain/routing/level NEON SIMD 최적화
 *
 * 64-byte 정렬 보장(posix_memalign) 덕분에 정렬 패널티 없이 동작.
 * __ARM_NEON 미정의 시 스칼라 폴백으로 컴파일 가능.
 */

#ifdef __ARM_NEON
#include <arm_neon.h>

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
