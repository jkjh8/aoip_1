#!/bin/bash
# Monitors eth0 carrier state and stops/starts network-dependent services accordingly.
# Prevents error-restart loops when Ethernet cable is disconnected.

IFACE="eth0"
CARRIER="/sys/class/net/$IFACE/carrier"
CHECK_INTERVAL=2
IP_WAIT_TIMEOUT=60

NETWORK_SERVICES_STOP=(
    "aoip"
    "aes67-daemon"
    "ptp-freq-sync"
    "phc2sys"
    "ptp4l"
)

NETWORK_SERVICES_START=(
    "ptp4l"
    "aes67-daemon"
    "aoip"
)

log() {
    logger -t aoip-network-monitor "$@"
    echo "$(date '+%Y-%m-%d %H:%M:%S') [aoip-network-monitor] $*"
}

get_carrier() {
    cat "$CARRIER" 2>/dev/null || echo "0"
}

wait_for_ip() {
    local count=0
    while [[ $count -lt $IP_WAIT_TIMEOUT ]]; do
        if ip addr show "$IFACE" 2>/dev/null | grep -q 'inet '; then
            return 0
        fi
        sleep 1
        ((count++))
    done
    return 1
}

stop_network_services() {
    # 링크 다운 시 서비스를 종료하지 않는다.
    # 엔진(aoip_engine)은 PTP holdover / ring zero-fill / rb_reset 으로 자체적으로
    # 무음 출력 + overflow 방지를 처리한다. aes67-daemon / ptp4l 도 링크 복귀 시 자동 재동기화.
    # 짧은 단절에서 stop/start 캐스케이드(수십초 다운타임) 회피.
    log "Link down on $IFACE — services left running (engine handles silence/holdover)"
}

start_network_services() {
    log "Link up on $IFACE — waiting for IP address..."
    if ! wait_for_ip; then
        log "ERROR: No IP address on $IFACE after ${IP_WAIT_TIMEOUT}s — services not started"
        return 1
    fi

    local ip
    ip=$(ip -4 addr show "$IFACE" | grep -oP '(?<=inet )\d+\.\d+\.\d+\.\d+')
    log "IP acquired: $ip — starting network services"

    # stop을 안 하므로 보통 이미 동작 중. 미동작 서비스만 기동(콜드부트 대비).
    for svc in "${NETWORK_SERVICES_START[@]}"; do
        if systemctl is-active --quiet "${svc}.service"; then
            log "${svc}.service already running — skip"
            continue
        fi
        log "Starting ${svc}.service"
        systemctl start "${svc}.service"
        case "$svc" in
            ptp4l)        sleep 3 ;;
            aes67-daemon) sleep 5 ;;
        esac
    done

    log "All network services started"
}

# Wait for carrier file to appear (interface may not exist yet on boot)
while [[ ! -f "$CARRIER" ]]; do
    sleep 1
done

last_state=$(get_carrier)
log "Started — $IFACE carrier: $last_state"

while true; do
    sleep "$CHECK_INTERVAL"

    current_state=$(get_carrier)

    if [[ "$current_state" != "$last_state" ]]; then
        log "Carrier state changed: $last_state → $current_state"
        if [[ "$current_state" == "1" ]]; then
            sleep 1  # debounce — wait for link to stabilize
            start_network_services
        else
            stop_network_services
        fi
        last_state="$current_state"
    fi
done
