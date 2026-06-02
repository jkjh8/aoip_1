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

void clk2_report(int64_t *pa_fr,  int64_t *pa_hts,
                 int64_t *pr_fr,  int64_t *pr_hts,
                 int64_t *next_ns);
