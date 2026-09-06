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

run() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      "$@" )
}

assert_eq "count of missing file is 0" "0" "$(run current_agent_count "$TMP/none.tsv")"
assert_eq "next index for missing file is 1" "1" "$(run next_agent_index "$TMP/none.tsv")"

tsv="$TMP/agents.tsv"
printf '1\tuuid-a\t2026-01-01T00:00:00Z\n' > "$tsv"
printf '2\tuuid-b\t2026-01-01T00:00:01Z\n' >> "$tsv"

assert_eq "count reflects two rows" "2" "$(run current_agent_count "$tsv")"
assert_eq "next index after two rows is 3" "3" "$(run next_agent_index "$tsv")"

printf '5\tuuid-c\t2026-01-01T00:00:02Z\n' >> "$tsv"
assert_eq "next index uses max, not row count" "6" "$(run next_agent_index "$tsv")"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
