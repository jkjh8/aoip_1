CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -ffast-math -march=native -I.
LDFLAGS = -lm -lpthread -lrt

AOIP_CFLAGS  = -O3 -Wall -Wextra -ffast-math -march=native -funroll-loops -I.
AOIP_LDFLAGS = -lrt -lasound -lsamplerate -lm -lpthread

TARGETS = aoip_engine rtp_recv rtp_send

all: $(TARGETS)

# aoip_engine: 메인 엔진 + ALSA 장치 관리 + DSP 수학 함수
aoip_engine: aoip_engine.c alsa_device.c dsp_math.c
	$(CC) $(AOIP_CFLAGS) -o $@ $^ $(AOIP_LDFLAGS)

# GStreamer-free: libsamplerate + minimp3 (header-only)
rtp_recv: rtp_recv.c include/shm_ring.h
	$(CC) $(CFLAGS) -o $@ $< -lsamplerate -lmp3lame $(LDFLAGS)

rtp_send: rtp_send.c
	$(CC) $(CFLAGS) -o $@ $< -lsamplerate -lmp3lame $(LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
