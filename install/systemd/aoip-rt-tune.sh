#!/bin/bash
echo HRTICK > /sys/kernel/debug/sched/features
echo HRTICK_DL > /sys/kernel/debug/sched/features

IRQ=$(grep -m1 "eth0" /proc/interrupts | cut -d: -f1 | tr -d " ")
DMA_IRQ=$(grep -m1 "dw_axi_dma" /proc/interrupts | cut -d: -f1 | tr -d " ")

# eth0 IRQ → CPU1, DMA IRQ → CPU2
[ -n "$IRQ" ] && echo 2 > /proc/irq/$IRQ/smp_affinity
[ -n "$DMA_IRQ" ] && echo 4 > /proc/irq/$DMA_IRQ/smp_affinity

sleep 5

for pid in $(pgrep ktimers); do chrt -f -p 96 $pid; done && echo "ktimers → FF96"

ETH_TID=$(ps -eLo lwp,comm | awk "/irq\/${IRQ}-/{print \$1}")
if [ -n "$ETH_TID" ]; then
    chrt -f -p 49 $ETH_TID
    taskset -cp 1 $ETH_TID
    echo "irq/eth0 → FF49, CPU1"
fi

DMA_TID=$(ps -eLo lwp,comm | awk "/irq\/${DMA_IRQ}-/{print \$1}")
if [ -n "$DMA_TID" ]; then
    chrt -f -p 48 $DMA_TID
    taskset -cp 2 $DMA_TID
    echo "irq/dma → FF48, CPU2"
fi

for pid in $(pgrep ptp4l); do
    chrt -f -p 47 $pid
    taskset -cp 1 $pid
done && echo "ptp4l → FF47, CPU1"

for pid in $(pgrep -x aes67-daemon); do
  for tid in $(ls /proc/$pid/task/ 2>/dev/null); do
    chrt -f -p 46 $tid
    taskset -cp 1 $tid
  done
done && echo "aes67-daemon → FF46, CPU1"

for pid in $(pgrep -x aoip_engine); do
  for tid in $(ls /proc/$pid/task/ 2>/dev/null); do chrt -f -p 45 $tid; done
done && echo "aoip_engine → FF45"
