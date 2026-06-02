#!/bin/bash
# phc2sys 주파수-전용 모드 래퍼
#
# 목적: 외부 PTP 마스터가 wall time(UNIX epoch)을 운반하지 않을 때
#       PHC ↔ CLOCK_REALTIME 사이를 "주파수만" 동기화하고 절대시간 step은 금지.
#
# 핵심 개선:
#   ptp4l이 PHC를 마스터 시간으로 step 완료 후에만 -O offset을 계산.
#   offsetFromMaster가 1ms(1,000,000 ns) 이하로 수렴한 것을 확인 후 진행.
#   그렇지 않으면 -O 값이 틀려 phc2sys servo가 saturate(±10%)됨.

set -e

IFACE="${PHC2SYS_IFACE:-eth0}"
PHC_DEV="${PHC2SYS_PHC:-/dev/ptp0}"
PMC=/usr/sbin/pmc
LOCK_OFFSET_NS=1000000   # PHC가 마스터에 1ms 이내로 수렴해야 -O 계산 진행
WAIT_MAX=120             # 최대 대기 초

echo "phc2sys-freqonly: ptp4l PHC 안정화 대기 (offsetFromMaster < 1ms)..."

for i in $(seq 1 ${WAIT_MAX}); do
    OFFSET_NS=$(${PMC} -u -b 0 'GET CURRENT_DATA_SET' 2>/dev/null \
        | awk '/offsetFromMaster/{v=$2; if(v<0)v=-v; print v; exit}')

    if [ -n "${OFFSET_NS}" ] && [ "${OFFSET_NS}" -lt "${LOCK_OFFSET_NS}" ] 2>/dev/null; then
        echo "phc2sys-freqonly: ptp4l locked, offsetFromMaster=${OFFSET_NS}ns (${i}초 대기)"
        break
    fi

    if [ $i -eq ${WAIT_MAX} ]; then
        echo "phc2sys-freqonly: 경고 — ${WAIT_MAX}초 내 lock 실패, 현재 offset=${OFFSET_NS}ns 로 진행" >&2
    fi
    sleep 1
done

# PHC ↔ sys 오프셋을 sub-second 정밀도로 계산 (round → 잔차 ≤0.5s)
OFFSET=$(python3 - "${PHC_DEV}" <<'PYEOF'
import subprocess, time, sys, re

phc_dev = sys.argv[1]
out = subprocess.check_output(['/usr/sbin/phc_ctl', phc_dev, 'get'],
                              stderr=subprocess.STDOUT, text=True)
m = re.search(r'clock time is\s+([0-9.]+)', out)
if not m:
    print("ERROR: PHC 시각 파싱 실패", file=sys.stderr)
    sys.exit(1)

phc = float(m.group(1))
sys_t = time.time()
offset = round(sys_t - phc)
print(offset)
PYEOF
)

if [ -z "${OFFSET}" ]; then
    echo "phc2sys-freqonly: 오프셋 계산 실패" >&2
    exit 1
fi

echo "phc2sys-freqonly: offset=${OFFSET}s 적용 → phc2sys -E linreg -N 10 -F 0 -S 0"

exec /usr/sbin/phc2sys \
    -s "${IFACE}" \
    -c CLOCK_REALTIME \
    -O "${OFFSET}" \
    -F 0.0 \
    -S 0.0 \
    -E linreg \
    -N 10 \
    -R 1 \
    -m -u 1
