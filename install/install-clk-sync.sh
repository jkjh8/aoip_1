#!/bin/bash
# =============================================================================
# install-clk-sync.sh
#
# I2S ↔ RAVENNA 클럭 동기화 기능 설치 스크립트.
#
# 구성요소:
#   1. ptp-i2s-sync 커널 모듈 빌드 + /lib/modules/.../extra 에 설치
#   2. /etc/modules-load.d/ptp-i2s-sync.conf — 부팅 시 자동 로드
#   3. ptp-i2s-sync.service — 모듈 로드 + sysfs 권한(audio 그룹 write) 설정
#
# 동작 개요:
#   - 모듈이 sysfs /sys/kernel/ptp_i2s_sync/freq_ppb 노출
#   - aoip_engine 의 clk2 컨트롤러가 hw:aoip ↔ RAVENNA drift 측정 후
#     pll_audio_core 의 ppb 를 미세 조정하여 ±1ppm 내로 수렴
#   - 초기 보정값 PPB_INIT=24000 (scripts/clk2.c) 은 hw:aoip 첫 readi 직후 적용
#     (alsa hw_params/prepare 영향 없는 시점)
#   - RAVENNA cap/play 스레드 + RTP 송수신 스레드는 g_clk_ready 게이트로
#     I2S 베이스라인 보정 완료 후에 진입
#
# 본 스크립트는 install.sh 의 한 단계로 호출되거나 독립 실행 가능 (멱등).
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

INSTALL_DIR="$(cd "$(dirname "$0")" && pwd)"
MOD_SRC_DIR="${INSTALL_DIR}/clk-test"
MOD_NAME="ptp-i2s-sync"
KVER="$(uname -r)"
KMOD_EXTRA="/lib/modules/${KVER}/extra"

# =============================================================================
# 1. 커널 헤더 확인
# =============================================================================
section "1. 커널 빌드 의존성 확인"

KBUILD_DIR="/lib/modules/${KVER}/build"
if [ ! -d "${KBUILD_DIR}" ]; then
    warn "커널 헤더 (${KBUILD_DIR}) 없음 — 설치 시도"
    apt-get install -y "linux-headers-${KVER}" || error "linux-headers-${KVER} 설치 실패"
fi
info "커널: ${KVER}"

# =============================================================================
# 2. ptp-i2s-sync.ko 빌드
# =============================================================================
section "2. ptp-i2s-sync 커널 모듈 빌드"

if [ ! -f "${MOD_SRC_DIR}/${MOD_NAME}.c" ]; then
    error "${MOD_SRC_DIR}/${MOD_NAME}.c 없음"
fi
if [ ! -f "${MOD_SRC_DIR}/Makefile" ]; then
    error "${MOD_SRC_DIR}/Makefile 없음"
fi

info "빌드 중... (${MOD_SRC_DIR})"
make -C "${MOD_SRC_DIR}" clean >/dev/null 2>&1 || true
make -C "${MOD_SRC_DIR}"

if [ ! -f "${MOD_SRC_DIR}/${MOD_NAME}.ko" ]; then
    error "${MOD_NAME}.ko 빌드 실패"
fi
info "${MOD_NAME}.ko 빌드 완료"

# =============================================================================
# 3. .ko 설치 + depmod
# =============================================================================
section "3. 모듈 설치"

mkdir -p "${KMOD_EXTRA}"
cp "${MOD_SRC_DIR}/${MOD_NAME}.ko" "${KMOD_EXTRA}/"
depmod -a
info "${KMOD_EXTRA}/${MOD_NAME}.ko 설치 완료"

# =============================================================================
# 4. 부팅 시 자동 로드 설정
# =============================================================================
section "4. 부팅 자동 로드"

MODULES_LOAD_CONF="/etc/modules-load.d/${MOD_NAME}.conf"
echo "${MOD_NAME}" > "${MODULES_LOAD_CONF}"
info "${MODULES_LOAD_CONF} → ${MOD_NAME}"

# =============================================================================
# 5. systemd 서비스 (모듈 로드 + sysfs 권한)
# =============================================================================
section "5. systemd 서비스 설치"

SYSTEMD_SRC="${INSTALL_DIR}/systemd/${MOD_NAME}.service"
SYSTEMD_DST="/etc/systemd/system/${MOD_NAME}.service"

if [ ! -f "${SYSTEMD_SRC}" ]; then
    error "${SYSTEMD_SRC} 없음"
fi

cp "${SYSTEMD_SRC}" "${SYSTEMD_DST}"
systemctl daemon-reload
systemctl enable "${MOD_NAME}.service"
info "${MOD_NAME}.service 설치 + enable 완료"

# =============================================================================
# 6. 즉시 적용 (재부팅 없이)
# =============================================================================
section "6. 모듈 즉시 로드"

# 기존 인스턴스가 있으면 안전하게 언로드 후 재로드 (개발 환경 재실행 대비)
if lsmod | grep -q "^${MOD_NAME//-/_}"; then
    info "기존 ${MOD_NAME} 모듈 언로드"
    rmmod "${MOD_NAME//-/_}" 2>/dev/null || \
        warn "rmmod 실패 — 모듈이 사용 중일 수 있음 (재부팅 후 적용됨)"
fi

if modprobe "${MOD_NAME}"; then
    info "${MOD_NAME} 모듈 로드됨"
    # sysfs 권한 설정 (서비스가 다음 부팅부터 적용하지만 즉시도 함께 설정)
    if [ -e "/sys/kernel/ptp_i2s_sync/freq_ppb" ]; then
        chgrp audio /sys/kernel/ptp_i2s_sync/freq_ppb
        chmod 0664 /sys/kernel/ptp_i2s_sync/freq_ppb
        info "sysfs 권한 설정: group=audio mode=0664"
    fi
else
    warn "modprobe 실패 — I2S 디바이스(${MOD_NAME%-sync}=1f000a0000.i2s)가 없거나 커널 버전 불일치"
fi

# =============================================================================
# 완료
# =============================================================================
echo ""
echo "============================================="
echo -e " ${GREEN}clk-sync 설치 완료${NC}"
echo "============================================="
echo " 확인:"
echo "   lsmod | grep ptp_i2s_sync"
echo "   ls -l /sys/kernel/ptp_i2s_sync/"
echo "   cat /sys/kernel/ptp_i2s_sync/freq_ppb   # 초기 0"
echo "   cat /sys/kernel/ptp_i2s_sync/rate_hz    # 현재 I2S 주파수"
echo ""
echo " aoip_engine 시작 후 ~수 초 내 sysfs 가 24000 으로 갱신되고"
echo " hw:aoip ↔ RAVENNA drift 가 ±1ppm 내로 수렴해야 정상."
echo ""
