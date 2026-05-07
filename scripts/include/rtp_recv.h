#pragma once
#include "shm_ring.h"

typedef struct RtpRecvCtx RtpRecvCtx;

/* ring: ShmRing owned by engine (already mmap'd)
 * key:  stream name, e.g. "rtp_in_1"
 * sock_path: Unix socket path to connect to Node.js for config/stats */
RtpRecvCtx *rtp_recv_start(ShmRing *ring, const char *key, const char *sock_path);
void        rtp_recv_stop(RtpRecvCtx *ctx);
