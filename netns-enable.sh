#!/bin/bash -e

if [[ $EUID -ne 0 ]]; then
    echo "This script must be run as root (try: sudo NIC=eno1 $0 $*)" >&2
    exit 1
fi

NIC="${NIC:-enp3s0}"

echo "NIC: $NIC"

# Stop NetworkManager from managing the NIC (runtime only)
nmcli dev set $NIC managed no
ip link set $NIC down

# Create the namespace and disable IPv6 in it before the NIC arrives
ip netns add dlink
ip netns exec dlink sysctl -qw net.ipv6.conf.all.disable_ipv6=1 net.ipv6.conf.default.disable_ipv6=1

# Move the NIC in and configure it
ip link set $NIC netns dlink
ip -n dlink addr add 192.168.0.2/24 dev $NIC
ip -n dlink link set lo up
ip -n dlink link set $NIC up

echo "Done."
