#!/usr/bin/env python3
# PTP role-aware 시스템 클럭 관리자
#
# 배경: RAVENNA 캡처 링버퍼의 읽기 포인터는 CLOCK_MONOTONIC(시스템 클럭),
# 쓰기 포인터는 송신자 RTP 타임스탬프(PTP GM 도메인)를 따른다. 두 도메인의
# 주파수가 어긋나면 상대 표류가 누적되어 입력 오디오가 깨진다.
# 이 데몬은 ptp4l의 포트 역할에 따라 두 도메인을 주파수 단위로 통합한다.
#
#   SLAVE  : 외부 GM 존재 → chrony 정지, 시스템 클럭 주파수를 PHC 기울기에
#            슬레이빙 (FLL). 위상/에포크는 건드리지 않으므로 GM이 ARB
#            타임스케일이거나 재부팅으로 에포크가 점프해도 안전.
#   MASTER : 이 박스가 GM → chrony 재개(시스템=UTC), phc2sys로 PHC를
#            시스템 클럭에 종속 → 슬레이브 장비들이 우리 도메인을 추종.
#   그 외  : (FAULTY/LISTENING 등) chrony 재개, PHC 불간섭.
#
# 주의: 위상을 절대 끌어당기지 않는다. 과거 phc2sys-freqonly.sh 가 잔차
# 위상(≤0.5s)을 slew 로 소거하려다 tick 왜곡(9841µs) 사고를 냈다.

import ctypes
import os
import signal
import subprocess
import sys
import time

IFACE = os.environ.get("PCM_IFACE", "eth0")
PHC_DEV = os.environ.get("PCM_PHC", "/dev/ptp0")
CHRONY_UNIT = "chrony.service"
PMC = "/usr/sbin/pmc"
PHC2SYS = "/usr/sbin/phc2sys"

# bcm_phy_ptp(/dev/ptp0)는 MDIO 경유라 1회 읽기에 ~3ms 걸린다 → 위상
# 샘플 노이즈 ±1.4ms. µs급 측정은 불가능하지만 링버퍼 예산이 3ms이므로
# 위상 앵커는 중앙값으로 ±0.5ms 만 유지하면 충분하고, 기울기는 긴
# 윈도우 회귀로 뽑는다.
POLL_SEC = 10            # 역할 폴링 = 표류 샘플 주기
DEBOUNCE = 2             # 역할 전환에 필요한 연속 동일 판정 횟수
WINDOW_N = 60            # 보정 1회당 샘플 수 (~10분 윈도우)
ANCHOR_N = 15            # 위상 앵커/오차 산출용 중앙값 표본 수
STEP_GUARD_NS = 10_000_000   # 예측오차 10ms 초과 = 진짜 PHC 스텝(에포크 점프)
GAIN = 0.4               # 주파수(기울기) 게인 (샘플 노이즈 커서 보수적으로)
MAX_DELTA_PPB = 150      # 1회 보정 상한 (급격한 freq 변화는 aoip_rate 측정을 흔든다)
MAX_AUTHORITY_PPB = 15_000   # slave 진입 시점 freq 대비 총 보정 한계
PHASE_TC_SEC = 1200      # 위상 오차를 회수하는 시정수 (완만하게)
PHASE_SLEW_PPB = 150     # 위상 회수용 목표 기울기 상한
PHASE_REANCHOR_NS = 3_000_000  # 위상 오차가 이보다 크면 싸우지 않고 재앵커
OFFSET_SANE_NS = 5_000_000   # ptp4l offsetFromMaster 가 이보다 크면 PHC 를 신뢰하지 않음

ADJ_FREQUENCY = 0x0002
NOMINAL_TICK = 10000


class _timex(ctypes.Structure):
    _fields_ = [
        ("modes", ctypes.c_uint), ("offset", ctypes.c_long),
        ("freq", ctypes.c_long), ("maxerror", ctypes.c_long),
        ("esterror", ctypes.c_long), ("status", ctypes.c_int),
        ("constant", ctypes.c_long), ("precision", ctypes.c_long),
        ("tolerance", ctypes.c_long), ("time_sec", ctypes.c_long),
        ("time_usec", ctypes.c_long), ("tick", ctypes.c_long),
        ("ppsfreq", ctypes.c_long), ("jitter", ctypes.c_long),
        ("shift", ctypes.c_int), ("stabil", ctypes.c_long),
        ("jitcnt", ctypes.c_long), ("calcnt", ctypes.c_long),
        ("errcnt", ctypes.c_long), ("stbcnt", ctypes.c_long),
        ("tai", ctypes.c_int), ("pad", ctypes.c_int * 11),
    ]


class _timespec(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_nsec", ctypes.c_long)]


_libc = ctypes.CDLL("libc.so.6", use_errno=True)


def log(msg):
    print(f"clock-manager: {msg}", flush=True)


def adjtimex_get():
    tx = _timex()
    tx.modes = 0
    if _libc.adjtimex(ctypes.byref(tx)) < 0:
        raise OSError(ctypes.get_errno(), "adjtimex read")
    return tx


def adjtimex_set_freq_ppb(ppb):
    tx = _timex()
    tx.modes = ADJ_FREQUENCY
    tx.freq = int(ppb * 65536 / 1000)
    if _libc.adjtimex(ctypes.byref(tx)) < 0:
        raise OSError(ctypes.get_errno(), "adjtimex freq")


def freq_ppb():
    return adjtimex_get().freq * 1000 / 65536


class PhcReader:
    def __init__(self, dev):
        self.fd = os.open(dev, os.O_RDONLY)
        self.clkid = ((~self.fd) << 3) | 3

    SANDWICH_MAX_NS = 5_000_000   # MDIO 정상 폭(~3ms) 허용, 그 이상은 선점
    SANDWICH_TRIES = 3

    def phc_minus_real_ns(self):
        """sandwich 읽기로 (PHC − CLOCK_REALTIME) ns 반환.
        3회 중 가장 폭이 좁은 표본 채택, 전부 비정상이면 None"""
        best = None
        best_w = None
        for _ in range(self.SANDWICH_TRIES):
            tr1, tp, tr2 = _timespec(), _timespec(), _timespec()
            _libc.clock_gettime(0, ctypes.byref(tr1))
            _libc.clock_gettime(self.clkid, ctypes.byref(tp))
            _libc.clock_gettime(0, ctypes.byref(tr2))
            r1 = tr1.tv_sec * 1_000_000_000 + tr1.tv_nsec
            r2 = tr2.tv_sec * 1_000_000_000 + tr2.tv_nsec
            w = r2 - r1
            if w <= self.SANDWICH_MAX_NS and (best_w is None or w < best_w):
                p = tp.tv_sec * 1_000_000_000 + tp.tv_nsec
                best = p - (r1 + r2) // 2
                best_w = w
        return best


def pmc_query(cmd):
    try:
        out = subprocess.run([PMC, "-u", "-b", "0", cmd],
                             capture_output=True, text=True, timeout=5)
        return out.stdout
    except (subprocess.TimeoutExpired, OSError):
        return ""


def get_role():
    """(role, offset_ns) — role ∈ slave|master|neutral"""
    out = pmc_query("GET PORT_DATA_SET")
    state = ""
    for line in out.splitlines():
        if "portState" in line:
            state = line.split()[-1]
            break
    if state == "SLAVE":
        cur = pmc_query("GET CURRENT_DATA_SET")
        for line in cur.splitlines():
            if "offsetFromMaster" in line:
                try:
                    off = abs(float(line.split()[-1]))
                except ValueError:
                    off = None
                if off is not None and off < OFFSET_SANE_NS:
                    return "slave", off
                return "neutral", off
        return "neutral", None
    if state in ("MASTER", "GRAND_MASTER"):
        return "master", None
    return "neutral", None


def chrony(action):
    subprocess.run(["systemctl", action, CHRONY_UNIT],
                   capture_output=True, timeout=30)


class Manager:
    def __init__(self):
        self.phc = PhcReader(PHC_DEV)
        self.mode = "neutral"
        self.pending = None
        self.pending_count = 0
        self.phc2sys = None
        self.samples = []          # (monotonic_sec, phc_minus_real_ns)
        self.base_freq = None
        self.d0 = None             # 위상 앵커: slave 진입 시점의 (PHC−REALTIME)
        self.tick_sanity()

    def tick_sanity(self):
        tick = adjtimex_get().tick
        if tick != NOMINAL_TICK:
            log(f"경고: tick={tick} → {NOMINAL_TICK} 복구")
            tx = _timex()
            tx.modes = 0x4000  # ADJ_TICK
            tx.tick = NOMINAL_TICK
            _libc.adjtimex(ctypes.byref(tx))

    # ---- 모드 전환 ----
    def enter(self, mode):
        if mode == self.mode:
            return
        log(f"모드 전환: {self.mode} → {mode}")
        self.stop_phc2sys()
        self.samples.clear()
        if mode == "slave":
            chrony("stop")
            self.base_freq = freq_ppb()
            self.d0 = None
            log(f"chrony 정지, freq 슬레이빙 시작 (base {self.base_freq:+.0f} ppb)")
        else:
            chrony("start")
            if self.mode == "slave":
                log("chrony 재개 (누적 wall 오차는 chrony 가 완만히 회수)")
            if mode == "master":
                self.start_phc2sys()
        self.mode = mode

    def start_phc2sys(self):
        # 시스템 클럭(=chrony/UTC) → PHC. 최초 1회 대점프는 step 허용(기본값).
        cmd = [PHC2SYS, "-s", "CLOCK_REALTIME", "-c", IFACE, "-O", "0", "-u", "60"]
        self.phc2sys = subprocess.Popen(cmd)
        log(f"phc2sys 시작 (REALTIME → {IFACE} PHC), pid {self.phc2sys.pid}")

    def stop_phc2sys(self):
        if self.phc2sys and self.phc2sys.poll() is None:
            self.phc2sys.terminate()
            try:
                self.phc2sys.wait(timeout=5)
            except subprocess.TimeoutExpired:
                self.phc2sys.kill()
            log("phc2sys 종료")
        self.phc2sys = None

    # ---- SLAVE 모드: 위상 앵커 PI ----
    # 순수 FLL(기울기만)은 추적 잔차가 랜덤워크로 누적되어 주 단위에서
    # 링버퍼 예산(3ms)을 위협한다. slave 진입 시점 오프셋 D0 를 앵커로 잡고
    # 위상 이탈을 완만히(시정수 PHASE_TC_SEC) 회수해 누적을 차단한다.
    # 에포크 값 자체는 의미가 없으므로(ARB GM) "고정 오프셋 락"으로 충분하다.
    def slave_tick(self):
        now = time.monotonic()
        d = self.phc.phc_minus_real_ns()
        if d is None:
            return  # 선점으로 오염된 샘플은 버린다
        if self.samples:
            t0, d_last = self.samples[-1]
            slope = self.slope()
            predicted = d_last + (slope or 0) * (now - t0)
            if abs(d - predicted) > STEP_GUARD_NS:
                log(f"PHC 위상 점프 감지 ({(d - predicted)/1e3:+.0f} µs) "
                    f"— 윈도우 리셋 + 재앵커")
                self.samples.clear()
                self.d0 = d
        self.samples.append((now, d))
        if self.d0 is None and len(self.samples) >= ANCHOR_N:
            ds = sorted(s[1] for s in self.samples[:ANCHOR_N])
            self.d0 = ds[len(ds) // 2]
            log(f"위상 앵커 설정 (중앙값 {ANCHOR_N}표본)")
        if self.d0 is None or len(self.samples) < WINDOW_N:
            return
        slope = self.slope()
        if slope is None:
            return
        tail = sorted(s[1] for s in self.samples[-ANCHOR_N:])
        e_phase = tail[len(tail) // 2] - self.d0
        if abs(e_phase) > PHASE_REANCHOR_NS:
            log(f"위상 오차 {e_phase/1e3:+.0f} µs > 한계 — 재앵커 (회수 포기)")
            self.d0 = d
            e_phase = 0
        # 목표 기울기: 위상 오차를 PHASE_TC_SEC 에 걸쳐 회수
        want = max(-PHASE_SLEW_PPB, min(PHASE_SLEW_PPB, -e_phase / PHASE_TC_SEC))
        delta = max(-MAX_DELTA_PPB, min(MAX_DELTA_PPB, GAIN * (slope - want)))
        cur = freq_ppb()
        new = cur + delta
        lo = self.base_freq - MAX_AUTHORITY_PPB
        hi = self.base_freq + MAX_AUTHORITY_PPB
        new = max(lo, min(hi, new))
        adjtimex_set_freq_ppb(new)
        log(f"기울기 {slope:+.1f} ppb, 위상 {e_phase/1e3:+.1f} µs "
            f"→ freq {cur:+.0f} → {new:+.0f} ppb")
        self.samples.clear()  # freq 변경으로 이전 샘플 무효 (d0 앵커는 유지)

    def slope(self):
        """윈도우 선형회귀 기울기 (ns/s == ppb). 샘플 부족 시 None"""
        if len(self.samples) < 4:
            return None
        xs = [s[0] for s in self.samples]
        ys = [s[1] for s in self.samples]
        n = len(xs)
        mx = sum(xs) / n
        my = sum(ys) / n
        den = sum((x - mx) ** 2 for x in xs)
        if den == 0:
            return None
        return sum((x - mx) * (y - my) for x, y in zip(xs, ys)) / den

    # ---- 메인 루프 ----
    def run(self):
        log(f"시작 (iface={IFACE}, phc={PHC_DEV})")
        while True:
            role, off = get_role()
            if role != self.mode:
                if role == self.pending:
                    self.pending_count += 1
                else:
                    self.pending, self.pending_count = role, 1
                if self.pending_count >= DEBOUNCE:
                    self.enter(role)
                    self.pending, self.pending_count = None, 0
            else:
                self.pending, self.pending_count = None, 0

            if self.mode == "slave":
                self.slave_tick()
            elif self.mode == "master" and self.phc2sys \
                    and self.phc2sys.poll() is not None:
                log("phc2sys 비정상 종료 — 재시작")
                self.start_phc2sys()

            time.sleep(POLL_SEC)

    def shutdown(self, *_):
        log("종료 — chrony 복구")
        self.stop_phc2sys()
        chrony("start")
        sys.exit(0)


if __name__ == "__main__":
    m = Manager()
    signal.signal(signal.SIGTERM, m.shutdown)
    signal.signal(signal.SIGINT, m.shutdown)
    m.run()
