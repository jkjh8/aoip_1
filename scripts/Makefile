CC      = gcc
CFLAGS  = -O3 -Wall -Wextra -ffast-math -march=native -funroll-loops \
          -falign-functions=64 -I. -I./include
LDFLAGS = -lrt -lasound -lsamplerate -lmp3lame -lopus -lm -lpthread

BIN_DIR   = bin
BUILD_DIR = build
SRC_DIR   = src

# Source files grouped by subsystem
CORE_SRCS = $(SRC_DIR)/core/aoip_engine.c   \
            $(SRC_DIR)/core/cmd_handler.c   \
            $(SRC_DIR)/core/config_loader.c \
            $(SRC_DIR)/core/bridge_manager.c

ALSA_SRCS = $(SRC_DIR)/alsa/alsa_device.c   \
            $(SRC_DIR)/alsa/dsp_io.c

DSP_SRCS  = $(SRC_DIR)/dsp/dsp_worker.c     \
            $(SRC_DIR)/dsp/dsp_reporter.c   \
            $(SRC_DIR)/dsp/dsp_eq.c         \
            $(SRC_DIR)/dsp/dsp_dynamics.c   \
            $(SRC_DIR)/dsp/dsp_channel.c    \
            $(SRC_DIR)/dsp/dsp_src.c

RTP_SRCS  = $(SRC_DIR)/rtp/rtp_recv.c       \
            $(SRC_DIR)/rtp/rtp_send.c

CLK_SRCS  = $(SRC_DIR)/clk/clk2.c

SRCS = $(CORE_SRCS) $(ALSA_SRCS) $(DSP_SRCS) $(RTP_SRCS) $(CLK_SRCS)
OBJS = $(patsubst $(SRC_DIR)/%.c,$(BUILD_DIR)/%.o,$(SRCS))

TARGET = $(BIN_DIR)/aoip_engine

all: $(TARGET)

$(TARGET): $(OBJS) | $(BIN_DIR)
	$(CC) $(CFLAGS) -o $@ $^ $(LDFLAGS)

$(BUILD_DIR)/%.o: $(SRC_DIR)/%.c
	@mkdir -p $(dir $@)
	$(CC) $(CFLAGS) -c -o $@ $<

$(BIN_DIR):
	@mkdir -p $@

clean:
	rm -rf $(BUILD_DIR) $(BIN_DIR)

.PHONY: all clean
