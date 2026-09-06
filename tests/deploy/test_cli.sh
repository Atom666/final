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

run_ok() {
    ( MIRROR_PROVISION_SOURCE=1
      MIRROR_PROVISION_SCRIPT_DIR="$REPO_ROOT/scripts"
      . "$SCRIPT"
      "$@" )
}

run_err() {
    ( MIRROR_PROVISION_SOURCE=1
      MIRROR_PROVISION_SCRIPT_DIR="$REPO_ROOT/scripts"
      . "$SCRIPT"
      "$@" ) 2>&1 1>/dev/null
}

err=$(run_err main)
assert_eq "no subcommand shows usage error" "1" "$(printf '%s\n' "$err" | grep -c 'Usage: scripts/nad-provision.sh')"

err=$(run_err main bogus)
assert_eq "unknown subcommand is rejected" "1" "$(printf '%s\n' "$err" | grep -c 'Unknown command: bogus')"

err=$(run_err main seed --count 3)
assert_eq "seed without --pool errors" "1" "$(printf '%s\n' "$err" | grep -c -- '--pool is required')"

err=$(run_err main seed --pool students)
assert_eq "seed without --count errors" "1" "$(printf '%s\n' "$err" | grep -c -- '--count is required')"

err=$(run_err main occupy --pool students)
assert_eq "occupy without --slots errors" "1" "$(printf '%s\n' "$err" | grep -c -- '--slots is required')"

err=$(run_err main export --pool students)
assert_eq "export without --out errors" "1" "$(printf '%s\n' "$err" | grep -c -- '--out is required')"

STATE_DIR_ARG="$TMP/state"
out=$(run_ok main init --nad-host 192.168.1.78 --state-dir "$STATE_DIR_ARG" 2>&1)
status=$?
if [ "$status" -ne 0 ] && printf '%s' "$out" | grep -qE 'mirror-receiver binary not found|must run as root'; then
    printf 'skip - full init end-to-end (requires root + an installed mirror-receiver, unavailable on this machine)\n'
else
    assert_eq "init succeeds when the receiver is installed" "0" "$status"
    assert_eq "init creates the CA" "1" "$([ -f "$STATE_DIR_ARG/ca.crt" ] && echo 1 || echo 0)"

    run_ok main seed --pool students --count 2 --state-dir "$STATE_DIR_ARG" >/dev/null
    assert_eq "seed via CLI creates the pool tsv" "2" "$(wc -l < "$STATE_DIR_ARG/students/students.tsv" | tr -d ' ')"

    run_ok main occupy --pool students --slots 1 --state-dir "$STATE_DIR_ARG" >/dev/null
    assert_eq "occupy via CLI updates status" "occupied" "$(awk -F'\t' '$1 == 1 { print $3 }' "$STATE_DIR_ARG/students/students.tsv")"

    out=$(run_ok main status --pool students --state-dir "$STATE_DIR_ARG")
    assert_eq "status via CLI reports one occupied" "1" "$(printf '%s\n' "$out" | grep -c 'free: 1, occupied: 1')"

    run_ok main export --pool students --out "$TMP/export.tar.gz" --state-dir "$STATE_DIR_ARG" >/dev/null
    assert_eq "export via CLI produces an archive with both slots" "2" "$(tar -tzf "$TMP/export.tar.gz" | wc -l | tr -d ' ')"
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
