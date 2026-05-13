#pragma once
#include "ring_buf.h"

typedef struct RtpSendCtx RtpSendCtx;

RtpSendCtx *rtp_send_start(RingBuf *ring, const char *key, const char *sock_path, int prio);
void        rtp_send_stop(RtpSendCtx *ctx);
