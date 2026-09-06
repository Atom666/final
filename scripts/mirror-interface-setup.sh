#!/bin/sh
set -eu

rx=0; nad=0
ip link show mirror-rx >/dev/null 2>&1 && rx=1
ip link show nad-mirror >/dev/null 2>&1 && nad=1

if [ "$rx" -eq 0 ] && [ "$nad" -eq 0 ]; then
    ip link add mirror-rx type veth peer name nad-mirror
elif [ "$rx" -ne "$nad" ]; then
    echo "mirror veth is incomplete; refusing to replace an existing interface" >&2
    exit 1
fi

ip -d link show mirror-rx | grep -q 'veth' || { echo "mirror-rx exists but is not veth" >&2; exit 1; }
ip -d link show nad-mirror | grep -q 'veth' || { echo "nad-mirror exists but is not veth" >&2; exit 1; }

ip link set mirror-rx mtu 9216
ip link set nad-mirror mtu 9216
ip link set mirror-rx up
ip link set nad-mirror up
