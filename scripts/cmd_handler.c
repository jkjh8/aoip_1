#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <sched.h>
#include <stdatomic.h>

#include "include/cmd_handler.h"
#include "include/engine_globals.h"
#include "include/engine_constants.h"
#include "include/dsp_eq.h"
#include "include/bridge_manager.h"
#include "include/clk2.h"
#include "include/rtp_utils.h"

/* ── SPSC 링버퍼 정의 ────────────────────────────────────────────── */
CmdRing g_cmd_ring;

void cmd_push(const Cmd *c)
{
    size_t wr = atomic_load_explicit(&g_cmd_ring.wr, memory_order_relaxed);
    while (wr - atomic_load_explicit(&g_cmd_ring.rd, memory_order_acquire) >= CMD_RING_SIZE)
        sched_yield();
    g_cmd_ring.buf[wr & (CMD_RING_SIZE-1)] = *c;
    atomic_store_explicit(&g_cmd_ring.wr, wr+1, memory_order_release);
}

int cmd_pop(Cmd *c)
{
    size_t rd = atomic_load_explicit(&g_cmd_ring.rd, memory_order_relaxed);
    if (rd == atomic_load_explicit(&g_cmd_ring.wr, memory_order_acquire)) return 0;
    *c = g_cmd_ring.buf[rd & (CMD_RING_SIZE-1)];
    atomic_store_explicit(&g_cmd_ring.rd, rd+1, memory_order_release);
    return 1;
}

/* ── ms → per-sample coef 변환 (non-RT, cmd_loop에서만 호출) ─────── */
static float ms_to_coef(float ms, float sr)
{
    if (ms <= 0.0f) return 0.0f;
    return expf(-1.0f / (sr * ms / 1000.0f));
}

/* ── 버터워스 HPF Q 값 ───────────────────────────────────────────── */
/* 2nd(12dB): Q=0.707 / 4th(24dB): Q1=0.5412, Q2=1.3066
 * 8th(48dB): Q1=0.5089, Q2=0.6013, Q3=0.8999, Q4=2.5628 */
static const float BUTTER_Q_12[] = { 0.7071f };
static const float BUTTER_Q_24[] = { 0.5412f, 1.3066f };
static const float BUTTER_Q_48[] = { 0.5089f, 0.6013f, 0.8999f, 2.5628f };

/* ── 명령 핸들러 ─────────────────────────────────────────────────── */

static void cmd_set(int n, char **tok)
{
    if (n < 3 || strcmp(tok[1], "period")) return;
    int pf = atoi(tok[2]);
    if (pf >= 1 && pf <= MAX_PERIOD_FRAMES) {
        g_period_frames = pf;
        fprintf(stderr, "[aoip_engine] period_frames=%d\n", g_period_frames);
    } else {
        fprintf(stderr, "[aoip_engine] set period: invalid %d (1..%d)\n",
                pf, MAX_PERIOD_FRAMES);
    }
}

static void cmd_bridge(int n, char **tok)
{
    if (n < 3) return;
    const char *sub  = tok[1];
    const char *name = tok[2];

    if ((!strcmp(sub, "add") || !strcmp(sub, "add_in") || !strcmp(sub, "add_out")) && n >= 8) {
        Device *d = NULL;
        for (int i = 0; i < g_n_dev; i++)
            if (!strcmp(g_dev[i].name, name)) { d = &g_dev[i]; break; }
        if (d) {
            bridge_stop(d);
        } else {
            if (g_n_dev >= MAX_DEVICES) return;
            d = &g_dev[g_n_dev++];
        }
        snprintf(d->name, sizeof(d->name), "%s", name);
        snprintf(d->dev,  sizeof(d->dev),  "%s", tok[3]);
        d->rate     = atoi(tok[4]);
        d->period   = atoi(tok[5]);
        d->nperiods = atoi(tok[6]);
        d->channels = atoi(tok[7]);
        d->ch_start = n >= 9 ? atoi(tok[8]) : (d - g_dev) * 2;
        d->mode     = !strcmp(sub, "add_in")  ? 1 :
                      !strcmp(sub, "add_out") ? 2 : 0;
        d->is_ravenna      = strstr(d->dev, "RAVENNA") ? 1 : 0;
        d->thread_priority = d->is_ravenna ? g_prio_ravenna : g_prio_alsa;
        d->is_i2s = (!strcmp(name, "analog") && d->mode != 2) ? 1 : 0;
        d->clk_accum       = 0;
        bridge_start(d);
    } else if (!strcmp(sub, "start")) {
        for (int i = 0; i < g_n_dev; i++)
            if (!strcmp(g_dev[i].name, name)) { bridge_start(&g_dev[i]); break; }
    } else if (!strcmp(sub, "stop")) {
        for (int i = 0; i < g_n_dev; i++)
            if (!strcmp(g_dev[i].name, name)) { bridge_stop(&g_dev[i]); break; }
    }
}

static void cmd_route(int n, char **tok)
{
    if (n < 4) return;
    int in_ch  = atoi(tok[2]) - 1;
    int out_ch = atoi(tok[3]) - 1;
    if (in_ch < 0 || in_ch >= MAX_CH || out_ch < 0 || out_ch >= MAX_CH) return;
    Cmd cmd = { .type = CMD_ROUTE_SET };
    cmd.route.in_ch  = in_ch;
    cmd.route.out_ch = out_ch;
    cmd.route.level  = (!strcmp(tok[1], "remove")) ? 0.0f :
                       (n >= 5 ? (float)atof(tok[4]) : 1.0f);
    cmd_push(&cmd);
    printf("route:updated\n");
    fflush(stdout);
}

static void cmd_rtp_in(int n, char **tok)
{
    if (n < 3) return;
    const char *sub  = tok[1];
    const char *name = tok[2];
    if (!strcmp(sub, "add")) {
        int slot = -1;
        for (int i = 0; i < g_n_rtp_in; i++)
            if (!g_rtp_in[i].enabled) { slot = i; break; }
        if (slot < 0) {
            if (g_n_rtp_in >= MAX_RTP) return;
            slot = g_n_rtp_in;
        }
        RtpStream *r = &g_rtp_in[slot];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%s", name);
        r->channels = n >= 4 ? atoi(tok[3]) : 2;
        r->ch_start = n >= 5 ? atoi(tok[4]) : slot * 2;
        int buf_ms  = RTP_FILL_TARGET * 1000 / SAMPLE_RATE;
        for (int i = 5; i < n; i++) {
            int v; if (sscanf(tok[i], "bufMs=%d", &v) == 1) { buf_ms = v; break; }
        }
        r->fill_target = buf_ms * SAMPLE_RATE / 1000;
        if (rtp_stream_open(r, 0)) {
            r->enabled = 1;
            if (slot == g_n_rtp_in) g_n_rtp_in++;
            for (int c = 0; c < r->channels; c++) {
                Cmd bc = { .type = CMD_BYPASS, .dir = 0, .ch = r->ch_start + c, .flag = 1 };
                cmd_push(&bc);
            }
        }
    } else if (!strcmp(sub, "remove")) {
        for (int i = 0; i < g_n_rtp_in; i++)
            if (!strcmp(g_rtp_in[i].name, name) && g_rtp_in[i].enabled) {
                rtp_stream_close(&g_rtp_in[i]); break;
            }
    }
}

static void cmd_rtp_out(int n, char **tok)
{
    if (n < 3) return;
    const char *sub  = tok[1];
    const char *name = tok[2];
    if (!strcmp(sub, "add")) {
        int slot = -1;
        for (int i = 0; i < g_n_rtp_out; i++)
            if (!g_rtp_out[i].enabled) { slot = i; break; }
        if (slot < 0) {
            if (g_n_rtp_out >= MAX_RTP) return;
            slot = g_n_rtp_out;
        }
        RtpStream *r = &g_rtp_out[slot];
        memset(r, 0, sizeof(*r));
        snprintf(r->name, sizeof(r->name), "%s", name);
        r->channels = n >= 4 ? atoi(tok[3]) : 2;
        r->ch_start = n >= 5 ? atoi(tok[4]) : slot * 2;
        if (rtp_stream_open(r, 1)) {
            r->enabled = 1;
            if (slot == g_n_rtp_out) g_n_rtp_out++;
            for (int c = 0; c < r->channels; c++) {
                Cmd bc = { .type = CMD_BYPASS, .dir = 1, .ch = r->ch_start + c, .flag = 1 };
                cmd_push(&bc);
            }
        }
    } else if (!strcmp(sub, "remove")) {
        for (int i = 0; i < g_n_rtp_out; i++)
            if (!strcmp(g_rtp_out[i].name, name) && g_rtp_out[i].enabled) {
                rtp_stream_close(&g_rtp_out[i]); break;
            }
    }
}

/* ── 기존 DSP 명령 (gain, mute, bypass) ─────────────────────────── */
static void cmd_dsp_legacy(const char *verb, int n, char **tok)
{
    if (n < 3) return;
    int dir = strcmp(tok[1], "in") ? 1 : 0;
    int ch  = atoi(tok[2]) - 1;
    if (ch < 0 || (dir == 0 && ch >= g_n_in) || (dir == 1 && ch >= g_n_out)) return;
    Cmd cmd = { .dir = dir, .ch = ch };
    if (!strcmp(verb, "gain") && n >= 4) {
        cmd.type = CMD_GAIN;
        cmd.gain = fmaxf(0.0f, fminf(GAIN_MAX, (float)atof(tok[3])));
        cmd_push(&cmd);
    } else if (!strcmp(verb, "mute") && n >= 4) {
        cmd.type = CMD_MUTE; cmd.flag = atoi(tok[3]);
        cmd_push(&cmd);
    } else if (!strcmp(verb, "bypass") && n >= 4) {
        cmd.type = CMD_BYPASS; cmd.flag = atoi(tok[3]);
        cmd_push(&cmd);
    }
}

/* ── 새 DSP 파라미터 명령 ────────────────────────────────────────── */

static void cmd_trim(int n, char **tok)
{
    /* trim in <ch> <db> */
    if (n < 4) return;
    int dir = strcmp(tok[1], "in") ? 1 : 0;
    int ch  = atoi(tok[2]) - 1;
    if (ch < 0) return;
    Cmd cmd = { .type = CMD_TRIM, .dir = dir, .ch = ch };
    cmd.trim_db = (float)atof(tok[3]);
    cmd_push(&cmd);
}

static void cmd_hpf(int n, char **tok)
{
    /* hpf in <ch> set slope <12|24|48> fc <hz>
     * hpf in <ch> enable|disable */
    if (n < 4) return;
    int ch = atoi(tok[2]) - 1;
    if (ch < 0 || ch >= g_n_in) return;

    Cmd cmd = { .type = CMD_HPF, .dir = 0, .ch = ch };
    HpfCmd *h = &cmd.hpf;

    if (!strcmp(tok[3], "disable")) {
        h->enabled = 0; h->n_sections = 0;
        cmd_push(&cmd); return;
    }
    if (!strcmp(tok[3], "enable")) {
        /* slope/fc는 현재 값 유지 — 별도로 "set"으로 먼저 설정 필요 */
        h->enabled = 1;
        cmd_push(&cmd); return;
    }
    /* set slope <12|24|48> fc <hz> */
    if (n < 7 || strcmp(tok[3], "set") || strcmp(tok[4], "slope")) return;
    int slope = atoi(tok[5]);
    float fc  = (n >= 8 && !strcmp(tok[6], "fc")) ? (float)atof(tok[7]) : 80.0f;

    const float *qs;
    int nsec;
    if      (slope == 48) { nsec = 4; qs = BUTTER_Q_48; }
    else if (slope == 24) { nsec = 2; qs = BUTTER_Q_24; }
    else                  { nsec = 1; qs = BUTTER_Q_12; }

    h->enabled    = 1;
    h->n_sections = nsec;
    for (int s = 0; s < nsec; s++) {
        Biquad tmp = {0};
        biquad_calc_hpf(&tmp, fc, qs[s], (float)SAMPLE_RATE);
        h->coef[s][0] = tmp.b0; h->coef[s][1] = tmp.b1; h->coef[s][2] = tmp.b2;
        h->coef[s][3] = tmp.a1; h->coef[s][4] = tmp.a2;
    }
    cmd_push(&cmd);
}

static void cmd_eq(int n, char **tok)
{
    /* eq in|out <ch> band <1-4> coef <b0 b1 b2 a1 a2>
     * eq in|out <ch> band <1-4> enable|disable */
    if (n < 6) return;
    int dir  = strcmp(tok[1], "in") ? 1 : 0;
    int ch   = atoi(tok[2]) - 1;
    int band = atoi(tok[4]) - 1;  /* tok[3]="band" */
    if (ch < 0 || band < 0 || band >= MAX_EQ_BANDS) return;

    Cmd cmd = { .type = CMD_EQ_BAND, .dir = dir, .ch = ch };
    EqBandCmd *e = &cmd.eq_band;
    e->band = band;

    if (!strcmp(tok[5], "disable")) {
        e->enabled = 0;
        cmd_push(&cmd); return;
    }
    if (!strcmp(tok[5], "enable")) {
        e->enabled = 1;
        cmd_push(&cmd); return;
    }
    /* coef <b0 b1 b2 a1 a2> */
    if (n < 11 || strcmp(tok[5], "coef")) return;
    e->b0 = (float)atof(tok[6]);
    e->b1 = (float)atof(tok[7]);
    e->b2 = (float)atof(tok[8]);
    e->a1 = (float)atof(tok[9]);
    e->a2 = (float)atof(tok[10]);
    e->enabled = 1;
    cmd_push(&cmd);
}

static void cmd_gate(int n, char **tok)
{
    /* gate in|out <ch> enable|disable
     * gate in|out <ch> set thr <db> attack <ms> release <ms> hold <ms> range <db> */
    if (n < 4) return;
    int dir = !strcmp(tok[1], "out") ? 1 : 0;
    int ch  = atoi(tok[2]) - 1;
    if (ch < 0 || (dir == 0 && ch >= g_n_in) || (dir == 1 && ch >= g_n_out)) return;

    Cmd cmd = { .type = CMD_GATE, .dir = dir, .ch = ch };
    GateCmd *g = &cmd.gate;

    if (!strcmp(tok[3], "disable")) { g->enabled = 0; cmd_push(&cmd); return; }
    if (!strcmp(tok[3], "enable"))  { g->enabled = 1; cmd_push(&cmd); return; }
    if (n < 14 || strcmp(tok[3], "set")) return;

    g->enabled = 1;
    /* set thr <db> attack <ms> release <ms> hold <ms> range <db> */
    for (int i = 4; i < n-1; i++) {
        if (!strcmp(tok[i], "thr"))     g->threshold_lin = powf(10.0f, (float)atof(tok[++i])/20.0f);
        else if (!strcmp(tok[i], "attack"))  g->attack_coef  = ms_to_coef((float)atof(tok[++i]), SAMPLE_RATE);
        else if (!strcmp(tok[i], "release")) g->release_coef = ms_to_coef((float)atof(tok[++i]), SAMPLE_RATE);
        else if (!strcmp(tok[i], "hold"))    g->hold_samples = (int)((float)atof(tok[++i]) * SAMPLE_RATE / 1000.0f);
        else if (!strcmp(tok[i], "range"))   g->range_lin    = powf(10.0f, (float)atof(tok[++i])/20.0f);
    }
    cmd_push(&cmd);
}

static void cmd_comp(int n, char **tok)
{
    /* comp in|out <ch> enable|disable
     * comp in|out <ch> set thr <db> ratio <r> knee <db> attack <ms> release <ms> makeup <db> */
    if (n < 4) return;
    int dir = strcmp(tok[1], "in") ? 1 : 0;
    int ch  = atoi(tok[2]) - 1;
    if (ch < 0) return;

    Cmd cmd = { .type = CMD_COMP, .dir = dir, .ch = ch };
    CompCmd *c = &cmd.comp;

    if (!strcmp(tok[3], "disable")) { c->enabled = 0; cmd_push(&cmd); return; }
    if (!strcmp(tok[3], "enable"))  { c->enabled = 1; cmd_push(&cmd); return; }
    if (n < 5 || strcmp(tok[3], "set")) return;

    c->enabled = 1;
    for (int i = 4; i < n-1; i++) {
        if      (!strcmp(tok[i], "thr"))     c->threshold_db = (float)atof(tok[++i]);
        else if (!strcmp(tok[i], "ratio"))   c->ratio        = (float)atof(tok[++i]);
        else if (!strcmp(tok[i], "knee"))    c->knee_db      = (float)atof(tok[++i]);
        else if (!strcmp(tok[i], "attack"))  c->attack_coef  = ms_to_coef((float)atof(tok[++i]), SAMPLE_RATE);
        else if (!strcmp(tok[i], "release")) c->release_coef = ms_to_coef((float)atof(tok[++i]), SAMPLE_RATE);
        else if (!strcmp(tok[i], "makeup"))  c->makeup_lin   = powf(10.0f, (float)atof(tok[++i])/20.0f);
    }
    cmd_push(&cmd);
}

static void cmd_lim(int n, char **tok)
{
    /* lim out <ch> enable|disable
     * lim out <ch> set thr <db> release <ms> */
    if (n < 4 || strcmp(tok[1], "out")) return;
    int ch = atoi(tok[2]) - 1;
    if (ch < 0 || ch >= g_n_out) return;

    Cmd cmd = { .type = CMD_LIM, .dir = 1, .ch = ch };
    LimCmd *l = &cmd.lim;

    if (!strcmp(tok[3], "disable")) { l->enabled = 0; cmd_push(&cmd); return; }
    if (!strcmp(tok[3], "enable"))  { l->enabled = 1; cmd_push(&cmd); return; }
    if (n < 5 || strcmp(tok[3], "set")) return;

    l->enabled = 1;
    for (int i = 4; i < n-1; i++) {
        if      (!strcmp(tok[i], "thr"))     l->threshold_lin = powf(10.0f, (float)atof(tok[++i])/20.0f);
        else if (!strcmp(tok[i], "release")) l->release_coef  = ms_to_coef((float)atof(tok[++i]), SAMPLE_RATE);
    }
    cmd_push(&cmd);
}

static void cmd_gr(int n, char **tok)
{
    /* gr enable|disable */
    if (n < 2) return;
    Cmd cmd = { .type = CMD_GR_ENABLE };
    cmd.flag = !strcmp(tok[1], "enable") ? 1 : 0;
    cmd_push(&cmd);
    fprintf(stderr, "[aoip_engine] gr %s\n", cmd.flag ? "on" : "off");
}

/* ── stdin 명령 루프 ─────────────────────────────────────────────── */
void cmd_loop(void)
{
    char line[512];
    while (fgets(line, sizeof(line), stdin)) {
        char *tok[20]; int n = 0;
        char *p = strtok(line, " \t\r\n");
        while (p && n < 20) { tok[n++] = p; p = strtok(NULL, " \t\r\n"); }
        if (n < 1) continue;

        const char *verb = tok[0];
        if      (!strcmp(verb, "set"))     cmd_set(n, tok);
        else if (!strcmp(verb, "bridge"))  cmd_bridge(n, tok);
        else if (!strcmp(verb, "route"))   cmd_route(n, tok);
        else if (!strcmp(verb, "rtp_in"))  cmd_rtp_in(n, tok);
        else if (!strcmp(verb, "rtp_out")) cmd_rtp_out(n, tok);
        else if (!strcmp(verb, "trim"))    cmd_trim(n, tok);
        else if (!strcmp(verb, "hpf"))     cmd_hpf(n, tok);
        else if (!strcmp(verb, "eq"))      cmd_eq(n, tok);
        else if (!strcmp(verb, "gate"))    cmd_gate(n, tok);
        else if (!strcmp(verb, "comp"))    cmd_comp(n, tok);
        else if (!strcmp(verb, "lim"))     cmd_lim(n, tok);
        else if (!strcmp(verb, "gr"))      cmd_gr(n, tok);
        else if (!strcmp(verb, "clk2"))  {
            g_clk2_report = n>=2 ? atoi(tok[1]) : 1;
            fprintf(stderr, "[aoip_engine] clk2 %s\n", g_clk2_report ? "on":"off");
        }
        else if (!strcmp(verb, "lvl"))   {
            g_lvl_report  = n>=2 ? atoi(tok[1]) : 1;
            fprintf(stderr, "[aoip_engine] lvl %s\n",  g_lvl_report  ? "on":"off");
        }
        else cmd_dsp_legacy(verb, n, tok);
    }
}
