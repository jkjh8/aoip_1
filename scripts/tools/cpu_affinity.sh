#!/usr/bin/env bash
# cpu_affinity.sh — AoIP CPU 배치 적용 / 복구
#
# 사용법:
#   cpu_affinity.sh apply    — 배치 적용 (현재 상태 저장)
#   cpu_affinity.sh restore  — 원상복구
#   cpu_affinity.sh status   — 현재 상태 확인
#   cpu_affinity.sh check    — cmdline.txt 격리 설정 확인
#
# 목표 레이아웃:
#   CPU0  : OS / IRQ 기본 / node                [비격리]
#   CPU1  : GENET IRQ + aes67-daemon (PTP)       [격리]
#   CPU2  : aoip_engine + rtp_recv               [격리]
#   CPU3  : rtp_send                             [격리]

set -euo pipefail

RESTORE_FILE="/var/tmp/cpu_affinity_restore.sh"

# ── 색상 ──────────────────────────────────────────────────
RED='\033[0;31m'; YEL='\033[1;33m'; GRN='\033[0;32m'; NC='\033[0m'
info()  { echo -e "${GRN}[affinity]${NC} $*"; }
warn()  { echo -e "${YEL}[affinity]${NC} $*"; }
error() { echo -e "${RED}[affinity]${NC} $*" >&2; }

# ── GENET IRQ 번호 목록 탐색 (복수) ─────────────────────
find_genet_irqs() {
  grep -iE "eth0|genet|bcmgenet" /proc/interrupts 2>/dev/null \
    | awk '{print $1}' | tr -d ':'
}

# ── 프로세스 CPU 배치 (없으면 skip) ──────────────────────
pin_proc() {
  local cpu=$1; shift
  local pids
  pids=$(pgrep -f "$*" 2>/dev/null || true)
  if [[ -z "$pids" ]]; then
    warn "  프로세스 없음 (skip): $*"
    return 0
  fi
  for pid in $pids; do
    taskset -cp "$cpu" "$pid" 2>/dev/null && \
      info "  PID $pid ($*) → CPU$cpu" || \
      warn "  PID $pid 배치 실패 (권한 확인): $*"
  done
}

# ── cmdline.txt 격리 설정 확인 ────────────────────────────
check_isolation() {
  local isolated
  isolated=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo "없음")
  echo ""
  echo "══════════════════════════════════════════════"
  echo " CPU 격리 상태 확인"
  echo "══════════════════════════════════════════════"
  echo " 현재 격리된 CPU : $isolated"
  echo " 목표            : 1-3"
  echo ""

  if [[ "$isolated" == "1-3" ]]; then
    info "격리 설정 완료 (재부팅 불필요)"
    return 0
  fi

  warn "격리가 적용되지 않았습니다."
  echo ""
  echo " /boot/firmware/cmdline.txt 수정이 필요합니다."
  echo ""
  echo " ┌─ 현재 내용 ────────────────────────────────"
  cat /boot/firmware/cmdline.txt 2>/dev/null | sed 's/^/ │ /' || echo " │ (파일 없음)"
  echo " └────────────────────────────────────────────"
  echo ""
  echo " 추가할 파라미터:"
  echo "   isolcpus=1,2,3 nohz_full=1,2,3 rcu_nocbs=1,2,3"
  echo ""
  read -rp " 지금 자동으로 수정하고 재부팅할까요? [y/N] " ans
  if [[ "${ans,,}" == "y" ]]; then
    local current
    current=$(cat /boot/firmware/cmdline.txt)
    # 이미 일부 파라미터가 있으면 제거 후 재추가
    current=$(echo "$current" | sed -E 's/isolcpus=[^ ]*//g; s/nohz_full=[^ ]*//g; s/rcu_nocbs=[^ ]*//g; s/  +/ /g; s/^ | $//g')
    echo "$current isolcpus=1,2,3 nohz_full=1,2,3 rcu_nocbs=1,2,3" \
      | sudo tee /boot/firmware/cmdline.txt > /dev/null
    info "cmdline.txt 수정 완료."
    cat /boot/firmware/cmdline.txt | sed 's/^/ → /'
    echo ""
    read -rp " 지금 재부팅할까요? [y/N] " rb
    [[ "${rb,,}" == "y" ]] && sudo reboot || warn "재부팅 후 격리가 적용됩니다."
  else
    warn "수동으로 수정 후 재부팅하세요."
  fi
}

# ── apply ─────────────────────────────────────────────────
do_apply() {
  if [[ $EUID -ne 0 ]]; then
    error "root 권한이 필요합니다. sudo $0 apply 로 실행하세요."
    exit 1
  fi

  info "현재 상태 저장 → $RESTORE_FILE"
  echo "#!/usr/bin/env bash" > "$RESTORE_FILE"
  echo "# 자동 생성된 복구 스크립트 — $(date)" >> "$RESTORE_FILE"
  chmod +x "$RESTORE_FILE"

  # ── GENET IRQ 배치 (복수 처리) ──────────────────────────
  local irqs
  irqs=$(find_genet_irqs)
  if [[ -n "$irqs" ]]; then
    while IFS= read -r irq; do
      local orig_aff
      orig_aff=$(cat "/proc/irq/$irq/smp_affinity" 2>/dev/null || echo "f")
      echo "echo $orig_aff | tee /proc/irq/$irq/smp_affinity > /dev/null" >> "$RESTORE_FILE"
      info "GENET IRQ $irq → CPU1 (affinity=0x2)"
      echo 2 | tee "/proc/irq/$irq/smp_affinity" > /dev/null
    done <<< "$irqs"
  else
    warn "GENET IRQ를 찾을 수 없습니다 (eth0/genet 확인)"
  fi

  # ── 프로세스 배치 ──────────────────────────────────────
  echo "# 프로세스 원상복구 (기본: 전체 CPU)" >> "$RESTORE_FILE"
  for name in aes67-daemon aoip_engine rtp_recv rtp_send; do
    local pids
    pids=$(pgrep -f "$name" 2>/dev/null || true)
    for pid in $pids; do
      local orig_cpu
      orig_cpu=$(taskset -cp "$pid" 2>/dev/null | grep -oP '[\d,\-]+$' || echo "0-3")
      echo "taskset -cp $orig_cpu $pid 2>/dev/null || true" >> "$RESTORE_FILE"
    done
  done

  echo ""
  info "프로세스 CPU 배치 적용..."
  pin_proc 1 "aes67-daemon"
  pin_proc 2 "aoip_engine"
  pin_proc 2 "rtp_recv"
  pin_proc 3 "rtp_send"
  pin_proc 0 "node"

  # ── 실시간 우선순위 보조 설정 ──────────────────────────
  local aes_pid
  aes_pid=$(pgrep aes67-daemon 2>/dev/null | head -1 || true)
  if [[ -n "$aes_pid" ]]; then
    chrt -f -p 80 "$aes_pid" 2>/dev/null && info "  aes67-daemon SCHED_FIFO 80 적용" || \
      warn "  aes67-daemon 우선순위 설정 실패"
  fi

  echo ""
  info "완료. 복구: sudo $0 restore"
  do_status
}

# ── restore ───────────────────────────────────────────────
do_restore() {
  if [[ $EUID -ne 0 ]]; then
    error "root 권한이 필요합니다. sudo $0 restore 로 실행하세요."
    exit 1
  fi

  if [[ ! -f "$RESTORE_FILE" ]]; then
    error "복구 파일이 없습니다: $RESTORE_FILE"
    error "apply를 먼저 실행하거나, 재부팅하면 자동 초기화됩니다."
    exit 1
  fi

  info "원상복구 실행..."
  bash "$RESTORE_FILE"
  rm -f "$RESTORE_FILE"
  info "복구 완료."
  do_status
}

# ── status ────────────────────────────────────────────────
do_status() {
  local isolated
  isolated=$(cat /sys/devices/system/cpu/isolated 2>/dev/null || echo "없음")

  echo ""
  echo "══════════════════════════════════════════════"
  echo " CPU Affinity 현황"
  echo "══════════════════════════════════════════════"
  printf " %-20s : %s\n" "격리된 CPU" "$isolated"

  local irqs
  irqs=$(find_genet_irqs)
  if [[ -n "$irqs" ]]; then
    while IFS= read -r irq; do
      local aff
      aff=$(cat "/proc/irq/$irq/smp_affinity" 2>/dev/null || echo "?")
      printf " %-20s : IRQ %-4s affinity=0x%s\n" "GENET(eth0)" "$irq" "$aff"
    done <<< "$irqs"
  fi

  echo ""
  printf " %-20s %6s %5s\n" "프로세스" "PID" "CPU"
  echo " ────────────────────────────────────────────"
  for name in aes67-daemon aoip_engine rtp_recv rtp_send node; do
    local pids
    pids=$(pgrep -f "$name" 2>/dev/null || true)
    if [[ -z "$pids" ]]; then
      printf " %-20s %6s %5s\n" "$name" "-" "-"
    else
      for pid in $pids; do
        local cpu
        cpu=$(taskset -cp "$pid" 2>/dev/null | grep -oP '[\d,\-]+$' || echo "?")
        printf " %-20s %6s %5s\n" "$name" "$pid" "$cpu"
      done
    fi
  done
  echo "══════════════════════════════════════════════"
  echo ""

  if [[ -f "$RESTORE_FILE" ]]; then
    info "복구 파일 있음: $RESTORE_FILE (sudo $0 restore 로 복구)"
  fi
}

# ── 진입점 ────────────────────────────────────────────────
case "${1:-}" in
  apply)   do_apply   ;;
  restore) do_restore ;;
  status)  do_status  ;;
  check)   check_isolation ;;
  *)
    echo "사용법: $0 {apply|restore|status|check}"
    echo ""
    echo "  apply    배치 적용 (현재 상태 저장)"
    echo "  restore  원상복구"
    echo "  status   현재 상태 확인"
    echo "  check    cmdline.txt 격리 설정 확인 (재부팅 필요 여부)"
    exit 1
    ;;
esac
