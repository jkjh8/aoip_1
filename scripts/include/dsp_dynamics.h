#pragma once
#include "engine_constants.h"

/* ── 게이트 상태머신 ─────────────────────────────────────────────── */
typedef enum {
    GATE_CLOSED = 0, GATE_ATTACK, GATE_OPEN, GATE_HOLD, GATE_RELEASE,
} GatePhase;

typedef struct {
    /* 파라미터 (cmd ring으로 업데이트) */
    float threshold_lin;
    float attack_coef;   /* per-sample: expf(-1/(sr*ms/1000)) */
    float release_coef;
    int   hold_samples;
    float range_lin;     /* 최대 감쇠 선형값 (0=완전차단, 0.001=-60dB 등) */
    int   enabled;

    /* RT 상태 (RT 스레드 전용) */
    GatePhase phase;
    float     envelope;
    float     gain;
    int       hold_count;
    float     gr_cur;    /* 현재 GR dB (≤0), reporter 스레드가 읽음 */
} GateState;

/* ── 컴프레서 ─────────────────────────────────────────────────────── */
typedef struct {
    float threshold_db;
    float ratio;
    float knee_db;
    float attack_coef;
    float release_coef;
    float makeup_lin;
    int   enabled;

    /* RT 상태 */
    float env_db;   /* 로그 도메인 평활화된 게인 감소 */
    float gr_cur;
} CompState;

/* ── 리미터 (lookahead 없음, 레이턴시 0) ────────────────────────── */
typedef struct {
    float threshold_lin;
    float release_coef;
    int   enabled;

    /* RT 상태 */
    float gain;
    float gr_cur;
} LimState;

/* ── RT 처리 함수 ─────────────────────────────────────────────────── */
void gate_process(GateState *g, float *buf, int frames);
void comp_process(CompState *c, float *buf, int frames);
void lim_process (LimState  *l, float *buf, int frames);
