#define _GNU_SOURCE
#include <math.h>
#include <string.h>
#include "include/dsp_eq.h"

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

float trim_db_to_linear(float db)
{
    if (db >  20.0f) db =  20.0f;
    if (db < -20.0f) db = -20.0f;
    return powf(10.0f, db / 20.0f);
}

/* ── biquad 계수 계산 (Audio EQ Cookbook, RBJ) ───────────────────── */

void biquad_calc_peak(Biquad *b, float fc, float q, float gain_db, float sr)
{
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * (float)M_PI * fc / sr;
    float alpha = sinf(w0) / (2.0f * q);
    float cos_w = cosf(w0);
    float a0    = 1.0f + alpha / A;

    b->b0 = (1.0f + alpha * A) / a0;
    b->b1 = (-2.0f * cos_w)    / a0;
    b->b2 = (1.0f - alpha * A) / a0;
    b->a1 = (-2.0f * cos_w)    / a0;
    b->a2 = (1.0f - alpha / A) / a0;
}

void biquad_calc_low_shelf(Biquad *b, float fc, float q, float gain_db, float sr)
{
    (void)q;
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * (float)M_PI * fc / sr;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w * 0.5f * sqrtf(2.0f);
    float sqA   = sqrtf(A);

    float a0 = (A+1.0f) + (A-1.0f)*cos_w + 2.0f*sqA*alpha;
    b->b0 = (A * ((A+1.0f) - (A-1.0f)*cos_w + 2.0f*sqA*alpha)) / a0;
    b->b1 = (2.0f*A * ((A-1.0f) - (A+1.0f)*cos_w))              / a0;
    b->b2 = (A * ((A+1.0f) - (A-1.0f)*cos_w - 2.0f*sqA*alpha)) / a0;
    b->a1 = (-2.0f * ((A-1.0f) + (A+1.0f)*cos_w))               / a0;
    b->a2 = ((A+1.0f) + (A-1.0f)*cos_w - 2.0f*sqA*alpha)        / a0;
}

void biquad_calc_high_shelf(Biquad *b, float fc, float q, float gain_db, float sr)
{
    (void)q;
    float A     = powf(10.0f, gain_db / 40.0f);
    float w0    = 2.0f * (float)M_PI * fc / sr;
    float cos_w = cosf(w0);
    float sin_w = sinf(w0);
    float alpha = sin_w * 0.5f * sqrtf(2.0f);
    float sqA   = sqrtf(A);

    float a0 = (A+1.0f) - (A-1.0f)*cos_w + 2.0f*sqA*alpha;
    b->b0 = (A * ((A+1.0f) + (A-1.0f)*cos_w + 2.0f*sqA*alpha)) / a0;
    b->b1 = (-2.0f*A * ((A-1.0f) + (A+1.0f)*cos_w))             / a0;
    b->b2 = (A * ((A+1.0f) + (A-1.0f)*cos_w - 2.0f*sqA*alpha)) / a0;
    b->a1 = (2.0f * ((A-1.0f) - (A+1.0f)*cos_w))                / a0;
    b->a2 = ((A+1.0f) - (A-1.0f)*cos_w - 2.0f*sqA*alpha)        / a0;
}

void biquad_calc_notch(Biquad *b, float fc, float q, float sr)
{
    float w0    = 2.0f * (float)M_PI * fc / sr;
    float alpha = sinf(w0) / (2.0f * q);
    float cos_w = cosf(w0);
    float a0    = 1.0f + alpha;

    b->b0 =  1.0f         / a0;
    b->b1 = (-2.0f*cos_w) / a0;
    b->b2 =  1.0f         / a0;
    b->a1 = (-2.0f*cos_w) / a0;
    b->a2 = (1.0f-alpha)  / a0;
}

void biquad_calc_hpf(Biquad *b, float fc, float q, float sr)
{
    float w0    = 2.0f * (float)M_PI * fc / sr;
    float alpha = sinf(w0) / (2.0f * q);
    float cos_w = cosf(w0);
    float a0    = 1.0f + alpha;

    b->b0 =  (1.0f + cos_w) * 0.5f / a0;
    b->b1 = -(1.0f + cos_w)         / a0;
    b->b2 =  (1.0f + cos_w) * 0.5f / a0;
    b->a1 = (-2.0f * cos_w)         / a0;
    b->a2 =  (1.0f - alpha)         / a0;
}

void biquad_calc_lpf(Biquad *b, float fc, float q, float sr)
{
    float w0    = 2.0f * (float)M_PI * fc / sr;
    float alpha = sinf(w0) / (2.0f * q);
    float cos_w = cosf(w0);
    float a0    = 1.0f + alpha;

    b->b0 = (1.0f - cos_w) * 0.5f / a0;
    b->b1 = (1.0f - cos_w)         / a0;
    b->b2 = (1.0f - cos_w) * 0.5f / a0;
    b->a1 = (-2.0f * cos_w)        / a0;
    b->a2 = (1.0f - alpha)         / a0;
}

/* ── RT 처리 ─────────────────────────────────────────────────────── */

void biquad_process(Biquad *b, float *buf, int frames)
{
    if (!b->enabled) return;
    float b0 = b->b0, b1 = b->b1, b2 = b->b2;
    float a1 = b->a1, a2 = b->a2;
    float s1 = b->s1, s2 = b->s2;
    for (int i = 0; i < frames; i++) {
        float x  = buf[i];
        float y  = b0*x + s1;
        s1 = b1*x - a1*y + s2;
        s2 = b2*x - a2*y;
        buf[i] = y;
    }
    b->s1 = s1;
    b->s2 = s2;
}

void hpf_process(HpfState *h, float *buf, int frames)
{
    if (!h->enabled) return;
    for (int s = 0; s < h->n_sections; s++)
        biquad_process(&h->sections[s], buf, frames);
}

void eq_process(EqState *eq, float *buf, int frames)
{
    for (int i = 0; i < MAX_EQ_BANDS; i++)
        biquad_process(&eq->bands[i], buf, frames);
}
