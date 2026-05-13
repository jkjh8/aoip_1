#pragma once
#include "ring_buf.h"

typedef struct RtpRecvCtx RtpRecvCtx;

RtpRecvCtx *rtp_recv_start(RingBuf *ring, const char *key, const char *sock_path, int prio);
void        rtp_recv_stop(RtpRecvCtx *ctx);
