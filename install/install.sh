#!/bin/bash
# =============================================================================
# AoIP 통합 설치 스크립트
# 대상: Raspberry Pi OS Bookworm (aarch64)
#
# 수행 작업:
#   1. apt 패키지 설치
#   2. Node.js 확인 + npm install
#   3. Ravenna ALSA 커널 모듈 빌드 및 설치
#   4. aoip 커스텀 DAC 드라이버 설치 (install-aoip-dac8x.sh)
#   5. C 바이너리 빌드 (aoip_engine, rtp_recv, rtp_send)
#   6. uac2-gadget.sh 배포
#   7. systemd 서비스 설치 및 활성화
#   8. daemon.conf 동적 설정 (MAC → IP, multicast)
#   9. 권한 설정
# =============================================================================

set -e

RED='\033[0;31m'
GREEN='\033[0;32m'
YELLOW='\033[1;33m'
NC='\033[0m'

info()    { echo -e "${GREEN}[INFO]${NC} $1"; }
warn()    { echo -e "${YELLOW}[WARN]${NC} $1"; }
error()   { echo -e "${RED}[ERROR]${NC} $1"; exit 1; }
section() { echo ""; echo -e "${GREEN}=== $1 ===${NC}"; }

if [ "$EUID" -ne 0 ]; then
    error "root 권한이 필요합니다: sudo $0"
fi

# 프로젝트 루트 (install/ 의 부모)
AOIP_DIR="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL_DIR="$(cd "$(dirname "$0")" && pwd)"
info "프로젝트 경로: ${AOIP_DIR}"

# =============================================================================
# 1. apt 패키지 설치
# =============================================================================
section "1. apt 패키지 설치"

PKGS=(
    # PTP
    linuxptp

    # aes67-daemon 런타임 의존성
    libboost-log1.83.0
    libboost-filesystem1.83.0
    libboost-thread1.83.0
    libboost-program-options1.83.0
    libavahi-client3
    libavahi-daemon
    libasound2t64
    libfaac0t64
    libcap2

    # C 빌드 의존성
    libmp3lame-dev
    libsamplerate0-dev
    libasound2-dev

    # 커널 모듈 / DT 오버레이 빌드
    build-essential
    device-tree-compiler
    linux-headers-$(uname -r)
)

PKGS_NEEDED=()
for pkg in "${PKGS[@]}"; do
    if ! dpkg -l "$pkg" 2>/dev/null | grep -q "^ii"; then
        PKGS_NEEDED+=("$pkg")
    fi
done

if [ ${#PKGS_NEEDED[@]} -gt 0 ]; then
    info "설치 필요: ${PKGS_NEEDED[*]}"
    apt-get update -qq
    apt-get install -y "${PKGS_NEEDED[@]}"
else
    info "모든 apt 패키지 설치되어 있음"
fi

# =============================================================================
# 2. Node.js 확인 + npm install
# =============================================================================
section "2. Node.js / npm"

# NVM 또는 시스템 node 경로 감지
NODE_BIN=""
# 설치된 사용자의 NVM 경로 먼저 탐색
TARGET_USER="${SUDO_USER:-$(logname 2>/dev/null || echo "")}"
if [ -n "${TARGET_USER}" ]; then
    NVM_DIR="/home/${TARGET_USER}/.nvm"
    if [ -d "${NVM_DIR}/versions/node" ]; then
        # 가장 최신 버전 선택
        NVM_NODE=$(ls -v "${NVM_DIR}/versions/node" | tail -1)
        if [ -n "${NVM_NODE}" ]; then
            CANDIDATE="${NVM_DIR}/versions/node/${NVM_NODE}/bin/node"
            if [ -x "${CANDIDATE}" ]; then
                NODE_BIN="${CANDIDATE}"
                info "NVM Node.js 감지: ${NODE_BIN}"
            fi
        fi
    fi
fi

# NVM 없으면 시스템 node 사용
if [ -z "${NODE_BIN}" ]; then
    NODE_BIN=$(command -v node 2>/dev/null || true)
    if [ -z "${NODE_BIN}" ]; then
        warn "Node.js 없음 — v20 설치 중..."
        curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
        apt-get install -y nodejs
        NODE_BIN=$(command -v node)
    fi
    info "Node.js 감지: ${NODE_BIN}"
fi

NODE_VER=$("${NODE_BIN}" --version 2>/dev/null | sed 's/v//' | cut -d. -f1 || echo 0)
if [ "${NODE_VER}" -lt 18 ] 2>/dev/null; then
    warn "Node.js v${NODE_VER} — v20 이상 권장"
fi

NODE_DIR=$(dirname "${NODE_BIN}")
info "Node.js $(${NODE_BIN} --version) — ${NODE_BIN}"

if [ -f "${AOIP_DIR}/package.json" ]; then
    info "npm install 실행 중..."
    cd "${AOIP_DIR}"
    sudo -u "${TARGET_USER:-root}" "${NODE_DIR}/npm" install --omit=dev
fi

# =============================================================================
# 3. Ravenna ALSA 커널 모듈 빌드 및 설치
# =============================================================================
section "3. Ravenna ALSA 커널 모듈"

RAVENNA_SRC="${INSTALL_DIR}/ravenna-alsa-lkm/driver"
KVER=$(uname -r)
KMOD_EXTRA="/lib/modules/${KVER}/extra"

if [ -f "${RAVENNA_SRC}/Makefile" ]; then
    info "커널 모듈 빌드 중... (커널: ${KVER})"
    make -C "${RAVENNA_SRC}" clean
    make -C "${RAVENNA_SRC}"
    mkdir -p "${KMOD_EXTRA}"
    cp "${RAVENNA_SRC}/MergingRavennaALSA.ko" "${KMOD_EXTRA}/"
    depmod -a
    info "MergingRavennaALSA.ko 설치 완료"
else
    warn "Makefile 없음 — 기존 빌드된 .ko 사용"
    if [ -f "${INSTALL_DIR}/aes67/driver/MergingRavennaALSA.ko" ]; then
        mkdir -p "${KMOD_EXTRA}"
        cp "${INSTALL_DIR}/aes67/driver/MergingRavennaALSA.ko" "${KMOD_EXTRA}/"
        depmod -a
        info "aes67/driver/MergingRavennaALSA.ko 설치 완료"
    else
        warn "MergingRavennaALSA.ko 없음 — Ravenna 모듈 설치 건너뜀"
    fi
fi

# =============================================================================
# 4. aoip 커스텀 DAC 드라이버 설치
# =============================================================================
section "4. aoip DAC 드라이버"

DAC_SCRIPT="${INSTALL_DIR}/install-aoip-dac8x.sh"
if [ -f "${DAC_SCRIPT}" ]; then
    info "install-aoip-dac8x.sh 실행 중..."
    bash "${DAC_SCRIPT}"
else
    warn "install-aoip-dac8x.sh 없음 — 건너뜀"
fi

# =============================================================================
# 5. C 바이너리 빌드
# =============================================================================
section "5. C 바이너리 빌드"

info "aoip_engine, rtp_recv, rtp_send 빌드 중..."
make -C "${AOIP_DIR}/scripts"
info "빌드 완료"

# ptp4l LD_PRELOAD shim 빌드 (IP_MULTICAST_LOOP 강제 ON)
# → 같은 호스트의 aes67-daemon이 ptp4l PTP 멀티캐스트 패킷 수신 가능하도록
PTP_SHIM_SRC="${INSTALL_DIR}/ptp_mcast_loop.c"
PTP_SHIM_DST="/usr/local/lib/ptp_mcast_loop.so"
if [ -f "${PTP_SHIM_SRC}" ]; then
    info "ptp_mcast_loop.so 빌드 중..."
    gcc -O2 -fPIC -shared -Wl,-soname,ptp_mcast_loop.so \
        "${PTP_SHIM_SRC}" -o "${PTP_SHIM_DST}" -ldl
    chmod 0755 "${PTP_SHIM_DST}"
    info "ptp_mcast_loop.so → ${PTP_SHIM_DST}"
else
    warn "ptp_mcast_loop.c 없음 — shim 빌드 건너뜀 (ptp4l/aes67-daemon 동시동작 불가)"
fi

# =============================================================================
# 5b. ptp-i2s-sync 커널 모듈 (I2S ↔ RAVENNA 클럭 동기화)
# =============================================================================
section "5b. ptp-i2s-sync 커널 모듈"

CLK_SYNC_SH="${INSTALL_DIR}/install-clk-sync.sh"
if [ -f "${CLK_SYNC_SH}" ]; then
    bash "${CLK_SYNC_SH}"
else
    warn "install-clk-sync.sh 없음 — clk 동기화 모듈 건너뜀"
fi

# =============================================================================
# 6. uac2-gadget.sh 배포
# =============================================================================
section "6. UAC2 Gadget 스크립트 배포"

UAC2_SH="${INSTALL_DIR}/gadget/uac2-gadget.sh"
UAC2_DST="/usr/local/bin/uac2-gadget.sh"

if [ -f "${UAC2_SH}" ]; then
    cp "${UAC2_SH}" "${UAC2_DST}"
    chmod +x "${UAC2_DST}"
    info "uac2-gadget.sh → ${UAC2_DST}"
else
    warn "uac2-gadget.sh 없음 — 건너뜀"
fi

# =============================================================================
# 7. systemd 서비스 설치 및 활성화
# =============================================================================
section "7. systemd 서비스 설치"

SYSTEMD_SRC="${INSTALL_DIR}/systemd"
SYSTEMD_DST="/etc/systemd/system"

# 설치 + 활성화할 서비스
SERVICES=(
    ravenna-module.service
    ptp4l.service
    phc2sys.service
    ptp-irq-affinity.service
    uac2-gadget.service
    aes67-daemon.service
    aoip-soundcard.service
    aoip.service
    aoip-rt-tune.service
)
# phc2sys: phc2sys-freqonly.sh 래퍼를 ExecStart로 호출.
#   외부 마스터가 wall time을 운반하지 않는 환경(PHC=1970)에서도 sys clock step 없이
#   주파수만 동기화 (-F 0 -S 0). linreg servo로 ±수십 ppm 변동.
#   tick은 변조하지 않음 (이전 phc2sys 표준 모드의 tick=9841 사고 회피).

for svc in "${SERVICES[@]}"; do
    SRC="${SYSTEMD_SRC}/${svc}"
    if [ -f "${SRC}" ]; then
        cp "${SRC}" "${SYSTEMD_DST}/${svc}"
        info "${svc} 설치됨"
    else
        warn "${svc} 없음 — 건너뜀"
    fi
done

# aoip.service: node 경로 치환
sed -i "s|__NODE_BIN__|${NODE_BIN}|g" "${SYSTEMD_DST}/aoip.service"
sed -i "s|__NODE_DIR__|${NODE_DIR}|g" "${SYSTEMD_DST}/aoip.service"
info "aoip.service node 경로 설정: ${NODE_BIN}"

systemctl daemon-reload

for svc in "${SERVICES[@]}"; do
    if [ -f "${SYSTEMD_DST}/${svc}" ]; then
        systemctl enable "${svc}"
        info "${svc} 자동시작 활성화"
    fi
done

# aoip-rt-tune.sh 스크립트 배포
RT_TUNE_SH="${SYSTEMD_SRC}/aoip-rt-tune.sh"
if [ -f "${RT_TUNE_SH}" ]; then
    cp "${RT_TUNE_SH}" /usr/local/sbin/aoip-rt-tune.sh
    chmod +x /usr/local/sbin/aoip-rt-tune.sh
    info "aoip-rt-tune.sh → /usr/local/sbin/"
fi

# ptp-irq-affinity.sh 스크립트 배포
PTP_IRQ_SH="${SYSTEMD_SRC}/ptp-irq-affinity.sh"
if [ -f "${PTP_IRQ_SH}" ]; then
    cp "${PTP_IRQ_SH}" /usr/local/sbin/ptp-irq-affinity.sh
    chmod +x /usr/local/sbin/ptp-irq-affinity.sh
    info "ptp-irq-affinity.sh → /usr/local/sbin/"
fi

# phc2sys-freqonly.sh 배포 (phc2sys.service가 호출)
PHC2SYS_SH="${SYSTEMD_SRC}/phc2sys-freqonly.sh"
if [ -f "${PHC2SYS_SH}" ]; then
    cp "${PHC2SYS_SH}" /usr/local/sbin/phc2sys-freqonly.sh
    chmod +x /usr/local/sbin/phc2sys-freqonly.sh
    info "phc2sys-freqonly.sh → /usr/local/sbin/"
fi

# ptp-freq-sync.py 배포 (백업 경로 — 현재 SERVICES에는 없으나 긴급 롤백용)
PTP_FREQ_PY="${SYSTEMD_SRC}/ptp-freq-sync.py"
if [ -f "${PTP_FREQ_PY}" ]; then
    cp "${PTP_FREQ_PY}" /usr/local/sbin/ptp-freq-sync.py
    chmod +x /usr/local/sbin/ptp-freq-sync.py
    info "ptp-freq-sync.py → /usr/local/sbin/ (백업)"
fi

# =============================================================================
# 8. daemon.conf 동적 설정 (MAC 기반)
# =============================================================================
section "8. daemon.conf 동적 설정"

DAEMON_CONF="${INSTALL_DIR}/aes67/daemon.conf"

if [ ! -f "${DAEMON_CONF}" ]; then
    warn "daemon.conf 없음 — 건너뜀"
else
    # eth0 MAC 주소 읽기
    ETH_MAC=$(cat /sys/class/net/eth0/address 2>/dev/null || true)
    if [ -z "${ETH_MAC}" ]; then
        warn "eth0 MAC 읽기 실패 — daemon.conf 설정 건너뜀"
    else
        # eth0 IP 주소 읽기
        ETH_IP=$(ip -4 addr show eth0 2>/dev/null | awk '/inet /{print $2}' | cut -d/ -f1 | head -1)
        if [ -z "${ETH_IP}" ]; then
            warn "eth0 IP 없음 — ip_addr 설정 건너뜀"
        fi

        # MAC → multicast 주소 생성: 239.69.<byte4>.<byte5>
        IFS=':' read -ra MAC_BYTES <<< "${ETH_MAC}"
        MCAST_X=$((16#${MAC_BYTES[4]}))
        MCAST_Y=$((16#${MAC_BYTES[5]}))
        RTP_MCAST="239.69.${MCAST_X}.${MCAST_Y}"

        info "MAC: ${ETH_MAC}"
        info "IP:  ${ETH_IP:-unchanged}"
        info "RTP Multicast Base: ${RTP_MCAST}"

        # daemon.conf JSON 패치 (python3 이용)
        python3 - "${DAEMON_CONF}" "${ETH_MAC}" "${ETH_IP}" "${RTP_MCAST}" <<'PYEOF'
import sys, json

conf_path = sys.argv[1]
mac       = sys.argv[2]
ip        = sys.argv[3] if sys.argv[3] else None
mcast     = sys.argv[4]

with open(conf_path) as f:
    cfg = json.load(f)

cfg["mac_addr"]       = mac
cfg["custom_node_id"] = f"aoip {mac}"
cfg["node_id"]        = f"aoip {mac}"
cfg["rtp_mcast_base"] = mcast
if ip:
    cfg["ip_addr"] = ip

with open(conf_path, "w") as f:
    json.dump(cfg, f, indent=2)
    f.write("\n")
PYEOF
        info "daemon.conf 업데이트 완료"
    fi
fi

# =============================================================================
# 9. 권한 설정
# =============================================================================
section "9. 권한 설정"

chmod +x "${INSTALL_DIR}/aes67/aes67-daemon"
chmod +x "${INSTALL_DIR}/aes67/scripts/"*.sh
chmod +x "${AOIP_DIR}/scripts/tools/"*.sh

if [ -n "${TARGET_USER}" ]; then
    usermod -aG audio "${TARGET_USER}"
    info "${TARGET_USER} → audio 그룹 추가"
fi

# =============================================================================
# 완료
# =============================================================================
echo ""
echo "============================================="
echo -e " ${GREEN}설치 완료!${NC}"
echo "============================================="
echo ""
echo " 서비스 시작:"
echo "   sudo systemctl start ptp4l aes67-daemon uac2-gadget aoip"
echo ""
echo " 상태 확인:"
echo "   systemctl status ptp4l aes67-daemon uac2-gadget aoip"
echo ""
echo " ALSA 장치 확인:"
echo "   aplay -l | grep -E 'RAVENNA|UAC2|sndrpi'"
echo ""
echo " (재부팅 권장: sudo reboot)"
echo ""
