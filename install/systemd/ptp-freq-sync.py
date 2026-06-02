#!/usr/bin/env python3
"""
ptp-freq-sync: ptp4l freq 보정값 → CLOCK_REALTIME adjtimex 복사 데몬

phc2sys 대체 목적:
  외부 PTP 마스터가 1970 epoch을 사용할 때 phc2sys는 56년 offset을
  servo로 좁히려다 adjtimex freq를 ±10%로 saturate시킴.
  → CLOCK_MONOTONIC 왜곡 → ALSA htstamp 측정 파괴

이 데몬은 offset을 무시하고 ptp4l이 이미 계산한 PHC freq 보정값만 복사:
  - ptp4l log: "rms NNN max NNN freq -13122 +/- NNN delay NNN"
  - freq 값(ppb)을 adjtimex(ADJ_FREQUENCY)로 CLOCK_REALTIME에 적용
  - ±MAX_PPM으로 클램핑 → CLOCK_MONOTONIC 왜곡 방지
"""

import re
import sys
import time
import ctypes
import ctypes.util
import subprocess
import signal
import logging

logging.basicConfig(
    level=logging.INFO,
    format='%(asctime)s ptp-freq-sync: %(message)s',
    datefmt='%Y-%m-%d %H:%M:%S',
)
log = logging.getLogger()

MAX_PPM   = 500           # adjtimex freq 클램핑 한계 (±500 ppm)
APPLY_MIN = 0.5           # 이 ppm 미만 변화는 adjtimex 재호출 생략 (EMA가 부드럽게 흐르도록 작게)
TIMEOUT_S = 30            # 이 초 동안 ptp4l 로그 없으면 freq=0으로 복귀

# Leaky integrator (EMA) — ptp4l freq 추정 자체가 ±수 ppm으로 흔들려서
# 그대로 adjtimex에 실으면 CLOCK_MONOTONIC freq가 1초 주기로 진동한다.
# RAVENNA 커널 드라이버가 자체 PI servo에 95% 적분으로 흡수하는 것과 같은 발상.
# α=0.95 → 시상수 약 20 샘플(=ptp4l 통계 주기 1s 기준 ~20초).
EMA_ALPHA   = 0.95
WARMUP_N    = 5           # 초기 N 샘플은 raw 그대로 적용 (빠른 수렴)
OUTLIER_PPM = 50.0        # ema 대비 이 폭 이상이면 α를 더 키워 천천히 흡수 (lock 손실 등 보호)

# ── adjtimex 인터페이스 ──────────────────────────────────────────────────────
ADJ_FREQUENCY = 0x0002
ADJ_TICK      = 0x4000
TICK_NOMINAL  = 10000   # USER_HZ=100 → 10000µs
_libc = ctypes.CDLL(ctypes.util.find_library('c'), use_errno=True)

class _Timex(ctypes.Structure):
    _fields_ = [
        ('modes',    ctypes.c_uint),
        ('offset',   ctypes.c_long),
        ('freq',     ctypes.c_long),  # ppm * 2^16 단위
        ('maxerror', ctypes.c_long),
        ('esterror', ctypes.c_long),
        ('status',   ctypes.c_int),
        ('constant', ctypes.c_long),
        ('precision',ctypes.c_long),
        ('tolerance',ctypes.c_long),
        ('tv_sec',   ctypes.c_long),
        ('tv_usec',  ctypes.c_long),
        ('tick',     ctypes.c_long),
        ('_pad',     ctypes.c_long * 20),
    ]

def _reset_tick():
    """tick을 USER_HZ=100 기준값(10000µs)으로 복구.
    phc2sys가 남긴 비정상 tick 값이 CLOCK_MONOTONIC을 왜곡하는 것을 방지."""
    t = _Timex()
    t.modes = ADJ_TICK
    t.tick  = TICK_NOMINAL
    ret = _libc.adjtimex(ctypes.byref(t))
    t2 = _Timex()
    _libc.adjtimex(ctypes.byref(t2))
    log.info(f'tick 복구: {t2.tick}µs (ret={ret})')

def _set_freq_ppb(ppb: float) -> float:
    ppm = ppb / 1000.0
    ppm = max(-MAX_PPM, min(MAX_PPM, ppm))
    kernel_freq = int(ppm * 65536)
    t = _Timex()
    t.modes = ADJ_FREQUENCY
    t.freq  = kernel_freq
    _libc.adjtimex(ctypes.byref(t))
    return ppm

def _reset_freq():
    t = _Timex()
    t.modes = ADJ_FREQUENCY
    t.freq  = 0
    _libc.adjtimex(ctypes.byref(t))

# ── 메인 루프 ──────────────────────────────────────────────────────────────
_FREQ_RE = re.compile(r'\bfreq\s+(-?\d+)\b')

def main():
    current_ppm   = 0.0           # 마지막으로 adjtimex에 실제 적용된 값
    ema_ppm       = None          # leaky integrator 상태
    sample_cnt    = 0
    last_seen     = time.monotonic()

    def _sigterm(_sig, _frame):
        log.info('SIGTERM — freq=0으로 복귀')
        _reset_freq()
        sys.exit(0)

    signal.signal(signal.SIGTERM, _sigterm)
    signal.signal(signal.SIGINT,  _sigterm)

    _reset_tick()
    log.info(f'시작 (MAX_PPM={MAX_PPM}, APPLY_MIN={APPLY_MIN}, TIMEOUT={TIMEOUT_S}s)')

    proc = subprocess.Popen(
        ['journalctl', '-f', '-u', 'ptp4l', '-o', 'cat', '--no-hostname'],
        stdout=subprocess.PIPE,
        stderr=subprocess.DEVNULL,
        text=True,
    )

    for line in proc.stdout:
        line = line.rstrip()
        m = _FREQ_RE.search(line)
        if not m:
            # timeout 체크: ptp4l 로그가 오래 없으면 freq 복귀
            if time.monotonic() - last_seen > TIMEOUT_S and current_ppm != 0.0:
                log.warning(f'{TIMEOUT_S}s 동안 ptp4l freq 없음 — freq=0 복귀, EMA 리셋')
                _reset_freq()
                current_ppm = 0.0
                ema_ppm     = None
                sample_cnt  = 0
            continue

        ppb     = float(m.group(1))
        raw_ppm = ppb / 1000.0
        last_seen = time.monotonic()

        # Leaky integrator 갱신
        if ema_ppm is None or sample_cnt < WARMUP_N:
            # 초기 워밍업: raw 그대로 — 빠른 수렴
            ema_ppm = raw_ppm
        else:
            # 큰 점프(lock 손실, 마스터 교체 등)는 α를 키워 더 천천히 흡수
            alpha = 0.98 if abs(raw_ppm - ema_ppm) > OUTLIER_PPM else EMA_ALPHA
            ema_ppm = alpha * ema_ppm + (1.0 - alpha) * raw_ppm
        sample_cnt += 1

        if abs(ema_ppm - current_ppm) < APPLY_MIN:
            continue

        applied = _set_freq_ppb(ema_ppm * 1000.0)
        log.info(f'ptp4l freq={ppb:+.0f}ppb ema={ema_ppm:+.3f}ppm → adjtimex {applied:+.3f}ppm')
        current_ppm = applied

if __name__ == '__main__':
    main()
