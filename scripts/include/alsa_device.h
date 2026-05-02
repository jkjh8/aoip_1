#pragma once
#define _GNU_SOURCE
#include <pthread.h>
#include <alsa/asoundlib.h>
#include <samplerate.h>
#include "engine_constants.h"
#include "ring_buf.h"

/* ── PI 드리프트 보정 상수 ──────────────────────────────────────── */
#define RATIO_KP   0.00005
#define RATIO_KI   0.000000005
#define RATIO_MIN  0.99990
#define RATIO_MAX  1.00010

/* ── PI 상태 ─────────────────────────────────────────────────────── */
typedef struct {
    double ratio, integ, smooth;
    int    prebuf_done;
} PiState;

void pi_reset(PiState *p);
void pi_update(PiState *p, int avail, int target);

/* ── ALSA 장치 ───────────────────────────────────────────────────── */
#define DEV_TMP_FRAMES ((PERIOD_FRAMES + 8) * 2)
#define UNDERRUN_FADE_PERIODS 4

typedef struct {
    char  name[32];
    char  dev[64];
    int   rate, period, nperiods, channels;
    int   enabled;
    int   ch_start;
    int   mode;           /* 0=both, 1=capture_only, 2=playback_only */

    RingBuf    in_ring;
    RingBuf    out_ring;

    PiState    cap_pi;
    SRC_STATE *cap_src;
    PiState    play_pi;
    SRC_STATE *play_src;

    float tmp_cap_in  [DEV_TMP_FRAMES * MAX_CH];
    float tmp_cap_out [DEV_TMP_FRAMES * MAX_CH];
    float tmp_play_in [DEV_TMP_FRAMES * MAX_CH];
    float tmp_play_out[DEV_TMP_FRAMES * MAX_CH];

    pthread_t    cap_tid;
    pthread_t    play_tid;
    volatile int quit_cap;
    volatile int quit_play;
    int          thread_priority;

    float       *cap_fade_buf;  /* 마지막 정상 캡처 데이터 — 언더런 시 페이드 소스 */
    int          cap_fade_cnt;
} Device;

/* ── 함수 선언 ───────────────────────────────────────────────────── */
snd_pcm_t *alsa_open(const char *dev, int stream, int rate,
                     int period, int nperiods, int ch);
void device_start(Device *d);
void device_stop(Device *d);
