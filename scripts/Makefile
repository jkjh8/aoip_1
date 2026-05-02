CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -ffast-math -march=native -I. \
          $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-net-1.0 gio-2.0)
LDFLAGS = -lm -lpthread \
          $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-net-1.0 gio-2.0)

AOIP_CFLAGS  = -O3 -Wall -Wextra -ffast-math -march=native -funroll-loops -I.
AOIP_LDFLAGS = -lrt -lasound -lsamplerate -lm -lpthread

TARGETS = aoip_engine rtp_recv rtp_send

all: $(TARGETS)

# aoip_engine: 메인 엔진 + ALSA 장치 관리 + DSP 수학 함수
aoip_engine: aoip_engine.c alsa_device.c dsp_math.c
	$(CC) $(AOIP_CFLAGS) -o $@ $^ $(AOIP_LDFLAGS)

rtp_recv: rtp_recv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rtp_send: rtp_send.c
	$(CC) $(CFLAGS) -o $@ $< -lsamplerate -lmp3lame -lm -lpthread -lrt

clean:
	rm -f $(TARGETS)

.PHONY: all clean
