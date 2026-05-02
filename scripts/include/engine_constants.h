#pragma once

#define SAMPLE_RATE     48000
#define PERIOD_FRAMES   512
#define RING_FRAMES     16384
#define MAX_CH          8
#define MAX_DEVICES     8
#define MAX_RTP         4
#define MAX_EQ_BANDS    4
#define CMD_RING_SIZE   128
#define GAIN_MAX        2.0f
#define FILL_TARGET     (RING_FRAMES / 4)
#define PREBUF_FRAMES   (RING_FRAMES / 8)
