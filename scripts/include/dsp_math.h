#pragma once
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

/* ── Biquad 필터 ──────────────────────────────────────────────────── */
typedef struct { float b0,b1,b2,a1,a2, x1,x2,y1,y2; } Biquad;
typedef struct { double b0,b1,b2,a1,a2; } BqCoeffs;

static inline void bq_reset(Biquad *bq) {
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0f;
}

static inline float bq_process(Biquad *bq, float x) {
    float y = bq->b0*x + bq->b1*bq->x1 + bq->b2*bq->x2
                       - bq->a1*bq->y1  - bq->a2*bq->y2;
    bq->x2 = bq->x1; bq->x1 = x;
    bq->y2 = bq->y1; bq->y1 = y;
    return y;
}

typedef enum { T_PEAK=0, T_LOSHELF, T_HISHELF, T_LP, T_HP } EqType;

/* 2차 Butterworth HPF 계수 계산 */
void calc_hpf(BqCoeffs *c, float freq, float sr);

/* 파라메트릭 EQ 계수 계산 (peak / shelf / LP / HP) */
void calc_eq(BqCoeffs *c, EqType type, float freq, float gain_db, float q, float sr);

/* ── 리미터 ───────────────────────────────────────────────────────── */
typedef struct {
    int   enabled;
    float threshold, attack_coef, release_coef, makeup;
    float env, gr;
} Limiter;

typedef struct { float threshold, attack_coef, release_coef, makeup; } LimCoeffs;

static inline void lim_reset(Limiter *l) { l->env = 0.0f; l->gr = 1.0f; }

static inline float lim_process(Limiter *l, float x) {
    float peak = fabsf(x);
    if (peak > l->env) l->env += l->attack_coef  * (peak - l->env);
    else               l->env += l->release_coef * (peak - l->env);
    float gr = (l->env > l->threshold && l->env > 0.0f)
               ? l->threshold / l->env : 1.0f;
    l->gr = gr;
    return x * gr * l->makeup;
}

/* 리미터 계수 계산 */
LimCoeffs calc_limiter(float thr_db, float atk_ms, float rel_ms, float mkup_db, float sr);
