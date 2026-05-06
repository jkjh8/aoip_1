#pragma once
#include "engine_constants.h"
#include "dsp_eq.h"
#include "dsp_dynamics.h"

/* ── 입력 채널 DSP 체인: fader → trim → HPF → EQ → Gate → Comp ──── */
typedef struct {
    /* fader (기존 Channel 호환) */
    float gain_tgt, gain_cur;
    int   muted, bypass_dsp;

    /* DSP 체인 */
    float    trim_lin;   /* ±20dB trim (1.0 = 0dB, bypass) */
    HpfState hpf;
    EqState  eq;
    GateState gate;
    CompState comp;
} InChDspState;

/* ── 출력 채널 DSP 체인: fader → Gate → EQ → Comp → Limiter ─────── */
typedef struct {
    float gain_tgt, gain_cur;
    int   muted, bypass_dsp;

    GateState gate;
    EqState   eq;
    CompState comp;
    LimState  lim;
} OutChDspState;

/* ── cmd ring 파라미터 페이로드 (Cmd union에 사용) ────────────────── */
typedef struct {
    float b0, b1, b2, a1, a2;
    int   band;      /* 0-based */
    int   enabled;
    int   type;      /* BiquadType */
} EqBandCmd;

typedef struct {
    int   n_sections;                /* 1=12dB, 2=24dB, 4=48dB */
    float coef[4][5];               /* [sec][b0,b1,b2,a1,a2] pre-computed */
    int   enabled;
} HpfCmd;

typedef struct {
    float threshold_lin;
    float attack_coef;
    float release_coef;
    int   hold_samples;
    float range_lin;
    int   enabled;
} GateCmd;

typedef struct {
    float threshold_db;
    float ratio;
    float knee_db;
    float attack_coef;
    float release_coef;
    float makeup_lin;
    int   enabled;
} CompCmd;

typedef struct {
    float threshold_lin;
    float release_coef;
    int   enabled;
} LimCmd;

/* ── DSP 체인 처리 (gain ramp 이후, bypass/mute 이전 확인은 호출자 책임) */
void in_ch_dsp (InChDspState  *ch, float *buf, int frames);
void out_ch_dsp(OutChDspState *ch, float *buf, int frames);

/* ── cmd ring → DSP 상태 적용 (RT 스레드, apply_cmd에서 호출) ────── */
void in_ch_apply_trim(InChDspState  *ch, float trim_db);
void in_ch_apply_hpf (InChDspState  *ch, const HpfCmd    *p);
void in_ch_apply_eq  (InChDspState  *ch, const EqBandCmd *p);
void in_ch_apply_gate(InChDspState  *ch, const GateCmd   *p);
void in_ch_apply_comp(InChDspState  *ch, const CompCmd   *p);

void out_ch_apply_gate(OutChDspState *ch, const GateCmd   *p);
void out_ch_apply_eq  (OutChDspState *ch, const EqBandCmd *p);
void out_ch_apply_comp(OutChDspState *ch, const CompCmd   *p);
void out_ch_apply_lim (OutChDspState *ch, const LimCmd    *p);
