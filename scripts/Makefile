CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -ffast-math -march=native \
          $(shell pkg-config --cflags gstreamer-1.0 gstreamer-app-1.0 gstreamer-net-1.0 gio-2.0)
LDFLAGS = -lm -lpthread \
          $(shell pkg-config --libs gstreamer-1.0 gstreamer-app-1.0 gstreamer-net-1.0 gio-2.0)

AOIP_CFLAGS  = -O3 -Wall -Wextra -ffast-math -march=native -funroll-loops
AOIP_LDFLAGS = -lrt -lasound -lsamplerate -lm -lpthread

TARGETS = aoip_engine rtp_recv rtp_send

all: $(TARGETS)

aoip_engine: aoip_engine.c
	$(CC) $(AOIP_CFLAGS) -o $@ $< $(AOIP_LDFLAGS)

rtp_recv: rtp_recv.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

rtp_send: rtp_send.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS) -lsamplerate

clean:
	rm -f $(TARGETS)

.PHONY: all clean
