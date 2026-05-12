#pragma once
#define _GNU_SOURCE
#include <pthread.h>
#include <stdatomic.h>
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
#define DEV_TMP_FRAMES ((MAX_PERIOD_FRAMES + 8) * 2)
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

    int          is_clock_master; /* 1 = 이 장치의 I2S 크리스탈이 DSP 클럭 기준 */
    int          clk_accum;       /* 마스터 클럭 누적 프레임 카운터 */

    /* ── Ravenna 직결 ───────────────────────────────────────────────── */
    int              is_ravenna;
    /* 캡처: 더블버퍼 (ALSA 캡처 스레드 → DSP)
     * RAVENNA hw period < g_period_frames 일 때 여러 번 누적 후 swap */
    float            direct_cap [2][MAX_PERIOD_FRAMES * MAX_CH];
    _Atomic uint32_t direct_cap_wp;
    int              direct_cap_acc;  /* 현재 쓰기 슬롯 내 누적 프레임 수 */
    /* 재생: DSP 스레드가 직접 PCM write (논블로킹)
     * ravenna_pcm은 DSP 스레드가 읽고, reopen 스레드가 씀 — 자연 정렬 포인터 원자성 이용 */
    snd_pcm_t       *ravenna_pcm;
    int              ravenna_buf_frames;
    int32_t          ravenna_ibuf[MAX_PERIOD_FRAMES * MAX_CH];
    /* 재오픈 요청: DSP 스레드가 1로 설정 → reopen 스레드가 처리 후 0으로 */
    _Atomic int      ravenna_reopen;
    pthread_t        ravenna_reopen_tid;
    /* 구 PCM 닫기 위임: DSP 스레드가 close를 reopen 스레드에 위임 (RT 스레드 블로킹 방지) */
    snd_pcm_t       *ravenna_dying_pcm;
    /* EIO 백오프: PTP 미잠금 시 매 틱 ioctl 호출 방지 */
    int              ravenna_eio_count;    /* 연속 EIO 횟수 */
    int              ravenna_eio_backoff;  /* 0이 될 때까지 write skip */

    /* RAVENNA 클럭 마스터: 캡처 스레드가 g_period_frames 누적 시 DSP eventfd 신호 */
    int              ravenna_accum;        /* 누적 프레임 수 (캡처 스레드만 접근) */

    /* PTP 잠금 상태: 0=EIO(뮤트), 1=정상
     * 캡처 스레드가 쓰고, DSP 스레드가 읽음 */
    _Atomic int      ravenna_ptp_locked;
} Device;

/* ── 함수 선언 ───────────────────────────────────────────────────── */
snd_pcm_t *alsa_open(const char *dev, int stream, int rate,
                     int period, int nperiods, int ch);
void device_start(Device *d);
void device_stop(Device *d);
