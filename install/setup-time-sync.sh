#!/bin/bash
# =============================================================================
# AoIP 시간 동기화 스택 설치/적용 (멱등 — 반복 실행 안전)
#
#   sudo bash install/setup-time-sync.sh
#
# 적용 항목:
#   1. fake-hwclock        — RTC 무배터리 대비: 매시간+종료 시 시간 저장, 부팅 복원
#                            (Pi5 프리셋이 부팅 복원 서비스를 mask하므로 unmask 필요)
#   2. chrony maxslewrate  — slave 모드 복귀 시 급슬루로 인한 피치 시프트 방지
#   3. ptp4l.conf          — step_threshold 1.0 (ARB GM의 큰 에포크 오프셋 스텝 허용)
#   4. aoip-clock-manager  — role-aware 클럭 통합 데몬 (SLAVE: freq 슬레이빙 /
#                            MASTER: phc2sys 역방향 / 부팅 게이트 + NTP 사후 교정)
#
# 배경: docs 또는 메모리의 "AES67 입력 주간 파손" 진단 참조.
# =============================================================================
set -e

if [ "$(id -u)" -ne 0 ]; then
    echo "root로 실행하세요: sudo bash $0" >&2
    exit 1
fi

REPO_DIR="$(cd "$(dirname "$0")/.." && pwd)"
info() { echo -e "\e[32m[time-sync]\e[0m $*"; }

# ── 1. fake-hwclock ─────────────────────────────────────────────────────────
if ! dpkg -s fake-hwclock >/dev/null 2>&1; then
    apt-get install -y fake-hwclock
    info "fake-hwclock 설치됨"
else
    info "fake-hwclock 이미 설치됨"
fi
# 부팅 복원 유닛은 fake-hwclock-load.service (fake-hwclock.service는
# 패키지가 의도적으로 mask해 둔 전환용 이름 — 착각 금지)
systemctl enable fake-hwclock-load.service >/dev/null 2>&1 || true
systemctl enable --now fake-hwclock-save.timer >/dev/null 2>&1 || true
fake-hwclock save 2>/dev/null || /sbin/fake-hwclock save
info "fake-hwclock 부팅복원 unmask+enable, 저장: $(cat /etc/fake-hwclock.data 2>/dev/null)"

# ── 2. chrony maxslewrate ───────────────────────────────────────────────────
if ! grep -q '^maxslewrate' /etc/chrony/chrony.conf; then
    echo 'maxslewrate 2' >> /etc/chrony/chrony.conf
    info "chrony.conf: maxslewrate 2 추가"
else
    info "chrony.conf: maxslewrate 이미 설정됨"
fi

# ── 3. ptp4l.conf ───────────────────────────────────────────────────────────
PTP4L_SRC="${REPO_DIR}/install/linuxptp/ptp4l.conf"
if [ -f "${PTP4L_SRC}" ]; then
    mkdir -p /etc/linuxptp
    if ! cmp -s "${PTP4L_SRC}" /etc/linuxptp/ptp4l.conf; then
        cp "${PTP4L_SRC}" /etc/linuxptp/ptp4l.conf
        info "ptp4l.conf 갱신 — ptp4l 재시작 (PHC 재스텝은 가드가 흡수)"
        systemctl try-restart ptp4l.service 2>/dev/null || true
    else
        info "ptp4l.conf 최신 상태"
    fi
fi

# ── 4. aoip-clock-manager ───────────────────────────────────────────────────
CM_SRC="${REPO_DIR}/scripts/tools/ptp-clock-manager.py"
CM_DST="/usr/local/sbin/ptp-clock-manager.py"
UNIT_SRC="${REPO_DIR}/install/systemd/aoip-clock-manager.service"
UNIT_DST="/etc/systemd/system/aoip-clock-manager.service"

CHANGED=0
if ! cmp -s "${CM_SRC}" "${CM_DST}"; then
    cp "${CM_SRC}" "${CM_DST}"; chmod +x "${CM_DST}"; CHANGED=1
fi
if ! cmp -s "${UNIT_SRC}" "${UNIT_DST}"; then
    cp "${UNIT_SRC}" "${UNIT_DST}"; systemctl daemon-reload; CHANGED=1
fi
systemctl enable aoip-clock-manager.service >/dev/null 2>&1
if [ "${CHANGED}" -eq 1 ] || ! systemctl is-active --quiet aoip-clock-manager.service; then
    systemctl restart aoip-clock-manager.service
    info "aoip-clock-manager 배포+재시작"
else
    info "aoip-clock-manager 최신 상태 (재시작 생략)"
fi

info "완료. 확인: journalctl -u aoip-clock-manager -f"
