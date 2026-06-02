#pragma once
#define _GNU_SOURCE
#include <stdint.h>
#include <pthread.h>
#include <stdatomic.h>
#include <alsa/asoundlib.h>
#include <samplerate.h>
#include "engine_constants.h"
#include "ring_buf.h"

/* ── PI 드리프트 보정 상수 ──────────────────────────────────────── */
#define RATIO_KP   0.0005      /* 버퍼 오차 → ratio 비례 반응 */
#define RATIO_KI   0.000003    /* 버퍼 오차 적분 → 영구 drift 제거 (~1s 수렴 목표) */
#define RATIO_MIN  0.99980
#define RATIO_MAX  1.00020

/* ── PI 상태 ─────────────────────────────────────────────────────── */
typedef struct {
    double ratio, integ, smooth;
    double kp, ki, min, max;   /* 튜닝 상수 (초기화 시 설정, reset 후에도 유지) */
} PiState;

void pi_reset(PiState *p);
void pi_update(PiState *p, int avail, int target);

/* ── ALSA 장치 ───────────────────────────────────────────────────── */
#define DEV_TMP_FRAMES ((MAX_PERIOD_FRAMES + 8) * 2)

typedef struct {
    char  name[32];
    char  dev[64];
    int   rate, period, nperiods, channels;
    int   enabled;
    int   ch_start;
    int   mode;           /* 0=both, 1=capture_only, 2=playback_only */

    RingBuf    in_ring;   /* RAVENNA/RTP 경로 전용 (SRC 필요) */
    RingBuf    out_ring;

    /* I2S zero-copy SlotRing — hw:aoip 전용, RAVENNA는 in_ring/out_ring 사용 */
    SlotRing   i2s_in_ring;
    SlotRing   i2s_out_ring;
    /* 캡처 스레드: 현재 write slot 포인터 배열 (g_period_frames 누적용) */
    float     *i2s_cap_ptrs[MAX_CH];
    int        i2s_cap_fill;         /* 현재 슬롯에 채운 프레임 수 */
    int        i2s_in_acquired;      /* 이번 DSP 틱에 입력 슬롯 획득 여부 */
    int        i2s_out_acquired;     /* 이번 DSP 틱에 출력 슬롯 획득 여부 */

    PiState    cap_pi;
    SRC_STATE *cap_src;
    PiState    play_pi;
    SRC_STATE *play_src;

    float tmp_cap_in  [DEV_TMP_FRAMES * MAX_CH]; /* RAVENNA SRC 임시 버퍼 */
    float tmp_cap_out [DEV_TMP_FRAMES * MAX_CH];
    float tmp_play_in [DEV_TMP_FRAMES * MAX_CH];
    float tmp_play_out[DEV_TMP_FRAMES * MAX_CH];

    pthread_t    cap_tid;
    pthread_t    play_tid;
    volatile int quit_cap;
    volatile int quit_play;
    int          thread_priority;

    int          is_i2s;
    int          clk_accum;
    uint32_t     cap_underrun;   /* 연속 캡처 언더런 틱 카운터 */
    uint32_t     play_overflow;  /* 연속 재생 오버플로우 틱 카운터 */

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

    /* PTP 언락 시 재생 스레드에 out_ring 플러시 요청:
     * 캡처 스레드가 1로 설정 → 재생 스레드가 rb_reset 후 0으로 */
    _Atomic int      ravenna_flush;

    /* out_ring 리셋 후 play SRC/PI 재초기화 요청:
     * 재생 스레드가 1로 설정 → DSP 스레드가 src_reset+pi_reset 후 0으로 */
    _Atomic int      play_src_reset;

    /* PTP 재잠금 후 클럭 안정화 prebuffer 누적 카운터 (캡처 스레드만 접근) */
    int              ravenna_prebuf_count;
    /* Phase 2 벽시계 시작 시각 (CLOCK_MONOTONIC ns) — phase2_printed=1 시 기록, holdover 만료 시 0 리셋 */
    int64_t          ravenna_phase2_start_ns;
    /* Phase 2 진입 메시지 출력 여부 — holdover 만료 시에만 클리어 (EPIPE 후 재진입 시 중복 출력 방지) */
    int              ravenna_phase2_printed;

    /* PTP 잠금 후 cap SRC 시작 전 in_ring 충전 완료 플래그 (DSP 스레드 전용) */
    int              cap_prebuf_ready;

    /* PTP 홀드오버: 언락 감지 후 무음 공급 누적 프레임 수 (캡처 스레드만 접근)
     * 0 = 정상, >0 = 홀드오버 중, RAVENNA_HOLDOVER_FRAMES 도달 시 뮤트 전환 */
    int              ravenna_holdover_frames;
} Device;

/* ── RT 스레드 CPU 어피니티 ──────────────────────────────────────── */
/* CPU 레이아웃: CPU1=ptp4l, CPU2=DSP/ALSA/rtp_recv, CPU3=rtp_send  */
static inline void pin_to_cpu(int cpu) {
    cpu_set_t cs; CPU_ZERO(&cs); CPU_SET(cpu, &cs);
    pthread_setaffinity_np(pthread_self(), sizeof(cs), &cs);
}

/* ── 함수 선언 ───────────────────────────────────────────────────── */
snd_pcm_t *alsa_open(const char *dev, int stream, int rate,
                     int period, int nperiods, int ch);
void device_start(Device *d);
void device_stop(Device *d);
