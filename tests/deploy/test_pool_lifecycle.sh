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
mkdir -p "$STATE_DIR"

run() {
    ( MIRROR_PROVISION_SOURCE=1
      MIRROR_PROVISION_SCRIPT_DIR="$REPO_ROOT/scripts"
      . "$SCRIPT"
      STATE_DIR="$TMP/state"
      "$@" )
}

# Exercises the real seed -> occupy -> status -> export -> release -> occupy -> status
# lifecycle through the actual functions (no fixtures, no fabricated TSV rows). This is
# the path that hid the unbound NAD_HOST bug: seed_pool must read the host back from the
# nad-host file, exactly like the CLI's `init` then `seed` would leave it.
run ensure_ca >/dev/null
printf '192.168.1.78\n' > "$STATE_DIR/nad-host"

out=$(run seed_pool students 3)
tsv="$STATE_DIR/students/students.tsv"
assert_eq "seed_pool creates 3 slots" "3" "$(wc -l < "$tsv" | tr -d ' ')"
assert_eq "seed_pool reports success" "1" "$(printf '%s\n' "$out" | grep -c 'Seeded pool students with 3 slot')"

out=$(run occupy_pool students 1-2)
status=$?
assert_eq "occupy_pool 1-2 succeeds" "0" "$status"
assert_eq "slot 1 is occupied" "occupied" "$(awk -F'\t' '$1 == 1 { print $3 }' "$tsv")"
assert_eq "slot 2 is occupied" "occupied" "$(awk -F'\t' '$1 == 2 { print $3 }' "$tsv")"
assert_eq "slot 3 is still free" "free" "$(awk -F'\t' '$1 == 3 { print $3 }' "$tsv")"

out=$(run status_pool students)
assert_eq "status shows 1 free, 2 occupied" "1" "$(printf '%s\n' "$out" | grep -c 'free: 1, occupied: 2')"

run export_pool students "" "$TMP/out.tar.gz"
listing=$(tar -tzf "$TMP/out.tar.gz")
assert_eq "export archives all 3 bootstrap scripts" "3" "$(printf '%s\n' "$listing" | wc -l | tr -d ' ')"

out=$(run release_pool students 1)
assert_eq "release_pool frees slot 1" "free" "$(awk -F'\t' '$1 == 1 { print $3 }' "$tsv")"

out=$(run occupy_pool students 1)
status=$?
assert_eq "re-occupying the freed slot 1 now succeeds" "0" "$status"
assert_eq "slot 1 is occupied again" "occupied" "$(awk -F'\t' '$1 == 1 { print $3 }' "$tsv")"

out=$(run status_pool students)
assert_eq "final status shows 1 free, 2 occupied" "1" "$(printf '%s\n' "$out" | grep -c 'free: 1, occupied: 2')"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
