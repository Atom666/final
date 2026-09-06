#!/bin/sh
set -u

SCRIPT="$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd)/scripts/nad-provision.sh"

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
      parse_args "$@"
      printf 'NAD_HOST=%s\n' "$NAD_HOST"
      printf 'MODE=%s\n' "$MODE"
      printf 'MODE_VALUE=%s\n' "$MODE_VALUE"
      printf 'STATE_DIR=%s\n' "$STATE_DIR" )
}

run_err() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      parse_args "$@" ) 2>&1 1>/dev/null
}

out=$(run_ok --nad-host 192.168.1.78 --count 3)
assert_eq "parses --nad-host" "NAD_HOST=192.168.1.78" "$(printf '%s\n' "$out" | grep '^NAD_HOST=')"
assert_eq "parses --count" "MODE=count" "$(printf '%s\n' "$out" | grep '^MODE=')"
assert_eq "captures --count value" "MODE_VALUE=3" "$(printf '%s\n' "$out" | grep '^MODE_VALUE=')"
assert_eq "default state dir" "STATE_DIR=$HOME/mirror-certs" "$(printf '%s\n' "$out" | grep '^STATE_DIR=')"

out=$(run_ok --nad-host 10.0.0.5 --add 2 --state-dir /tmp/xyz)
assert_eq "parses --add" "MODE=add" "$(printf '%s\n' "$out" | grep '^MODE=')"
assert_eq "captures --add value" "MODE_VALUE=2" "$(printf '%s\n' "$out" | grep '^MODE_VALUE=')"
assert_eq "parses --state-dir" "STATE_DIR=/tmp/xyz" "$(printf '%s\n' "$out" | grep '^STATE_DIR=')"

err=$(run_err --count 3)
assert_eq "missing --nad-host errors" "--nad-host is required" "$err"

err=$(run_err --nad-host 1.2.3.4)
assert_eq "missing mode errors" "one of --count or --add is required" "$err"

err=$(run_err --nad-host 1.2.3.4 --count 3 --add 2)
assert_eq "count+add mutually exclusive" "--count and --add are mutually exclusive" "$err"

err=$(run_err --nad-host 1.2.3.4 --count abc)
assert_eq "rejects non-integer count" "--count must be a positive integer" "$err"

err=$(run_err --nad-host 1.2.3.4 --count 0)
assert_eq "rejects zero count" "--count must be a positive integer" "$err"

err=$(run_err --nad-host 'evil;rm -rf /' --count 1)
assert_eq "rejects nad-host with invalid characters" "--nad-host must be a valid IP address or hostname" "$err"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
