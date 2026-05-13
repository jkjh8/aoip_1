#pragma once
#include <samplerate.h>
#include "engine_constants.h"
#include "ring_buf.h"
#include "alsa_device.h"

typedef struct {
    char    name[32];
    int     channels;
    int     ch_start;
    int     enabled;
    RingBuf ring;
    int     is_send;
    void   *rtp_ctx;

    int        prebuffering;        /* 1=실데이터 채워질 때까지 zeros 출력 */
    PiState    rtp_pi;
    SRC_STATE *rtp_src;
    int        rtp_underrun;        /* 연속 언더런 틱 카운터 */
    float      rtp_in_buf [DEV_TMP_FRAMES * MAX_CH];
    float      rtp_out_buf[DEV_TMP_FRAMES * MAX_CH];
} RtpStream;
