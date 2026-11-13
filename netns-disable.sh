#!/bin/bash -e

if [[ $EUID -ne 0 ]]; then
    echo "This script must be run as root (try: sudo NIC=eno1 $0 $*)" >&2
    exit 1
fi

NIC="${NIC:-enp3s0}"

echo "NIC: $NIC"

ip netns del dlink | true                 # NIC returns to the main namespace
sleep 1
nmcli dev set $NIC managed yes

echo "Done."
