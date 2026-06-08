#define _GNU_SOURCE
#include <string.h>
#include <math.h>
#include "include/dsp_channel.h"
#include "include/dsp_neon.h"
#include "include/dsp_eq.h"
#include "include/dsp_dynamics.h"

/* ── 입력 채널 DSP — 게이트 전 단계 (trim + HPF + EQ) ───────────── */
/* 호출 후 buf는 게이트 입력 상태. 호출자가 여기서 피크 측정 후 gate_on 호출. */
void in_ch_dsp_pre_gate(InChDspState *ch, float *buf, int frames)
{
    if (ch->trim_lin != 1.0f)
        gain_apply_neon(buf, ch->trim_lin, frames);
    hpf_process(&ch->hpf, buf, frames);
    eq_process(&ch->eq, buf, frames);
}

/* ── 입력 채널 DSP — 게이트 이후 단계 (gate + comp) ─────────────── */
void in_ch_dsp_gate_on(InChDspState *ch, float *buf, int frames)
{
    gate_process(&ch->gate, buf, frames);
    comp_process(&ch->comp, buf, frames);
}

/* ── 출력 채널 DSP 체인 ──────────────────────────────────────────── */
void out_ch_dsp(OutChDspState *ch, float *buf, int frames)
{
    gate_process(&ch->gate, buf, frames);
    eq_process(&ch->eq, buf, frames);
    comp_process(&ch->comp, buf, frames);
    lim_process(&ch->lim, buf, frames);
}

/* ── 파라미터 적용 함수 (apply_cmd에서 호출, RT 스레드) ─────────── */

void in_ch_apply_trim(InChDspState *ch, float trim_db)
{
    ch->trim_lin = trim_db_to_linear(trim_db);
}

void in_ch_apply_hpf(InChDspState *ch, const HpfCmd *p)
{
    ch->hpf.enabled    = p->enabled;
    ch->hpf.n_sections = p->n_sections;
    for (int s = 0; s < p->n_sections && s < 4; s++) {
        Biquad *b = &ch->hpf.sections[s];
        b->b0 = p->coef[s][0]; b->b1 = p->coef[s][1]; b->b2 = p->coef[s][2];
        b->a1 = p->coef[s][3]; b->a2 = p->coef[s][4];
        b->s1 = 0.0f; b->s2 = 0.0f;  /* 계수 갱신 시 상태 초기화 */
        b->enabled = p->enabled;
    }
}

void in_ch_apply_eq(InChDspState *ch, const EqBandCmd *p)
{
    if (p->band < 0 || p->band >= MAX_EQ_BANDS) return;
    Biquad *b = &ch->eq.bands[p->band];
    b->b0 = p->b0; b->b1 = p->b1; b->b2 = p->b2;
    b->a1 = p->a1; b->a2 = p->a2;
    b->s1 = 0.0f;  b->s2 = 0.0f;
    b->enabled = p->enabled;
}

void in_ch_apply_gate(InChDspState *ch, const GateCmd *p)
{
    ch->gate.threshold_lin = p->threshold_lin;
    ch->gate.attack_coef   = p->attack_coef;
    ch->gate.release_coef  = p->release_coef;
    ch->gate.hold_samples  = p->hold_samples;
    ch->gate.range_lin     = p->range_lin;
    ch->gate.enabled       = p->enabled;
    if (!p->enabled) {
        ch->gate.phase    = GATE_CLOSED;
        ch->gate.envelope = 0.0f;
        ch->gate.gain     = 1.0f;
        ch->gate.gr_cur   = 0.0f;
    } else if (ch->gate.phase == GATE_CLOSED) {
        /* CLOSED 중 range 변경 시 gain 즉시 반영 */
        ch->gate.gain = p->range_lin;
    }
}

void in_ch_apply_comp(InChDspState *ch, const CompCmd *p)
{
    ch->comp.threshold_db = p->threshold_db;
    ch->comp.ratio        = p->ratio;
    ch->comp.knee_db      = p->knee_db;
    ch->comp.attack_coef  = p->attack_coef;
    ch->comp.release_coef = p->release_coef;
    ch->comp.makeup_lin   = p->makeup_lin;
    ch->comp.enabled      = p->enabled;
    if (!p->enabled) { ch->comp.env_db = 0.0f; ch->comp.gr_cur = 0.0f; }
}

void out_ch_apply_gate(OutChDspState *ch, const GateCmd *p)
{
    ch->gate.threshold_lin = p->threshold_lin;
    ch->gate.attack_coef   = p->attack_coef;
    ch->gate.release_coef  = p->release_coef;
    ch->gate.hold_samples  = p->hold_samples;
    ch->gate.range_lin     = p->range_lin;
    ch->gate.enabled       = p->enabled;
    if (!p->enabled) {
        ch->gate.phase    = GATE_CLOSED;
        ch->gate.envelope = 0.0f;
        ch->gate.gain     = 1.0f;
        ch->gate.gr_cur   = 0.0f;
    } else if (ch->gate.phase == GATE_CLOSED) {
        ch->gate.gain = p->range_lin;
    }
}

void out_ch_apply_eq(OutChDspState *ch, const EqBandCmd *p)
{
    if (p->band < 0 || p->band >= MAX_EQ_BANDS) return;
    Biquad *b = &ch->eq.bands[p->band];
    b->b0 = p->b0; b->b1 = p->b1; b->b2 = p->b2;
    b->a1 = p->a1; b->a2 = p->a2;
    b->s1 = 0.0f;  b->s2 = 0.0f;
    b->enabled = p->enabled;
}

void out_ch_apply_comp(OutChDspState *ch, const CompCmd *p)
{
    ch->comp.threshold_db = p->threshold_db;
    ch->comp.ratio        = p->ratio;
    ch->comp.knee_db      = p->knee_db;
    ch->comp.attack_coef  = p->attack_coef;
    ch->comp.release_coef = p->release_coef;
    ch->comp.makeup_lin   = p->makeup_lin;
    ch->comp.enabled      = p->enabled;
    if (!p->enabled) { ch->comp.env_db = 0.0f; ch->comp.gr_cur = 0.0f; }
}

void out_ch_apply_lim(OutChDspState *ch, const LimCmd *p)
{
    ch->lim.threshold_lin = p->threshold_lin;
    ch->lim.release_coef  = p->release_coef;
    ch->lim.enabled       = p->enabled;
    if (!p->enabled) { ch->lim.gain = 1.0f; ch->lim.gr_cur = 0.0f; }
}
