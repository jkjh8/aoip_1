#pragma once
#include "shm_ring.h"

typedef struct RtpSendCtx RtpSendCtx;

/* ring: ShmRing owned by engine (already mmap'd)
 * key:  stream name, e.g. "rtp_out_1"
 * sock_path: Unix socket path to connect to Node.js for config/commands/stats */
RtpSendCtx *rtp_send_start(ShmRing *ring, const char *key, const char *sock_path, int prio);
void        rtp_send_stop(RtpSendCtx *ctx);
