#pragma once

#define SAMPLE_RATE         48000
#define MAX_PERIOD_FRAMES   512
#define DEFAULT_PERIOD_FRAMES 24
#define RING_FRAMES         16384
#define MAX_CH          64
#define MAX_DEVICES     8
#define MAX_RTP         16
#define MAX_EQ_BANDS    4
#define CMD_RING_SIZE   128
#define GAIN_MAX        2.0f
#define DSP_WORKER_COUNT 2
#define DSP_WORKER_CH   (MAX_CH / DSP_WORKER_COUNT)
#define FILL_TARGET     (RING_FRAMES / 4)
#define PREBUF_FRAMES   (RING_FRAMES / 8)

/* RTP 링버퍼 크기 및 적응형 버퍼 타겟 (bufMs=500ms → 24000 frames @48kHz) */
#define RTP_RING_FRAMES 65536
#define RTP_FILL_TARGET 24000

/* RAVENNA 캡처 SRC 버퍼 타겟 (1ms @ 48kHz) */
#define RAVENNA_FILL_TARGET (SAMPLE_RATE * 1 / 1000)

/* PTP 언락 시 즉시 뮤트 대신 무음 공급으로 버퍼를 유지하는 홀드오버 기간 (200ms) */
#define RAVENNA_HOLDOVER_FRAMES (SAMPLE_RATE / 5)

/* RTP 전용 PI 상수 — ALSA PI보다 20x 빠른 응답, ±1000ppm 보정 범위 */
#define RTP_RATIO_KP   0.001
#define RTP_RATIO_KI   0.000001
#define RTP_RATIO_MIN  0.9990
#define RTP_RATIO_MAX  1.0010

/* SlotRing 슬롯 수 — I2S zero-copy 링버퍼 (RING_FRAMES / MAX_PERIOD_FRAMES = 32) */
#define SLOT_COUNT (RING_FRAMES / MAX_PERIOD_FRAMES)
