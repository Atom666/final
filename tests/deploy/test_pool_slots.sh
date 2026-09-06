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

run() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      "$@" )
}

run_err() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      "$@" ) 2>&1 1>/dev/null
}

assert_eq "students pool labels as student" "student" "$(run pool_label students)"
assert_eq "labs pool labels as machine" "machine" "$(run pool_label labs)"

err=$(run_err pool_label bogus)
assert_eq "unknown pool name is rejected" "unknown pool: bogus" "$err"

assert_eq "single index" "7" "$(run expand_slot_spec 7)"
assert_eq "comma list" "$(printf '1\n3\n5')" "$(run expand_slot_spec 1,3,5)"
assert_eq "range expands inclusive" "$(printf '5\n6\n7\n8\n9')" "$(run expand_slot_spec 5-9)"
assert_eq "mixed list and range" "$(printf '1\n5\n6\n7')" "$(run expand_slot_spec 1,5-7)"

err=$(run_err expand_slot_spec 1,abc)
assert_eq "non-numeric slot is rejected" "invalid slot: abc" "$err"

err=$(run_err expand_slot_spec 5-abc)
assert_eq "non-numeric range bound is rejected" "invalid slot range: 5-abc" "$err"

err=$(run_err expand_slot_spec 9-5)
assert_eq "reversed range is rejected" "invalid slot range: 9-5" "$err"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
