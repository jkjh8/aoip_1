/*
 * dsp_math.c — Biquad EQ / HPF / Limiter 계수 계산
 *
 * calc_hpf:     2차 Butterworth 고역통과 필터
 * calc_eq:      파라메트릭 EQ (peak / shelf / LP / HP)
 * calc_limiter: 피크 리미터 계수
 */
#include "include/dsp_math.h"
#include <math.h>

void calc_hpf(BqCoeffs *c, float freq, float sr)
{
    double w0 = 2.0*M_PI*freq/sr, cw = cos(w0), sw = sin(w0);
    double alpha = sw / (2.0 * 0.7071), a0 = 1.0 + alpha;
    c->b0 =  (1.0+cw)/2.0/a0;
    c->b1 = -(1.0+cw)/a0;
    c->b2 =  (1.0+cw)/2.0/a0;
    c->a1 = -2.0*cw/a0;
    c->a2 =  (1.0-alpha)/a0;
}

void calc_eq(BqCoeffs *c, EqType type, float freq, float gain_db, float q, float sr)
{
    double w0 = 2.0*M_PI*freq/sr, cw = cos(w0), sw = sin(w0);
    double A = pow(10.0, gain_db/40.0), alpha = sw/(2.0*q), sqA, a0;

    switch (type) {
    case T_PEAK:
        a0    = 1.0 + alpha/A;
        c->b0 = (1.0+alpha*A)/a0;
        c->b1 = c->a1 = -2.0*cw/a0;
        c->b2 = (1.0-alpha*A)/a0;
        c->a2 = (1.0-alpha/A)/a0;
        break;
    case T_LOSHELF:
        sqA   = sqrt(A);
        alpha = sw/2.0 * sqrt((A+1.0/A)*(1.0/q-1.0)+2.0);
        a0    = (A+1)+(A-1)*cw + 2.0*sqA*alpha;
        c->b0 =  A*((A+1)-(A-1)*cw + 2.0*sqA*alpha)/a0;
        c->b1 = 2*A*((A-1)-(A+1)*cw)/a0;
        c->b2 =  A*((A+1)-(A-1)*cw - 2.0*sqA*alpha)/a0;
        c->a1 = -2*((A-1)+(A+1)*cw)/a0;
        c->a2 =    ((A+1)+(A-1)*cw - 2.0*sqA*alpha)/a0;
        break;
    case T_HISHELF:
        sqA   = sqrt(A);
        alpha = sw/2.0 * sqrt((A+1.0/A)*(1.0/q-1.0)+2.0);
        a0    = (A+1)-(A-1)*cw + 2.0*sqA*alpha;
        c->b0 =  A*((A+1)+(A-1)*cw + 2.0*sqA*alpha)/a0;
        c->b1 =-2*A*((A-1)+(A+1)*cw)/a0;
        c->b2 =  A*((A+1)+(A-1)*cw - 2.0*sqA*alpha)/a0;
        c->a1 =  2*((A-1)-(A+1)*cw)/a0;
        c->a2 =    ((A+1)-(A-1)*cw - 2.0*sqA*alpha)/a0;
        break;
    case T_LP:
        a0    = 1.0+alpha;
        c->b0 = (1.0-cw)/2.0/a0;
        c->b1 = (1.0-cw)/a0;
        c->b2 = (1.0-cw)/2.0/a0;
        c->a1 = -2.0*cw/a0;
        c->a2 = (1.0-alpha)/a0;
        break;
    case T_HP:
        a0    = 1.0+alpha;
        c->b0 =  (1.0+cw)/2.0/a0;
        c->b1 = -(1.0+cw)/a0;
        c->b2 =  (1.0+cw)/2.0/a0;
        c->a1 = -2.0*cw/a0;
        c->a2 = (1.0-alpha)/a0;
        break;
    }
}

LimCoeffs calc_limiter(float thr_db, float atk_ms, float rel_ms, float mkup_db, float sr)
{
    LimCoeffs c;
    c.threshold    = powf(10.0f, thr_db/20.0f);
    c.attack_coef  = 1.0f - expf(-1.0f / fmaxf(1.0f, atk_ms*sr/1000.0f));
    c.release_coef = 1.0f - expf(-1.0f / fmaxf(1.0f, rel_ms*sr/1000.0f));
    c.makeup       = powf(10.0f, mkup_db/20.0f);
    return c;
}
