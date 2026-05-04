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
#   8. 권한 설정
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

NODE_VER=$(node --version 2>/dev/null | sed 's/v//' | cut -d. -f1 || echo 0)
if [ "$NODE_VER" -lt 18 ] 2>/dev/null; then
    warn "Node.js v${NODE_VER} — v20 설치 중..."
    curl -fsSL https://deb.nodesource.com/setup_20.x | bash -
    apt-get install -y nodejs
else
    info "Node.js $(node --version) 설치됨"
fi

if [ -f "${AOIP_DIR}/package.json" ]; then
    info "npm install 실행 중..."
    cd "${AOIP_DIR}"
    npm install --omit=dev
fi

# =============================================================================
# 3. Ravenna ALSA 커널 모듈 빌드 및 설치
# =============================================================================
section "3. Ravenna ALSA 커널 모듈"

INSTALL_DIR="$(cd "$(dirname "$0")" && pwd)"
RAVENNA_SRC="${INSTALL_DIR}/ravenna-alsa-lkm/driver"
RAVENNA_KO="${RAVENNA_SRC}/MergingRavennaALSA.ko"
KVER=$(uname -r)
KMOD_EXTRA="/lib/modules/${KVER}/extra"

if [ -f "${RAVENNA_SRC}/Makefile" ]; then
    info "커널 모듈 빌드 중... (커널: ${KVER})"
    make -C "${RAVENNA_SRC}" clean
    make -C "${RAVENNA_SRC}"

    info "커널 모듈 설치: ${KMOD_EXTRA}/"
    mkdir -p "${KMOD_EXTRA}"
    cp "${RAVENNA_KO}" "${KMOD_EXTRA}/"
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

SERVICES=(
    ravenna-module.service
    ptp4l.service
    uac2-gadget.service
    aes67-daemon.service
    aoip.service
)

for svc in "${SERVICES[@]}"; do
    SRC="${SYSTEMD_SRC}/${svc}"
    if [ -f "${SRC}" ]; then
        cp "${SRC}" "${SYSTEMD_DST}/${svc}"
        info "${svc} 설치됨"
    else
        warn "${svc} 없음 — 건너뜀"
    fi
done

systemctl daemon-reload

for svc in "${SERVICES[@]}"; do
    if [ -f "${SYSTEMD_DST}/${svc}" ]; then
        systemctl enable "${svc}"
        info "${svc} 자동시작 활성화"
    fi
done

# =============================================================================
# 8. 권한 설정
# =============================================================================
section "8. 권한 설정"

# 실행 권한
chmod +x "${INSTALL_DIR}/aes67/aes67-daemon"
chmod +x "${INSTALL_DIR}/aes67/scripts/"*.sh
chmod +x "${AOIP_DIR}/scripts/"*.sh

# 현재 로그인 사용자를 audio 그룹에 추가
TARGET_USER="${SUDO_USER:-$(logname 2>/dev/null || echo "")}"
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
