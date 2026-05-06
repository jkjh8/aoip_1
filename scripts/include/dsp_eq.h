#pragma once
#include <stdint.h>
#include "engine_constants.h"

typedef enum {
    BIQUAD_PEAK = 0,
    BIQUAD_LOW_SHELF,
    BIQUAD_HIGH_SHELF,
    BIQUAD_NOTCH,
    BIQUAD_HIGH_PASS,
    BIQUAD_LOW_PASS,
} BiquadType;

/* Transposed Direct Form II biquad — 상태는 RT 스레드 전용 */
typedef struct {
    float b0, b1, b2, a1, a2;
    float s1, s2;           /* state registers */
    int   enabled;
} Biquad;

/* HPF 캐스케이드: 12=1섹션(Q=0.707), 24=2섹션, 48=4섹션 */
typedef struct {
    Biquad sections[4];
    int    n_sections;
    int    enabled;
} HpfState;

/* 4밴드 파라메트릭 EQ (밴드별 독립 enabled) */
typedef struct {
    Biquad bands[MAX_EQ_BANDS];
} EqState;

/* ── 계수 계산 (non-RT, cmd_loop 또는 JS에서 pre-compute 후 전달) ── */
void biquad_calc_peak      (Biquad *b, float fc, float q, float gain_db, float sr);
void biquad_calc_low_shelf (Biquad *b, float fc, float q, float gain_db, float sr);
void biquad_calc_high_shelf(Biquad *b, float fc, float q, float gain_db, float sr);
void biquad_calc_notch     (Biquad *b, float fc, float q, float sr);
void biquad_calc_hpf       (Biquad *b, float fc, float q, float sr);
void biquad_calc_lpf       (Biquad *b, float fc, float q, float sr);

/* ── RT 처리 함수 ─────────────────────────────────────────────────── */
void biquad_process(Biquad   *b,  float *buf, int frames);
void hpf_process   (HpfState *h,  float *buf, int frames);
void eq_process    (EqState  *eq, float *buf, int frames);

/* ── 유틸 ─────────────────────────────────────────────────────────── */
float trim_db_to_linear(float db);
