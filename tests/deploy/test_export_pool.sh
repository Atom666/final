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
for n in 1 2 3; do
    uuid=$(awk -F'\t' -v n="$n" '$1 == n { print $2 }' "$tsv")
    echo "bootstrap $n" > "$STATE_DIR/students/student${n}_${uuid}.sh"
done

run() {
    ( MIRROR_PROVISION_SOURCE=1
      . "$SCRIPT"
      STATE_DIR="$TMP/state"
      "$@" )
}

run export_pool students "" "$TMP/all.tar.gz"
listing=$(tar -tzf "$TMP/all.tar.gz" | sort)
assert_eq "default export includes every slot" "3" "$(printf '%s\n' "$listing" | wc -l | tr -d ' ')"

run export_pool students 2 "$TMP/one.tar.gz"
listing=$(tar -tzf "$TMP/one.tar.gz")
assert_eq "explicit --slots exports only that slot" "1" "$(printf '%s\n' "$listing" | wc -l | tr -d ' ')"
assert_eq "exported file matches slot 2's script name" "1" "$(printf '%s\n' "$listing" | grep -c '^student2_')"

before=$(cat "$tsv")
after=$(cat "$tsv")
assert_eq "export never touches the registry" "$before" "$after"

err=$(run export_pool students 99 "$TMP/bad.tar.gz" 2>&1 1>/dev/null)
assert_eq "exporting a nonexistent slot is an error" "1" "$(printf '%s\n' "$err" | grep -c 'slot 99')"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
