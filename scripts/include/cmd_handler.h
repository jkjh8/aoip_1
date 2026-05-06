#pragma once
#include <stddef.h>
#include <stdatomic.h>
#include "engine_constants.h"
#include "dsp_channel.h"

/* ── 커맨드 타입 ─────────────────────────────────────────────────── */
typedef enum {
    /* 기존 */
    CMD_GAIN, CMD_MUTE, CMD_BYPASS, CMD_ROUTE_SET,
    /* 새 DSP 파라미터 */
    CMD_TRIM,
    CMD_HPF,
    CMD_EQ_BAND,
    CMD_GATE,
    CMD_COMP,
    CMD_LIM,
    CMD_GR_ENABLE,
} CmdType;

/* ── 커맨드 구조체 (SPSC 링버퍼 전송 단위) ──────────────────────── */
typedef struct {
    CmdType type;
    int     dir;   /* 0=in, 1=out */
    int     ch;
    union {
        float   gain;
        int     flag;
        float   trim_db;
        struct { int in_ch, out_ch; float level; } route;
        EqBandCmd eq_band;
        HpfCmd    hpf;
        GateCmd   gate;
        CompCmd   comp;
        LimCmd    lim;
    };
} Cmd;

/* ── SPSC 링버퍼 ─────────────────────────────────────────────────── */
typedef struct {
    Cmd            buf[CMD_RING_SIZE];
    _Atomic size_t wr, rd;
} CmdRing;

extern CmdRing g_cmd_ring;

/* ── 공개 함수 ───────────────────────────────────────────────────── */
void cmd_push(const Cmd *c);
int  cmd_pop (Cmd *c);
void cmd_loop(void);
