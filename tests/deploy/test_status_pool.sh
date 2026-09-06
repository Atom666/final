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

STATE_DIR="$TMP/state"
mkdir -p "$STATE_DIR/students"
printf '1\tuuid-1\tfree\t2026-01-01T00:00:00Z\n' > "$STATE_DIR/students/students.tsv"
printf '2\tuuid-2\toccupied\t2026-01-01T00:00:01Z\n' >> "$STATE_DIR/students/students.tsv"
printf '3\tuuid-3\tfree\t2026-01-01T00:00:02Z\n' >> "$STATE_DIR/students/students.tsv"

run() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      STATE_DIR="$TMP/state"
      "$@" )
}

out=$(run status_pool students)
assert_eq "lists every slot" "3" "$(printf '%s\n' "$out" | grep -c '^[0-9]')"
assert_eq "includes uuid-2 as occupied" "1" "$(printf '%s\n' "$out" | grep -c 'uuid-2.*occupied')"
assert_eq "summarizes free count" "1" "$(printf '%s\n' "$out" | grep -c 'free: 2, occupied: 1')"

err=$(run status_pool bogus 2>&1 1>/dev/null)
assert_eq "unseeded pool is an error" "1" "$(printf '%s\n' "$err" | grep -c 'has not been seeded')"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
