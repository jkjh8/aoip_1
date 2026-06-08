#pragma once
#include <stdint.h>
#include <stdatomic.h>

/* hw:aoip ↔ hw:RAVENNA 클럭 비교 — ALSA htstamp 기반 (DMA 인터럽트 시점)
 * 스레드 지터 없음: 커널이 DMA 완료 시 기록하는 CLOCK_MONOTONIC 타임스탬프 사용
 * frames: 누적 캡처 프레임 수,  hts_ns: 최신 ALSA htstamp (ns)
 * seq: seqlock 카운터 (frames/hts_ns 페어를 일관성 있게 스냅샷하기 위함)
 *      writer 진입 전후로 1씩 증가 → 짝수=안정, 홀수=갱신 중 */
extern _Atomic int64_t  g_aoip_frames;
extern _Atomic int64_t  g_aoip_hts_ns;
extern _Atomic uint32_t g_aoip_seq;
extern _Atomic int64_t  g_ravenna_frames;
extern _Atomic int64_t  g_ravenna_hts_ns;
extern _Atomic uint32_t g_ravenna_seq;

extern volatile int g_clk2_report;

/* RAVENNA/aoip 실측 비율 힌트: cap_pi 초기값으로 사용 (기본값 1.0) */
extern _Atomic double g_ravenna_ratio_hint;

/* I2S 베이스라인 보정 완료 플래그 — RAVENNA/RTP 스레드는 이 신호 후에 진입해야
 * 클럭 도메인 점프 없이 안전하게 PCM open / RTP join 가능. */
extern _Atomic int g_clk_ready;

/* RAVENNA PTP 락 도달 플래그 — 라이브 ppb 보정은 이 신호 이후에만 실행.
 * alsa_device 가 "PTP locked (3s stable)" 시점에 1 로 set. */
extern _Atomic int g_ptp_locked;

/* PTP 재동기화 카운터 — alsa_device 가 매 lock 시점(최초/재락 모두)에 +1.
 * clk2 는 이 값 변화 감지 시 ratio_hint 갱신을 일정 시간 freeze + median 윈도우 리셋.
 * PTP 끊김/재연결 시 ravenna_rate 점프가 DSP SRC 피치로 새지 않도록 보호. */
extern _Atomic uint32_t g_ptp_resync_gen;

/* hw:aoip 캡처 스레드가 첫 readi 성공 직후 1회 호출. clk_ready 신호만 set
 * (precal 적용은 dsp_io 에서 RAVENNA SRC ready 시점에 수행). */
void clk2_apply_initial_ppb(void);

/* RAVENNA SRC ready 시점에 dsp_io 에서 1회 호출 — PPB_INIT 를 sysfs 에 적용. */
void clk2_apply_precal(void);

void clk2_report(int64_t *pa_fr,  int64_t *pa_hts,
                 int64_t *pr_fr,  int64_t *pr_hts,
                 int64_t *next_ns);
