#pragma once
#include "alsa_device.h"
#include "ring_buf.h"

int  ring_capture_src(SRC_STATE *src, PiState *pi,
                      RingBuf *ring,
                      float *tmp_in, float *tmp_out,
                      int fill_target, int channels, int ch_start);
void alsa_playback_src(Device *d);
