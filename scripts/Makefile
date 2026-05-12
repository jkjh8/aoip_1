CC      = gcc
CFLAGS  = -O3 -Wall -Wextra -ffast-math -march=native -funroll-loops -I.
LDFLAGS = -lrt -lasound -lsamplerate -lmp3lame -lm -lpthread

all: aoip_engine i2s_measure

aoip_engine: aoip_engine.c alsa_device.c dsp_math.c rtp_recv.c rtp_send.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

i2s_measure: i2s_measure.c
	$(CC) -O2 -Wall -o $@ $^ -lasound

clean:
	rm -f aoip_engine i2s_measure

.PHONY: all clean
