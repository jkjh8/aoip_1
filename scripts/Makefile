CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -ffast-math -march=native \
          $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-net-1.0 gio-2.0)
LDFLAGS = -ljack -lm -lpthread \
          $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-net-1.0 gio-2.0)

DSP_CFLAGS  = -O3 -Wall -Wextra -ffast-math -march=native -funroll-loops
DSP_LDFLAGS = -ljack -lm -lpthread

TARGETS = dsp_engine rtp_recv rtp_send jack_pipe_out

all: $(TARGETS)

dsp_engine: dsp_engine.c
	$(CC) $(DSP_CFLAGS) -o $@ $< $(DSP_LDFLAGS)

rtp_recv: rtp_recv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rtp_send: rtp_send.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

jack_pipe_out: jack_pipe_out.c
	$(CC) $(DSP_CFLAGS) -o $@ $< $(DSP_LDFLAGS)

clean:
	rm -f $(TARGETS)

.PHONY: all clean
