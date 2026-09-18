#!/bin/bash
# ─────────────────────────────────────────────────────────────────────────────
# RAVENNA ALSA 커널모듈 REF_UNIT 패치 설치 (2026-09-17)
#
#   원본 드라이버는 PTP/TIC 시각을 100us 단위(REF_UNIT)로 절삭한다.
#   이 패치는 그것을 1us 로 낮춰 TIC 위상 양자화를 4.8프레임 → 0.05프레임으로
#   줄인다. 실측: 게이트 max jump 9.8~14.3fr → 1.74fr.
#
#   사용법:
#     sudo ./install.sh            패치본 설치 + 검증 (실패 시 자동 복구)
#     sudo ./install.sh --debug    계측(printk) 포함 빌드 설치 — 진단용
#     sudo ./install.sh --revert   원본 모듈로 복구 (빌드 없이 .ko 교체)
#
#   주의: aoip.service 는 Requires=aes67-daemon.service 라 데몬을 내리면
#         엔진도 같이 내려간다. 교체 중 오디오가 ~20초 끊긴다.
# ─────────────────────────────────────────────────────────────────────────────
set -u
HERE="$(cd "$(dirname "$0")" && pwd)"
SRCDIR="${RAVENNA_SRC:-/home/kjh/aes67-linux-daemon/3rdparty/ravenna-alsa-lkm/driver}"
DST="/lib/modules/$(uname -r)/kernel/extra/MergingRavennaALSA.ko"
MODE="${1:-}"

[ "$(id -u)" -eq 0 ] || { echo "root 권한 필요: sudo $0 $MODE"; exit 1; }
[ -d "$SRCDIR" ] || { echo "드라이버 소스 없음: $SRCDIR  (RAVENNA_SRC 로 지정 가능)"; exit 1; }

stop_services() {
  systemctl stop aoip 2>/dev/null
  systemctl stop aes67-daemon 2>/dev/null
  sleep 2
  pkill -9 -f aoip_engine 2>/dev/null
  sleep 1
  lsmod | grep -q MergingRavennaALSA && rmmod MergingRavennaALSA
  return 0
}
start_services() {
  systemctl start aes67-daemon; sleep 4
  systemctl start aoip;         sleep 6
}
install_ko() {   # $1 = .ko 경로
  cp "$1" "$DST" && depmod -a
}

if [ "$MODE" = "--revert" ]; then
  echo "[revert] 원본 모듈로 복구"
  stop_services
  install_ko "$HERE/MergingRavennaALSA.ko.orig"
  start_services
  echo "[revert] 완료: $(curl -s --max-time 5 http://127.0.0.1:8080/api/ptp/status)"
  exit 0
fi

case "$MODE" in
  --debug) SRC="$HERE/PTP.c.patched-debug"; echo "[1] 계측 포함 빌드 (dmesg 에 TICSTAT/PLLSTAT)" ;;
  "")      SRC="$HERE/PTP.c.patched";       echo "[1] 패치본 빌드" ;;
  *)       echo "알 수 없는 옵션: $MODE"; exit 1 ;;
esac

# 최초 1회 원본 보존 (이미 있으면 덮지 않는다)
[ -f "$SRCDIR/PTP.c.upstream" ] || cp "$SRCDIR/PTP.c" "$SRCDIR/PTP.c.upstream"

cp "$SRC" "$SRCDIR/PTP.c"
( cd "$SRCDIR" && make modules ) || { echo "빌드 실패"; exit 1; }

echo "[2] 모듈 교체"
T0=$(date '+%Y-%m-%d %H:%M:%S')
stop_services
install_ko "$SRCDIR/MergingRavennaALSA.ko"
start_services

# ── 검증: PTP 락만 보면 안 된다. 게이트 통과까지 확인할 것 ──────────────
#    (2026-09-17: PTP locked 인데 TIC 이 무너져 18분 무음을 낸 이력)
echo "[3] 검증 — 게이트 통과 대기 (최대 180초)"
PASS=0
for _ in $(seq 1 36); do
  journalctl -u aoip --since "$T0" --no-pager 2>/dev/null | grep -q "clock verified" && { PASS=1; break; }
  sleep 5
done

if [ "$PASS" -ne 1 ]; then
  echo "[!] 게이트 미통과 → 원본으로 자동 복구"
  journalctl -u aoip --since "$T0" --no-pager | grep -i "unstable" | tail -3
  stop_services; install_ko "$HERE/MergingRavennaALSA.ko.orig"; start_services
  exit 2
fi

echo "[3] 언뮤트 확인, 120초 안정성 관찰"
T1=$(date '+%Y-%m-%d %H:%M:%S'); sleep 120
BAD=$(journalctl -u aoip --since "$T1" --no-pager | grep -c "clock unstable")
if [ "$BAD" -gt 0 ]; then
  echo "[!] clock unstable ${BAD}회 → 원본으로 자동 복구"
  stop_services; install_ko "$HERE/MergingRavennaALSA.ko.orig"; start_services
  exit 2
fi

echo "[4] 설치 완료"
curl -s --max-time 5 http://127.0.0.1:8080/api/ptp/status; echo
journalctl -u aoip --since "$T0" --no-pager | grep -E "clock verified|live clock 600s" | tail -3
echo "  * 10분 뒤 'live clock 600s ... max jump' 확인 권장 (원본 9.8~14.3fr → 패치 1.7fr)"
