/*
 * dsp_engine.c — JACK DSP engine (gain, mute, HPF, parametric EQ, limiter)
 *
 * JACK client name: "gainer" (compatible with existing port references)
 *
 * Port layout:
 *   Input  channels: gainer:in_1..N  →  gainer:out_1..N   (HPF + 4-band PEQ + gain)
 *   Output channels: gainer:sin_1..M →  gainer:sout_1..M  (4-band PEQ + limiter + gain)
 *
 * Signal chain:
 *   Input:  src → HPF → EQ → level meter → gain ramp → dst
 *   Output: src → EQ  → limiter → level meter → gain ramp → dst
 *
 * Commands via stdin (all 1-based channel indices):
 *   gain     in|out  <ch>  <linear 0.0–2.0>
 *   mute     in|out  <ch>  <0|1>
 *   hpf      in      <ch>  enable    <0|1>
 *   hpf      in      <ch>  freq      <hz>
 *   eq       in|out  <ch>  <band>    enable  <0|1>
 *   eq       in|out  <ch>  <band>    coeffs  <b0> <b1> <b2> <a1> <a2>
 *   eq       in|out  <ch>  <band>    freq    <hz>
 *   eq       in|out  <ch>  <band>    gain    <db>
 *   eq       in|out  <ch>  <band>    q       <q>
 *   eq       in|out  <ch>  <band>    type    <peak|loshelf|hishelf|lp|hp>
 *   limiter  out     <ch>  enable    <0|1>
 *   limiter  out     <ch>  threshold <db>
 *   limiter  out     <ch>  attack    <ms>
 *   limiter  out     <ch>  release   <ms>
 *   limiter  out     <ch>  makeup    <db>
 *
 * Stdout output (for Node.js):
 *   lvl in  <ch> <db>                — input level (post-EQ, pre-gain), ~8 Hz
 *   lvl out <ch> <db>                — output level (post-limiter, pre-gain), ~8 Hz
 *   lm  out <ch> <pre_db> <post_db>  — limiter I/O level, ~8 Hz when enabled
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <signal.h>
#include <stdatomic.h>
#include <pthread.h>
#include <unistd.h>
#include <jack/jack.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

#define MAX_CHANNELS        32
#define MAX_EQ_BANDS        4
#define RING_SIZE           128   /* must be power of 2 */
#define GAIN_MAX            2.0f

/* ── Biquad filter ──────────────────────────────────────── */

typedef struct {
    double b0, b1, b2, a1, a2;
    float  x1, x2, y1, y2;
} Biquad;

static inline void bq_reset(Biquad *bq) {
    bq->x1 = bq->x2 = bq->y1 = bq->y2 = 0.0f;
}

static inline float bq_process(Biquad *bq, float x) {
    float y = (float)(bq->b0*x + bq->b1*bq->x1 + bq->b2*bq->x2
                               - bq->a1*bq->y1  - bq->a2*bq->y2);
    bq->x2 = bq->x1; bq->x1 = x;
    bq->y2 = bq->y1; bq->y1 = y;
    return y;
}

typedef struct { double b0, b1, b2, a1, a2; } BqCoeffs;

/* 2nd-order Butterworth HPF */
static void calc_hpf(BqCoeffs *c, float freq, float sr) {
    double w0    = 2.0 * M_PI * freq / sr;
    double cosw0 = cos(w0), sinw0 = sin(w0);
    double alpha = sinw0 / (2.0 * 0.7071);   /* Q = 1/sqrt(2) */
    double a0    = 1.0 + alpha;
    c->b0 =  (1.0 + cosw0) / 2.0 / a0;
    c->b1 = -(1.0 + cosw0)       / a0;
    c->b2 =  (1.0 + cosw0) / 2.0 / a0;
    c->a1 = -2.0 * cosw0         / a0;
    c->a2 =  (1.0 - alpha)       / a0;
}

typedef enum { T_PEAK=0, T_LOSHELF, T_HISHELF, T_LP, T_HP } EqType;

/* RBJ cookbook biquad EQ */
static void calc_eq(BqCoeffs *c, EqType type, float freq, float gain_db,
                    float q, float sr) {
    double w0    = 2.0 * M_PI * freq / sr;
    double cosw0 = cos(w0), sinw0 = sin(w0);
    double A     = pow(10.0, gain_db / 40.0);
    double alpha = sinw0 / (2.0 * q);
    double sqA, a0;

    switch (type) {
    case T_PEAK:
        a0    = 1.0 + alpha / A;
        c->b0 = (1.0 + alpha * A) / a0;
        c->b1 = c->a1 = -2.0 * cosw0 / a0;
        c->b2 = (1.0 - alpha * A) / a0;
        c->a2 = (1.0 - alpha / A) / a0;
        break;
    case T_LOSHELF:
        sqA   = sqrt(A);
        alpha = sinw0 / 2.0 * sqrt((A + 1.0/A) * (1.0/q - 1.0) + 2.0);
        a0    = (A+1) + (A-1)*cosw0 + 2.0*sqA*alpha;
        c->b0 =  A * ((A+1) - (A-1)*cosw0 + 2.0*sqA*alpha) / a0;
        c->b1 = 2*A * ((A-1) - (A+1)*cosw0)                / a0;
        c->b2 =  A * ((A+1) - (A-1)*cosw0 - 2.0*sqA*alpha) / a0;
        c->a1 = -2  * ((A-1) + (A+1)*cosw0)                / a0;
        c->a2 =       ((A+1) + (A-1)*cosw0 - 2.0*sqA*alpha)/ a0;
        break;
    case T_HISHELF:
        sqA   = sqrt(A);
        alpha = sinw0 / 2.0 * sqrt((A + 1.0/A) * (1.0/q - 1.0) + 2.0);
        a0    = (A+1) - (A-1)*cosw0 + 2.0*sqA*alpha;
        c->b0 =  A * ((A+1) + (A-1)*cosw0 + 2.0*sqA*alpha) / a0;
        c->b1 =-2*A * ((A-1) + (A+1)*cosw0)                / a0;
        c->b2 =  A * ((A+1) + (A-1)*cosw0 - 2.0*sqA*alpha) / a0;
        c->a1 =  2  * ((A-1) - (A+1)*cosw0)                / a0;
        c->a2 =       ((A+1) - (A-1)*cosw0 - 2.0*sqA*alpha)/ a0;
        break;
    case T_LP:
        a0    = 1.0 + alpha;
        c->b0 = (1.0 - cosw0) / 2.0 / a0;
        c->b1 = (1.0 - cosw0)       / a0;
        c->b2 = (1.0 - cosw0) / 2.0 / a0;
        c->a1 = -2.0 * cosw0        / a0;
        c->a2 = (1.0 - alpha)       / a0;
        break;
    case T_HP:
        a0    = 1.0 + alpha;
        c->b0 =  (1.0 + cosw0) / 2.0 / a0;
        c->b1 = -(1.0 + cosw0)       / a0;
        c->b2 =  (1.0 + cosw0) / 2.0 / a0;
        c->a1 = -2.0 * cosw0         / a0;
        c->a2 =  (1.0 - alpha)       / a0;
        break;
    }
}

/* ── Limiter ────────────────────────────────────────────── */

typedef struct {
    int   enabled;
    float threshold;    /* linear amplitude ceiling */
    float attack_coef;  /* 1 - exp(-1 / attack_samples) */
    float release_coef; /* 1 - exp(-1 / release_samples) */
    float makeup;       /* linear makeup gain */
    float env;          /* envelope follower state */
    float gr;           /* current gain reduction ratio (≤1.0) */
} Limiter;

static inline void lim_reset(Limiter *lim) {
    lim->env = 0.0f; lim->gr = 1.0f;
}

/* Returns the gain-reduced + makeup signal */
static inline float lim_process(Limiter *lim, float x) {
    float peak = fabsf(x);
    if (peak > lim->env)
        lim->env += lim->attack_coef  * (peak - lim->env);
    else
        lim->env += lim->release_coef * (peak - lim->env);

    float gr = (lim->env > lim->threshold && lim->env > 0.0f)
               ? lim->threshold / lim->env : 1.0f;
    lim->gr = gr;
    return x * gr * lim->makeup;
}

typedef struct { float threshold, attack_coef, release_coef, makeup; } LimCoeffs;

static LimCoeffs calc_limiter(float threshold_db, float attack_ms,
                               float release_ms, float makeup_db, float sr) {
    LimCoeffs c;
    c.threshold    = powf(10.0f, threshold_db / 20.0f);
    c.attack_coef  = 1.0f - expf(-1.0f / fmaxf(1.0f, attack_ms  * sr / 1000.0f));
    c.release_coef = 1.0f - expf(-1.0f / fmaxf(1.0f, release_ms * sr / 1000.0f));
    c.makeup       = powf(10.0f, makeup_db / 20.0f);
    return c;
}

/* ── SPSC lock-free command ring buffer ──────────────────── */

typedef enum {
    CMD_GAIN,
    CMD_MUTE,
    CMD_BYPASS,
    CMD_HPF_ENABLE,
    CMD_HPF_COEFFS,
    CMD_HPF_STAGES,
    CMD_EQ_ENABLE,
    CMD_EQ_COEFFS,
    CMD_LIMITER_ENABLE,
    CMD_LIMITER_PARAMS,
} CmdType;

typedef struct {
    CmdType  type;
    int      dir;    /* 0 = input, 1 = output */
    int      ch;     /* 0-based */
    int      band;   /* EQ band 0-3 */
    union {
        float     gain;
        int       flag;
        BqCoeffs  coeffs;
        LimCoeffs lim_coeffs;
    };
} Cmd;

typedef struct {
    Cmd             buf[RING_SIZE];
    _Atomic size_t  wr;
    _Atomic size_t  rd;
} CmdRing;

static CmdRing g_ring;

/* Called from command thread (single producer) */
static void ring_push(const Cmd *cmd) {
    size_t wr = atomic_load_explicit(&g_ring.wr, memory_order_relaxed);
    while (wr - atomic_load_explicit(&g_ring.rd, memory_order_acquire) >= RING_SIZE)
        ;
    g_ring.buf[wr & (RING_SIZE - 1)] = *cmd;
    atomic_store_explicit(&g_ring.wr, wr + 1, memory_order_release);
}

/* Called from RT audio callback (single consumer) */
static int ring_pop(Cmd *cmd) {
    size_t rd = atomic_load_explicit(&g_ring.rd, memory_order_relaxed);
    if (rd == atomic_load_explicit(&g_ring.wr, memory_order_acquire)) return 0;
    *cmd = g_ring.buf[rd & (RING_SIZE - 1)];
    atomic_store_explicit(&g_ring.rd, rd + 1, memory_order_release);
    return 1;
}

/* ── Per-channel state ───────────────────────────────────── */

typedef struct {
    jack_port_t *in_port, *out_port;

    /* Gain — smoothed per-sample ramp */
    float gain_tgt, gain_cur;
    int   muted;

    /* When set, skip all DSP (HPF/EQ/limiter) — level meter + gain only */
    int bypass_dsp;

    /* HPF (input channels only) */
    int    hpf_enabled;
    int    hpf_stages;   /* 1 = 12dB/oct, 2 = 24dB/oct */
    Biquad hpf[2];

    /* Parametric EQ */
    int    eq_enabled[MAX_EQ_BANDS];
    Biquad eq[MAX_EQ_BANDS];

    /* Limiter (output channels only; enabled flag guards activation) */
    Limiter lim;

} Channel;

static Channel g_inputs[MAX_CHANNELS];
static Channel g_outputs[MAX_CHANNELS];
static int     g_n_in, g_n_out;
static float   g_sr;

/* Limiter meter values — written by RT thread (volatile float), read by reporter thread */
static volatile float g_lim_pre[MAX_CHANNELS];
static volatile float g_lim_post[MAX_CHANNELS];
/* Output channel peak levels (post-limiter, pre-gain) for level metering */
static volatile float g_out_level[MAX_CHANNELS];
/* Input channel peak levels (post-EQ, pre-gain) for level metering */
static volatile float g_in_level[MAX_CHANNELS];
static volatile int   g_reporter_running;

/* ── Apply a single command (RT thread) ──────────────────── */

static void apply_cmd(const Cmd *cmd) {
    Channel *ch = (cmd->dir == 0) ? &g_inputs[cmd->ch] : &g_outputs[cmd->ch];

    switch (cmd->type) {
    case CMD_GAIN:
        ch->gain_tgt = cmd->gain;
        break;
    case CMD_MUTE:
        ch->muted = cmd->flag;
        if (ch->muted) ch->gain_cur = ch->gain_tgt;
        break;
    case CMD_BYPASS:
        ch->bypass_dsp = cmd->flag;
        break;
    case CMD_HPF_ENABLE:
        ch->hpf_enabled = cmd->flag;
        break;
    case CMD_HPF_STAGES:
        ch->hpf_stages = cmd->flag;
        break;
    case CMD_HPF_COEFFS: {
        Biquad *bq = &ch->hpf[cmd->band];  /* band: 0 or 1 */
        bq->b0 = cmd->coeffs.b0; bq->b1 = cmd->coeffs.b1; bq->b2 = cmd->coeffs.b2;
        bq->a1 = cmd->coeffs.a1; bq->a2 = cmd->coeffs.a2;
        break;
    }
    case CMD_EQ_ENABLE:
        ch->eq_enabled[cmd->band] = cmd->flag;
        break;
    case CMD_EQ_COEFFS: {
        Biquad *bq = &ch->eq[cmd->band];
        bq->b0 = cmd->coeffs.b0; bq->b1 = cmd->coeffs.b1; bq->b2 = cmd->coeffs.b2;
        bq->a1 = cmd->coeffs.a1; bq->a2 = cmd->coeffs.a2;
        break;
    }
    case CMD_LIMITER_ENABLE:
        ch->lim.enabled = cmd->flag;
        if (!cmd->flag) lim_reset(&ch->lim);
        break;
    case CMD_LIMITER_PARAMS:
        ch->lim.threshold    = cmd->lim_coeffs.threshold;
        ch->lim.attack_coef  = cmd->lim_coeffs.attack_coef;
        ch->lim.release_coef = cmd->lim_coeffs.release_coef;
        ch->lim.makeup       = cmd->lim_coeffs.makeup;
        break;
    }
}

/* ── JACK process callback ───────────────────────────────── */

static void process_channel(Channel *ch, jack_nframes_t nframes, int is_input) {
    float *src = (float *)jack_port_get_buffer(ch->in_port,  nframes);
    float *dst = (float *)jack_port_get_buffer(ch->out_port, nframes);

    if (ch->muted) {
        memset(dst, 0, nframes * sizeof(float));
        ch->gain_cur = ch->gain_tgt;
        return;
    }

    float cur  = ch->gain_cur;
    float tgt  = ch->gain_tgt;
    float step = (tgt - cur) / (float)nframes;

    for (jack_nframes_t i = 0; i < nframes; i++) {
        float s = src[i];

        if (ch->bypass_dsp) {
            /* Bypass: level meter only, no DSP */
            float peak = fabsf(s);
            if (is_input) {
                int idx = (int)(ch - g_inputs);
                if (peak > g_in_level[idx]) g_in_level[idx] = peak;
            } else {
                int idx = (int)(ch - g_outputs);
                if (peak > g_out_level[idx]) g_out_level[idx] = peak;
            }
        } else {
            /* HPF */
            if (ch->hpf_enabled) {
                s = bq_process(&ch->hpf[0], s);
                if (ch->hpf_stages > 1)
                    s = bq_process(&ch->hpf[1], s);
            }

            /* Parametric EQ */
            for (int b = 0; b < MAX_EQ_BANDS; b++)
                if (ch->eq_enabled[b])
                    s = bq_process(&ch->eq[b], s);

            /* Limiter (output only): track pre level, apply, track post level */
            if (!is_input) {
                int idx = (int)(ch - g_outputs);
                float peak = fabsf(s);
                if (peak > g_lim_pre[idx]) g_lim_pre[idx] = peak;
                if (ch->lim.enabled)
                    s = lim_process(&ch->lim, s);
                peak = fabsf(s);
                if (peak > g_lim_post[idx]) g_lim_post[idx] = peak;
                if (peak > g_out_level[idx]) g_out_level[idx] = peak;
            }

            /* Input level tracking (post-EQ, pre-gain) */
            if (is_input) {
                int idx = (int)(ch - g_inputs);
                float peak = fabsf(s);
                if (peak > g_in_level[idx]) g_in_level[idx] = peak;
            }
        }

        /* Gain ramp */
        cur += step;
        s   *= cur;

        dst[i] = s;
    }
    ch->gain_cur = tgt;
}

static int process_cb(jack_nframes_t nframes, void *arg) {
    (void)arg;
    Cmd cmd;
    while (ring_pop(&cmd)) apply_cmd(&cmd);

    for (int i = 0; i < g_n_in;  i++) process_channel(&g_inputs[i],  nframes, 1);
    for (int i = 0; i < g_n_out; i++) process_channel(&g_outputs[i], nframes, 0);

    return 0;
}

/* ── Reporter thread (~8 Hz) ─────────────────────────────── */

static void *reporter_thread(void *arg) {
    (void)arg;
    while (g_reporter_running) {
        usleep(125000);  /* 125 ms (~8 Hz) */

        /* Input channel levels (post-EQ, pre-gain) */
        for (int i = 0; i < g_n_in; i++) {
            float peak = g_in_level[i];
            g_in_level[i] = 0.0f;
            float db = peak > 1e-7f ? 20.0f * log10f(peak) : -120.0f;
            printf("lvl in %d %.1f\n", i + 1, db);
        }

        /* Output channel levels (post-limiter, pre-gain) */
        for (int i = 0; i < g_n_out; i++) {
            float peak = g_out_level[i];
            g_out_level[i] = 0.0f;
            float db = peak > 1e-7f ? 20.0f * log10f(peak) : -120.0f;
            printf("lvl out %d %.1f\n", i + 1, db);
        }

        /* Limiter meters (output channels with limiter enabled) */
        for (int i = 0; i < g_n_out; i++) {
            if (!g_outputs[i].lim.enabled) continue;
            float pre_p  = g_lim_pre[i];
            float post_p = g_lim_post[i];
            g_lim_pre[i]  = 0.0f;
            g_lim_post[i] = 0.0f;
            float pre_db  = pre_p  > 1e-7f ? 20.0f * log10f(pre_p)  : -120.0f;
            float post_db = post_p > 1e-7f ? 20.0f * log10f(post_p) : -120.0f;
            printf("lm out %d %.1f %.1f\n", i + 1, pre_db, post_db);
        }

        fflush(stdout);
    }
    return NULL;
}

/* ── Command parsing (command thread) ────────────────────── */

/* Per-channel raw parameters — used to recompute coefficients */
typedef struct {
    float   hpf_freq;
    int     hpf_slope;  /* 12 or 24 (dB/oct) */
    struct  { float freq, gain_db, q; EqType type; } eq[MAX_EQ_BANDS];
    struct  { float threshold_db, attack_ms, release_ms, makeup_db; } lim;
} ChState;

static ChState g_in_state[MAX_CHANNELS];
static ChState g_out_state[MAX_CHANNELS];

static EqType parse_eq_type(const char *s) {
    if (!strcmp(s, "loshelf")) return T_LOSHELF;
    if (!strcmp(s, "hishelf")) return T_HISHELF;
    if (!strcmp(s, "lp"))      return T_LP;
    if (!strcmp(s, "hp"))      return T_HP;
    return T_PEAK;
}

static void cmd_loop(void) {
    char line[256];
    while (fgets(line, sizeof(line), stdin)) {
        char *tok[12];
        int   n = 0;
        char *p = strtok(line, " \t\r\n");
        while (p && n < 12) { tok[n++] = p; p = strtok(NULL, " \t\r\n"); }
        if (n < 3) continue;

        const char *verb  = tok[0];
        int         dir   = strcmp(tok[1], "in") ? 1 : 0;
        int         ch    = atoi(tok[2]) - 1;  /* 1-based → 0-based */

        if (ch < 0 || (dir == 0 && ch >= g_n_in) || (dir == 1 && ch >= g_n_out))
            continue;

        ChState *cs  = (dir == 0) ? &g_in_state[ch] : &g_out_state[ch];
        Cmd      cmd = { .dir = dir, .ch = ch };

        /* gain in|out <ch> <linear> */
        if (!strcmp(verb, "gain") && n >= 4) {
            cmd.type = CMD_GAIN;
            cmd.gain = fmaxf(0.0f, fminf(GAIN_MAX, (float)atof(tok[3])));
            ring_push(&cmd);

        /* mute in|out <ch> <0|1> */
        } else if (!strcmp(verb, "mute") && n >= 4) {
            cmd.type = CMD_MUTE;
            cmd.flag = atoi(tok[3]);
            ring_push(&cmd);

        /* bypass in|out <ch> <0|1> */
        } else if (!strcmp(verb, "bypass") && n >= 4) {
            cmd.type = CMD_BYPASS;
            cmd.flag = atoi(tok[3]);
            ring_push(&cmd);

        /* hpf in <ch> enable|freq|slope <val> */
        } else if (!strcmp(verb, "hpf") && n >= 5) {
            const char *param = tok[3];
            if (!strcmp(param, "enable")) {
                cmd.type = CMD_HPF_ENABLE;
                cmd.flag = atoi(tok[4]);
                ring_push(&cmd);
            } else if (!strcmp(param, "freq")) {
                cs->hpf_freq = (float)atof(tok[4]);
                BqCoeffs c;
                calc_hpf(&c, cs->hpf_freq, g_sr);
                cmd.type = CMD_HPF_COEFFS; cmd.band = 0; cmd.coeffs = c;
                ring_push(&cmd);
                if (cs->hpf_slope >= 24) {
                    cmd.band = 1;
                    ring_push(&cmd);
                }
            } else if (!strcmp(param, "slope")) {
                cs->hpf_slope = atoi(tok[4]);
                int stages = (cs->hpf_slope >= 24) ? 2 : 1;
                cmd.type = CMD_HPF_STAGES; cmd.flag = stages;
                ring_push(&cmd);
                /* 기울기 변경 시 계수 재계산 */
                if (cs->hpf_freq > 0.0f) {
                    BqCoeffs c;
                    calc_hpf(&c, cs->hpf_freq, g_sr);
                    cmd.type = CMD_HPF_COEFFS; cmd.band = 0; cmd.coeffs = c;
                    ring_push(&cmd);
                    if (stages > 1) {
                        cmd.band = 1;
                        ring_push(&cmd);
                    }
                }
            }

        /* eq in|out <ch> <band> enable|freq|gain|q|type|coeffs <val...> */
        } else if (!strcmp(verb, "eq") && n >= 6) {
            int band = atoi(tok[3]);
            if (band < 0 || band >= MAX_EQ_BANDS) continue;
            cmd.band = band;
            const char *param = tok[4];

            if (!strcmp(param, "enable")) {
                cmd.type = CMD_EQ_ENABLE;
                cmd.flag = atoi(tok[5]);
                ring_push(&cmd);
            } else if (!strcmp(param, "coeffs") && n >= 10) {
                BqCoeffs c;
                c.b0 = atof(tok[5]);
                c.b1 = atof(tok[6]);
                c.b2 = atof(tok[7]);
                c.a1 = atof(tok[8]);
                c.a2 = atof(tok[9]);
                cmd.type   = CMD_EQ_COEFFS;
                cmd.coeffs = c;
                ring_push(&cmd);
            } else {
                if      (!strcmp(param, "freq"))  cs->eq[band].freq    = (float)atof(tok[5]);
                else if (!strcmp(param, "gain"))  cs->eq[band].gain_db = (float)atof(tok[5]);
                else if (!strcmp(param, "q"))     cs->eq[band].q       = fmaxf(0.1f, (float)atof(tok[5]));
                else if (!strcmp(param, "type"))  cs->eq[band].type    = parse_eq_type(tok[5]);
                else continue;

                BqCoeffs c;
                calc_eq(&c, cs->eq[band].type, cs->eq[band].freq,
                        cs->eq[band].gain_db, cs->eq[band].q, g_sr);
                cmd.type   = CMD_EQ_COEFFS;
                cmd.coeffs = c;
                ring_push(&cmd);
            }

        /* limiter out <ch> enable|threshold|attack|release|makeup <val> */
        } else if (!strcmp(verb, "limiter") && n >= 5) {
            if (dir != 1) continue;  /* limiter only on output channels */
            const char *param = tok[3];

            if (!strcmp(param, "enable")) {
                cmd.type = CMD_LIMITER_ENABLE;
                cmd.flag = atoi(tok[4]);
                ring_push(&cmd);
            } else {
                if      (!strcmp(param, "threshold")) cs->lim.threshold_db = (float)atof(tok[4]);
                else if (!strcmp(param, "attack"))    cs->lim.attack_ms    = fmaxf(0.1f, (float)atof(tok[4]));
                else if (!strcmp(param, "release"))   cs->lim.release_ms   = fmaxf(1.0f, (float)atof(tok[4]));
                else if (!strcmp(param, "makeup"))    cs->lim.makeup_db    = (float)atof(tok[4]);
                else continue;

                LimCoeffs lc = calc_limiter(cs->lim.threshold_db, cs->lim.attack_ms,
                                            cs->lim.release_ms,   cs->lim.makeup_db, g_sr);
                cmd.type       = CMD_LIMITER_PARAMS;
                cmd.lim_coeffs = lc;
                ring_push(&cmd);
            }
        }
    }
}

/* ── Main ───────────────────────────────────────────── */

static jack_client_t *g_client;

static void sig_handler(int s) {
    (void)s;
    fclose(stdin);
}

int main(int argc, char *argv[]) {
    if (argc < 3) {
        fprintf(stderr, "Usage: dsp_engine <n_in> <n_out>\n");
        return 1;
    }
    g_n_in  = atoi(argv[1]);
    g_n_out = atoi(argv[2]);
    if (g_n_in  < 0 || g_n_in  > MAX_CHANNELS ||
        g_n_out < 0 || g_n_out > MAX_CHANNELS) {
        fprintf(stderr, "Channel count out of range (max %d)\n", MAX_CHANNELS);
        return 1;
    }

    g_client = jack_client_open("gainer", JackNoStartServer, NULL);
    if (!g_client) {
        fprintf(stderr, "[dsp_engine] JACK connect failed\n");
        return 1;
    }
    g_sr = (float)jack_get_sample_rate(g_client);

    /* Default per-channel state */
    for (int i = 0; i < g_n_in; i++) {
        g_in_state[i].hpf_freq  = 80.0f;
        g_in_state[i].hpf_slope = 12;
        g_inputs[i].hpf_stages  = 1;
        for (int b = 0; b < MAX_EQ_BANDS; b++) {
            g_in_state[i].eq[b].freq    = 100.0f;
            g_in_state[i].eq[b].gain_db = 0.0f;
            g_in_state[i].eq[b].q       = 0.7f;
            g_in_state[i].eq[b].type    = T_PEAK;
        }
        g_inputs[i].gain_tgt = g_inputs[i].gain_cur = 1.0f;
        g_inputs[i].lim.gr   = 1.0f;
    }
    for (int i = 0; i < g_n_out; i++) {
        g_out_state[i].hpf_freq  = 80.0f;
        g_out_state[i].hpf_slope = 12;
        g_outputs[i].hpf_stages  = 1;
        for (int b = 0; b < MAX_EQ_BANDS; b++) {
            g_out_state[i].eq[b].freq    = 100.0f;
            g_out_state[i].eq[b].gain_db = 0.0f;
            g_out_state[i].eq[b].q       = 0.7f;
            g_out_state[i].eq[b].type    = T_PEAK;
        }
        /* Default limiter state: -6dBFS threshold, 5ms attack, 100ms release, 0dB makeup */
        g_out_state[i].lim.threshold_db = -6.0f;
        g_out_state[i].lim.attack_ms    = 5.0f;
        g_out_state[i].lim.release_ms   = 100.0f;
        g_out_state[i].lim.makeup_db    = 0.0f;
        LimCoeffs lc = calc_limiter(-6.0f, 5.0f, 100.0f, 0.0f, g_sr);
        g_outputs[i].lim.threshold    = lc.threshold;
        g_outputs[i].lim.attack_coef  = lc.attack_coef;
        g_outputs[i].lim.release_coef = lc.release_coef;
        g_outputs[i].lim.makeup       = lc.makeup;
        g_outputs[i].lim.gr           = 1.0f;
        g_outputs[i].gain_tgt = g_outputs[i].gain_cur = 1.0f;
    }

    /* Register JACK ports */
    for (int i = 0; i < g_n_in; i++) {
        char name[32];
        snprintf(name, sizeof(name), "in_%d",  i + 1);
        g_inputs[i].in_port  = jack_port_register(g_client, name,
                                    JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        snprintf(name, sizeof(name), "out_%d", i + 1);
        g_inputs[i].out_port = jack_port_register(g_client, name,
                                    JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }
    for (int i = 0; i < g_n_out; i++) {
        char name[32];
        snprintf(name, sizeof(name), "sin_%d",  i + 1);
        g_outputs[i].in_port  = jack_port_register(g_client, name,
                                    JACK_DEFAULT_AUDIO_TYPE, JackPortIsInput, 0);
        snprintf(name, sizeof(name), "sout_%d", i + 1);
        g_outputs[i].out_port = jack_port_register(g_client, name,
                                    JACK_DEFAULT_AUDIO_TYPE, JackPortIsOutput, 0);
    }

    jack_set_process_callback(g_client, process_cb, NULL);

    if (jack_activate(g_client)) {
        fprintf(stderr, "[dsp_engine] jack_activate failed\n");
        return 1;
    }

    signal(SIGTERM, sig_handler);
    signal(SIGINT,  sig_handler);

    pthread_t reporter_tid;
    g_reporter_running = 1;
    pthread_create(&reporter_tid, NULL, reporter_thread, NULL);

    fprintf(stderr, "[dsp_engine] ready (in=%d out=%d sr=%.0f)\n",
            g_n_in, g_n_out, g_sr);

    cmd_loop();   /* blocks until stdin closes */

    g_reporter_running = 0;
    pthread_join(reporter_tid, NULL);

    jack_deactivate(g_client);
    jack_client_close(g_client);
    return 0;
}
