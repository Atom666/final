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
      NAD_HOST="192.168.1.78"
      "$@" )
}

run ensure_ca >/dev/null

out=$(run seed_pool students 3)
assert_eq "seed_pool reports how many slots it created" "1" "$(printf '%s\n' "$out" | grep -c 'Seeded pool students with 3 slot')"

tsv="$STATE_DIR/students/students.tsv"
assert_eq "tsv has one row per slot" "3" "$(wc -l < "$tsv" | tr -d ' ')"
assert_eq "all rows start free" "0" "$(awk -F'\t' '$3 != "free"' "$tsv" | wc -l | tr -d ' ')"
assert_eq "slot indices are 1..3" "$(printf '1\n2\n3')" "$(cut -f1 "$tsv")"

uuid1=$(awk -F'\t' '$1 == 1 { print $2 }' "$tsv")
key="$STATE_DIR/students/student1_$uuid1.key"
crt="$STATE_DIR/students/student1_$uuid1.crt"
boot="$STATE_DIR/students/student1_${uuid1}.sh"
assert_eq "slot 1 key exists" "1" "$([ -f "$key" ] && echo 1 || echo 0)"
assert_eq "slot 1 cert exists" "1" "$([ -f "$crt" ] && echo 1 || echo 0)"
assert_eq "slot 1 bootstrap script exists" "1" "$([ -f "$boot" ] && echo 1 || echo 0)"

cert_subject=$(openssl x509 -in "$crt" -noout -subject)
assert_eq "slot cert CN is the bare uuid" "subject=CN=$uuid1" "$cert_subject"

verify_out=$(openssl verify -CAfile "$STATE_DIR/ca.crt" "$crt" 2>&1)
assert_eq "slot cert verifies against the CA" "OK" "${verify_out##*: }"

before=$(cat "$tsv")
out=$(run seed_pool students 3)
after=$(cat "$tsv")
assert_eq "re-running seed with the same count is a no-op" "$before" "$after"
assert_eq "re-running with the same count reports already seeded" "1" "$(printf '%s\n' "$out" | grep -c 'already seeded')"

err=$(run seed_pool students 5 2>&1 1>/dev/null)
assert_eq "re-running with a different count is an error" "1" "$(printf '%s\n' "$err" | grep -c 'growing/shrinking')"

err=$(run seed_pool students 5 2>&1)
status=$?
assert_eq "seed_pool exits non-zero on the count mismatch" "1" "$status"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
