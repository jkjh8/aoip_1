CC      = gcc
CFLAGS  = -O2 -Wall -Wextra -ffast-math -march=native
LDFLAGS = -ljack -lm -lpthread
TARGET  = dsp_engine

$(TARGET): dsp_engine.c
	$(CC) $(CFLAGS) -o $@ $< $(LDFLAGS)

clean:
	rm -f $(TARGET)

.PHONY: clean
