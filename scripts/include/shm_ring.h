#pragma once
#include <stdatomic.h>
#include <stdint.h>
#include <stddef.h>

/* POSIX 공유 메모리 링버퍼 — aoip_engine(owner), rtp_recv/rtp_send(attach) 공통 레이아웃 */
#define SHM_RING_FRAMES  32768
#define SHM_MAX_CH       24

typedef struct {
    _Atomic uint32_t wp;        /* 단조 증가 write frame 카운터 */
    _Atomic uint32_t rp;        /* 단조 증가 read  frame 카운터 */
    int32_t  channels;
    int32_t  ring_frames;
    uint8_t  _pad[48];          /* 헤더를 64바이트로 정렬 */
    float    buf[SHM_RING_FRAMES * SHM_MAX_CH];
} ShmRing;

#define SHMRING_SIZE ((size_t)sizeof(ShmRing))
