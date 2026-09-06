#!/bin/sh
set -u

TEMPLATE="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)/scripts/templates/agent-bootstrap.sh.tmpl"

pass=0
fail=0

assert_eq() {
    if [ "$2" = "$3" ]; then
        printf 'ok   - %s\n' "$1"
        pass=$((pass + 1))
    else
        printf 'FAIL - %s\n    expected: %s\n    actual:   %s\n' "$1" "$2" "$3"
        fail=$((fail + 1))
    fi
}

run() {
    ( MIRROR_BOOTSTRAP_SOURCE=1
      . "$TEMPLATE"
      "$@" )
}

route_line="default via 192.168.1.1 dev ens33 proto dhcp metric 100"
assert_eq "extracts interface from default route" "ens33" "$(run capture_iface_from_route_output "$route_line")"

route_line2="default via 10.0.0.1 dev eth0"
assert_eq "extracts interface from minimal default route" "eth0" "$(run capture_iface_from_route_output "$route_line2")"

assert_eq "empty route output yields empty interface" "" "$(run capture_iface_from_route_output "")"

non_default="10.0.0.0/24 dev eth1 proto kernel scope link src 10.0.0.5"
assert_eq "ignores non-default routes" "" "$(run capture_iface_from_route_output "$non_default")"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
