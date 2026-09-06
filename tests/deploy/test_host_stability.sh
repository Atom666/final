#!/bin/sh
set -u

SCRIPT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)/scripts/nad-provision.sh"
TMP=$(mktemp -d)
trap 'rm -rf "$TMP"' EXIT

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

run_ok() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      STATE_DIR=$1
      NAD_HOST=$2
      mkdir -p "$STATE_DIR"
      check_nad_host_stable
      cat "$STATE_DIR/nad-host" )
}

run_err() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      STATE_DIR=$1
      NAD_HOST=$2
      mkdir -p "$STATE_DIR"
      check_nad_host_stable ) 2>&1 1>/dev/null
}

state1="$TMP/state1"
out=$(run_ok "$state1" "192.168.1.78")
assert_eq "first run records the host" "192.168.1.78" "$out"

out=$(run_ok "$state1" "192.168.1.78")
assert_eq "second run with same host is a no-op" "192.168.1.78" "$out"

err=$(run_err "$state1" "10.0.0.9")
assert_eq "mismatched host is rejected" "Stored nad-host (192.168.1.78) does not match --nad-host (10.0.0.9)." "$(printf '%s\n' "$err" | head -n1)"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
