#!/bin/sh
#
# Take the RNDIS interface down for a while and bring it back.
#
# The whole cycle runs on a timer inside this script, detached, so that
# recovery does not depend on anything off the board. If the outage takes the
# adb link down with it, the interface still comes back by itself after the
# configured delay -- which matters, because the only other way to recover a
# lost adb link is to physically re-plug the board.
#
# Uses `ip link set dev IFACE down|up`, not `ifconfig IFACE down`: on this
# board (busybox 1.36.1) the ifconfig form returned 0 and left the interface
# reachable, while the ip form actually clears the UP flag.
#
# The state check is the interface flags and operstate, not the address list.
# `ip link down` deliberately keeps IPv4 addresses in the kernel, so "the
# address is still there" says nothing about whether the link went down.
#
# Usage: netdrop.sh [down-seconds] [interface]
#
# Everything goes to /tmp/netdrop.log so the caller can read it after recovery.

IFACE="${2:-usb0}"
DOWN_SECONDS="${1:-20}"
ARM_SECONDS=3
LOG=/tmp/netdrop.log

log() { echo "$(date '+%H:%M:%S') $1" >> "$LOG"; }

flags_line() {
    ip addr show "$IFACE" 2>/dev/null | sed -n '1p' | sed 's/^[0-9]*: //' | cut -c1-60
}

snapshot() {
    log "  [$1] operstate=$(cat "/sys/class/net/$IFACE/operstate" 2>/dev/null) flags=$(flags_line)"
}

: > "$LOG"

# Save the addresses so they can be re-applied if the down clears them.
ADDRS=$(ip addr show "$IFACE" 2>/dev/null | sed -n 's/.*inet \([0-9.]*\)\/\([0-9]*\).*/\1\/\2/p')

log "armed: iface=$IFACE down=${DOWN_SECONDS}s addresses=[$ADDRS]"
log "gateway pid before: $(pidof v4l2_mpp_encode)"
snapshot "before"
log "route table before:"
ip route show 2>/dev/null | while read -r line; do log "  $line"; done

# Give the caller a moment to see the launch succeed before the link goes away.
sleep "$ARM_SECONDS"

log "bringing $IFACE down now"
ip link set dev "$IFACE" down
log "  ip link set down returned $?"
snapshot "after down"

sleep "$DOWN_SECONDS"

log "bringing $IFACE back up"
ip link set dev "$IFACE" up
log "  ip link set up returned $?"
sleep 2
snapshot "after up"

# Re-apply any address the down actually removed. The kernel normally keeps
# them, but if it did not, the board would be up with no way to reach it.
for a in $ADDRS; do
    if ip addr show "$IFACE" 2>/dev/null | grep -q "inet $a"; then
        log "  address $a survived"
    else
        ip addr add "$a" dev "$IFACE"
        log "  address $a re-applied, rc=$?"
    fi
done

# Only if the down took the default route with it. Adding unconditionally put
# a second default route in the table last time.
if ip route show 2>/dev/null | grep -q '^default'; then
    log "  default route survived"
else
    ip route add default dev "$IFACE"
    log "  default route re-applied, rc=$?"
fi

sleep 1
snapshot "final"
log "route table after:"
ip route show 2>/dev/null | while read -r line; do log "  $line"; done
log "gateway pid after: $(pidof v4l2_mpp_encode)"
log "done"
