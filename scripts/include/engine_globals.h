#pragma once
/*
 * engine_globals.h — aoip_engine.c에 정의된 전역 변수 extern 선언
 * 파일 분리된 모듈(cmd_handler, dsp_io, dsp_reporter 등)이 공유.
 */
#include <stdatomic.h>
#include <pthread.h>
#include "engine_constants.h"
#include "alsa_device.h"
#include "rtp_stream.h"
#include "dsp_channel.h"

/* ── 오디오 처리 설정 ────────────────────────────────────────────── */
extern int           g_dsp_clock_fd;
extern int           g_period_frames;
extern int           g_ravenna_fill_target;
extern int           g_prio_dsp, g_prio_alsa, g_prio_ravenna, g_prio_rtp;
extern _Atomic int   g_quit;
extern int           g_bypass_all_dsp;
extern float         g_sr;

/* ── ALSA 장치 + RTP 스트림 ─────────────────────────────────────── */
extern Device        g_dev[MAX_DEVICES];
extern int           g_n_dev;

extern RtpStream     g_rtp_in[MAX_RTP];
extern int           g_n_rtp_in;
extern RtpStream     g_rtp_out[MAX_RTP];
extern int           g_n_rtp_out;

/* ── DSP 채널 상태 배열 ─────────────────────────────────────────── */
extern InChDspState  g_in_ch[MAX_CH];
extern OutChDspState g_out_ch[MAX_CH];
extern int           g_n_in, g_n_out;

/* ── 라우팅 매트릭스 ────────────────────────────────────────────── */
extern float         g_route[MAX_CH][MAX_CH];

/* ── 레벨 미터 원자 배열 ────────────────────────────────────────── */
extern _Atomic float g_in_level[MAX_CH];
extern _Atomic float g_out_level[MAX_CH];

/* ── GR 미터링 원자 배열 ─────────────────────────────────────────── */
extern _Atomic float g_in_gr_gate[MAX_CH];
extern _Atomic float g_in_gr_comp[MAX_CH];
extern _Atomic float g_out_gr_gate[MAX_CH];
extern _Atomic float g_out_gr_comp[MAX_CH];
extern _Atomic float g_out_gr_lim[MAX_CH];
extern volatile int  g_gr_report;

/* ── 리포터 제어 ────────────────────────────────────────────────── */
extern volatile int  g_reporter_running;
extern volatile int  g_lvl_report;

/* ── 오디오 버퍼 포인터 ─────────────────────────────────────────── */
extern float        *g_in_ptr[MAX_CH];
extern float        *g_out_ptr[MAX_CH];
extern float         g_in_buf_static[MAX_CH][MAX_PERIOD_FRAMES];
extern float         g_out_buf_static[MAX_CH][MAX_PERIOD_FRAMES];
extern float         g_interleave_tmp[MAX_PERIOD_FRAMES * MAX_CH];
