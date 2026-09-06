#!/bin/sh
set -u

REPO_ROOT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)"
SCRIPT="$REPO_ROOT/scripts/nad-provision.sh"
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
tsv="$STATE_DIR/students/students.tsv"
printf '1\tuuid-1\tfree\t2026-01-01T00:00:00Z\n' > "$tsv"
printf '2\tuuid-2\tfree\t2026-01-01T00:00:00Z\n' >> "$tsv"
printf '3\tuuid-3\tfree\t2026-01-01T00:00:00Z\n' >> "$tsv"

run() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      STATE_DIR="$TMP/state"
      "$@" )
}

out=$(run occupy_pool students 1-2)
status=$?
assert_eq "occupy_pool exits 0 when all slots apply" "0" "$status"
assert_eq "occupy_pool reports each slot" "2" "$(printf '%s\n' "$out" | grep -c '^OK   - slot')"
assert_eq "slot 1 becomes occupied" "occupied" "$(awk -F'\t' '$1 == 1 { print $3 }' "$tsv")"
assert_eq "slot 2 becomes occupied" "occupied" "$(awk -F'\t' '$1 == 2 { print $3 }' "$tsv")"
assert_eq "slot 3 stays free" "free" "$(awk -F'\t' '$1 == 3 { print $3 }' "$tsv")"

err=$(run occupy_pool students 1 2>&1 1>/dev/null)
status=$?
assert_eq "re-occupying an occupied slot fails" "1" "$status"
assert_eq "re-occupying an occupied slot reports the conflict" "1" "$(printf '%s\n' "$err" | grep -c 'slot 1 is already occupied')"

out=$(run release_pool students 1)
assert_eq "release_pool frees slot 1" "free" "$(awk -F'\t' '$1 == 1 { print $3 }' "$tsv")"
assert_eq "slot 2 is unaffected by releasing slot 1" "occupied" "$(awk -F'\t' '$1 == 2 { print $3 }' "$tsv")"

err=$(run release_pool students 3 2>&1 1>/dev/null)
status=$?
assert_eq "releasing an already-free slot fails" "1" "$status"

err=$(run occupy_pool students 99 2>&1 1>/dev/null)
assert_eq "occupying a nonexistent slot reports it" "1" "$(printf '%s\n' "$err" | grep -c 'slot 99 does not exist')"

out=$(run occupy_pool students 2-3)
assert_eq "a mixed-validity range still applies the valid slots" "occupied" "$(awk -F'\t' '$1 == 3 { print $3 }' "$tsv")"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
