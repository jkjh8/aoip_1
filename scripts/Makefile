CC      = gcc
CFLAGS  = -O3 -Wall -Wextra -ffast-math -march=native -funroll-loops \
          -falign-functions=64 -I. -I./include
LDFLAGS = -lrt -lasound -lsamplerate -lmp3lame -lopus -lm -lpthread

SRCS = aoip_engine.c   \
       cmd_handler.c   \
       dsp_worker.c    \
       dsp_io.c        \
       dsp_reporter.c  \
       bridge_manager.c \
       config_loader.c \
       dsp_eq.c        \
       dsp_dynamics.c  \
       dsp_channel.c   \
       alsa_device.c   \
       rtp_recv.c      \
       rtp_send.c      \
       clk2.c          \
       dsp_src.c

OBJS = $(SRCS:.c=.o)

all: aoip_engine

aoip_engine: $(OBJS)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

%.o: %.c
	$(CC) $(CFLAGS) -c -o $@ $<

clean:
	rm -f aoip_engine $(OBJS)

.PHONY: all clean
