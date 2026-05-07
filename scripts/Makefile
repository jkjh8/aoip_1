CC      = gcc
CFLAGS  = -O3 -Wall -Wextra -ffast-math -march=native -funroll-loops \
          -falign-functions=64 -I.
LDFLAGS = -lrt -lasound -lsamplerate -lmp3lame -lopus -lm -lpthread

all: aoip_engine

aoip_engine: aoip_engine.c alsa_device.c rtp_recv.c rtp_send.c clk2.c dsp_src.c
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

clean:
	rm -f aoip_engine

.PHONY: all clean
