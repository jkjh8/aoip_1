#define _GNU_SOURCE
#include <math.h>
#include <string.h>
#include "include/dsp_dynamics.h"

/* ── 게이트 (엔벨로프 팔로워 + 상태머신) ─────────────────────────── */
void gate_process(GateState *g, float *buf, int frames)
{
    if (!g->enabled) return;

    float thr     = g->threshold_lin;
    float atk     = g->attack_coef;
    float rel     = g->release_coef;
    float range   = g->range_lin;
    float env     = g->envelope;
    float gain    = g->gain;
    float gr_min  = 0.0f;  /* period 내 최소 게인 (GR 측정용) */

    for (int i = 0; i < frames; i++) {
        float abs_x = buf[i] < 0.0f ? -buf[i] : buf[i];

        /* 엔벨로프 팔로워 (어택/릴리즈 분리) */
        if (abs_x > env)
            env = atk * env + (1.0f - atk) * abs_x;
        else
            env = rel * env + (1.0f - rel) * abs_x;

        /* 상태머신 */
        switch (g->phase) {
        case GATE_CLOSED:
            if (env >= thr) { g->phase = GATE_ATTACK; }
            break;
        case GATE_ATTACK:
            gain += (1.0f - gain) * (1.0f - atk);
            if (gain >= 0.9999f) {
                gain = 1.0f;
                g->phase = GATE_OPEN;
                g->hold_count = g->hold_samples;
            }
            break;
        case GATE_OPEN:
            if (env < thr) {
                if (g->hold_count > 0) {
                    g->phase = GATE_HOLD;
                } else {
                    g->phase = GATE_RELEASE;
                }
            }
            break;
        case GATE_HOLD:
            if (env >= thr) { g->phase = GATE_OPEN; break; }
            if (--g->hold_count <= 0) g->phase = GATE_RELEASE;
            break;
        case GATE_RELEASE:
            /* RELEASE 중 신호 복귀: gain이 중간값이므로 OPEN 직행 금지.
             * ATTACK으로 보내 1.0까지 정상 복귀시켜야 게인 고정 버그 방지. */
            if (env >= thr) { g->phase = GATE_ATTACK; break; }
            gain = rel * gain + (1.0f - rel) * range;
            if (gain <= range * 1.001f) {
                gain = range;
                g->phase = GATE_CLOSED;
            }
            break;
        }

        buf[i] *= gain;
        if (gain < (1.0f - gr_min)) gr_min = 1.0f - gain;
    }

    g->envelope = env;
    g->gain     = gain;
    /* gr_cur: 0dB = 열림, 음수 = 감쇠 */
    g->gr_cur = (gain > 1e-6f) ? 20.0f * log10f(gain) : -120.0f;
}

/* ── 컴프레서 (소프트니 게인 계산, 로그 도메인 평활화) ──────────── */
void comp_process(CompState *c, float *buf, int frames)
{
    if (!c->enabled) return;

    float thr    = c->threshold_db;
    float inv_r  = 1.0f / c->ratio - 1.0f;  /* (1/ratio - 1): 게인 감소량 배율 */
    float knee   = c->knee_db;
    float knee2  = knee * 0.5f;
    float atk    = c->attack_coef;
    float rel    = c->release_coef;
    float mkup   = c->makeup_lin;
    float env_db = c->env_db;
    float gr_min = 0.0f;

    for (int i = 0; i < frames; i++) {
        float abs_x = buf[i] < 0.0f ? -buf[i] : buf[i];
        float x_db  = (abs_x > 1e-7f) ? 20.0f * log10f(abs_x) : -140.0f;

        /* 소프트니 게인 감소 (dB 도메인) */
        float over = x_db - thr;
        float gr_db;
        if (knee > 0.0f && fabsf(over) <= knee2)
            gr_db = inv_r * (over + knee2) * (over + knee2) / (2.0f * knee);
        else if (over > knee2)
            gr_db = inv_r * over;
        else
            gr_db = 0.0f;

        /* 어택/릴리즈 평활화 */
        if (gr_db < env_db)
            env_db = atk * env_db + (1.0f - atk) * gr_db;
        else
            env_db = rel * env_db + (1.0f - rel) * gr_db;

        buf[i] *= powf(10.0f, env_db / 20.0f) * mkup;

        if (env_db < gr_min) gr_min = env_db;
    }

    c->env_db = env_db;
    c->gr_cur = gr_min;   /* dB (≤0) */
}

/* ── 리미터 (샘플 정확, lookahead 없음, 레이턴시 0) ─────────────── */
void lim_process(LimState *l, float *buf, int frames)
{
    if (!l->enabled) return;

    float thr  = l->threshold_lin;
    float rel  = l->release_coef;
    float gain = l->gain;
    float gr_min = 0.0f;

    for (int i = 0; i < frames; i++) {
        float abs_x = buf[i] < 0.0f ? -buf[i] : buf[i];

        /* 즉각 어택: 한 샘플 내에서 게인 감소 */
        if (abs_x * gain > thr)
            gain = (abs_x > 1e-9f) ? thr / abs_x : gain;

        buf[i] *= gain;

        /* 릴리즈: 1.0을 향해 서서히 복귀 */
        gain = rel * gain + (1.0f - rel) * 1.0f;
        if (gain > 1.0f) gain = 1.0f;

        if (gain < 1.0f && (1.0f - gain) > gr_min) gr_min = 1.0f - gain;
    }

    l->gain   = gain;
    l->gr_cur = (gain > 1e-6f && gain < 1.0f) ? 20.0f * log10f(gain) : 0.0f;
}
