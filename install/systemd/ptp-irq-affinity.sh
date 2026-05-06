#!/bin/bash
# eth0 IRQ를 CPU 3에 고정, RT 우선순위 설정 (RT 커널 PTP 안정화)
# CPU 레이아웃: CPU2=DSP/ALSA/rtp_recv, CPU3=eth0 IRQ/ptp4l/rtp_send
#
# 우선순위 설계 (linuxptp 권장 기준):
#   FIFO 85 : eth0 IRQ, PHC IRQ  — 패킷/타임스탬프 수신
#   FIFO 80 : ptp4l
#   FIFO 75 : aes67-daemon
#   FIFO 70 : backlog_napi

set_irq() {
    local irq=$1 prio=$2 cpu_mask=$3
    [ -z "$irq" ] && return
    echo "$cpu_mask" > /proc/irq/$irq/smp_affinity 2>/dev/null

    local pid
    pid=$(cat /proc/irq/$irq/irq_thread_pid 2>/dev/null)
    [ -z "$pid" ] && pid=$(ps -eo pid,comm | awk -v n="irq/$irq" '$2 ~ n {print $1}' | head -1)
    if [ -n "$pid" ]; then
        chrt -f -p "$prio" "$pid" 2>/dev/null && \
            echo "[ptp-irq] IRQ $irq PID $pid → CPU mask 0x$cpu_mask FIFO $prio" || \
            echo "[ptp-irq] IRQ $irq PID $pid 우선순위 설정 실패"
    else
        echo "[ptp-irq] IRQ $irq: 스레드 없음 (affinity만 적용)"
    fi
}

# eth0 IRQ → CPU3(0x8), FIFO 85
ETH_IRQ=$(grep -m1 'eth0' /proc/interrupts | awk -F: '{print $1}' | tr -d ' ')
set_irq "$ETH_IRQ" 85 8

# PHC IRQ → CPU3(0x8), FIFO 85
PHC_IRQ=$(grep -m1 '1f00100000.ethernet' /proc/interrupts | awk -F: '{print $1}' | tr -d ' ')
set_irq "$PHC_IRQ" 85 8

# backlog_napi → FIFO 70
for pid in $(pgrep -f 'backlog_napi' 2>/dev/null); do
    chrt -f -p 70 "$pid" 2>/dev/null
done
