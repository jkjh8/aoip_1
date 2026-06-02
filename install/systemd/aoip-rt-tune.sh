#!/bin/bash
# AoIP RT 통합 튜닝
# CPU 레이아웃: CPU0=OS/Node, CPU1=eth0 IRQ/ptp4l/aes67, CPU2=DMA/DSP/ALSA, CPU3=rtp_send
#
# 우선순위 설계 (linuxptp 권장 기준, 높을수록 우선):
#   FIFO 96 : ktimers
#   FIFO 85 : eth0 IRQ, PHC IRQ        — 패킷/타임스탬프 수신
#   FIFO 84 : DMA IRQ (I2S)            — 오디오 캡처
#   FIFO 80 : ptp4l
#   FIFO 75 : aes67-daemon
#   FIFO 70 : backlog_napi
#   FIFO 59/58/57/56 : aoip_engine 내부 (DSP/ALSA/RAVENNA/RTP) — C 코드가 자체 설정, 건드리지 않음

echo HRTICK    > /sys/kernel/debug/sched/features
echo HRTICK_DL > /sys/kernel/debug/sched/features

# CPU governor → performance (PTP jitter 감소 핵심)
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
    [ -w "$g" ] && echo performance > "$g"
done

# eth0 NIC 튜닝: IRQ coalesce 최소화 + 링버퍼 최대
ethtool -C eth0 rx-usecs 0 tx-usecs 0 2>/dev/null \
  || ethtool -C eth0 rx-usecs 1 tx-usecs 1 2>/dev/null \
  || true
ethtool -G eth0 rx 4096 tx 4096 2>/dev/null || true

ETH_IRQ=$(grep -m1 'eth0'                /proc/interrupts | awk -F: '{print $1}' | tr -d ' ')
PHC_IRQ=$(grep -m1 '1f00100000.ethernet' /proc/interrupts | awk -F: '{print $1}' | tr -d ' ')
DMA_IRQ=$(grep -m1 'dw_axi_dma'          /proc/interrupts | awk -F: '{print $1}' | tr -d ' ')

# IRQ affinity (CPU mask: CPU1=0x2, CPU2=0x4)
[ -n "$ETH_IRQ" ] && echo 2 > /proc/irq/$ETH_IRQ/smp_affinity
[ -n "$PHC_IRQ" ] && echo 2 > /proc/irq/$PHC_IRQ/smp_affinity
[ -n "$DMA_IRQ" ] && echo 4 > /proc/irq/$DMA_IRQ/smp_affinity

sleep 5

# ktimers → FIFO 96
for pid in $(pgrep ktimers); do chrt -f -p 96 $pid; done
echo "[rt-tune] ktimers → FF96"

# IRQ thread 우선순위 + CPU pin
set_irq_thread() {
    local irq=$1 prio=$2 cpu=$3 label=$4
    [ -z "$irq" ] && return
    local tid
    tid=$(ps -eLo lwp,comm | awk -v n="irq/${irq}-" '$2 ~ n {print $1; exit}')
    if [ -n "$tid" ]; then
        chrt -f -p "$prio" "$tid" 2>/dev/null
        taskset -cp "$cpu" "$tid" >/dev/null 2>&1
        echo "[rt-tune] ${label} (irq/${irq} tid ${tid}) → FF${prio}, CPU${cpu}"
    fi
}

set_irq_thread "$ETH_IRQ" 85 1 "eth0 IRQ"
set_irq_thread "$PHC_IRQ" 85 1 "PHC IRQ"
set_irq_thread "$DMA_IRQ" 84 2 "DMA IRQ"

# ptp4l → FIFO 80, CPU1
for pid in $(pgrep ptp4l); do
    chrt -f -p 80 $pid
    taskset -cp 1 $pid >/dev/null 2>&1
done
echo "[rt-tune] ptp4l → FF80, CPU1"

# aes67-daemon → FIFO 75, CPU1
for pid in $(pgrep -x aes67-daemon); do
    for tid in $(ls /proc/$pid/task/ 2>/dev/null); do
        chrt -f -p 75 $tid 2>/dev/null
        taskset -cp 1 $tid >/dev/null 2>&1
    done
done
echo "[rt-tune] aes67-daemon → FF75, CPU1"

# backlog_napi → FIFO 70
for pid in $(pgrep -f 'backlog_napi' 2>/dev/null); do
    chrt -f -p 70 "$pid" 2>/dev/null
done

# NOTE: aoip_engine 은 C 코드가 per-thread 로 DSP=59/ALSA=58/RAVENNA=57/RTP=56 설정.
#       여기서 flatten 하지 않음 (이전 FIFO45 일괄 설정 제거됨).
