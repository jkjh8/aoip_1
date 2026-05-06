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
    log "Link down on $IFACE — stopping network services"
    for svc in "${NETWORK_SERVICES_STOP[@]}"; do
        if systemctl is-active --quiet "${svc}.service"; then
            log "Stopping ${svc}.service"
            systemctl stop "${svc}.service"
        fi
    done
    log "All network services stopped"
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

    for svc in "${NETWORK_SERVICES_START[@]}"; do
        log "Starting ${svc}.service"
        systemctl start "${svc}.service"
        # Give each service time to initialize before starting the next
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
