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

printf '192.168.1.78\n' > "$STATE_DIR/nad-host"
run ensure_ca >/dev/null

out=$(run seed_pool labs 3)
assert_eq "seed_pool reports how many slots it created" "1" "$(printf '%s\n' "$out" | grep -c 'Seeded pool labs with 3 slot')"

tsv="$STATE_DIR/labs/labs.tsv"
assert_eq "tsv has one row per slot" "3" "$(wc -l < "$tsv" | tr -d ' ')"
assert_eq "all rows start free" "0" "$(awk -F'\t' '$3 != "free"' "$tsv" | wc -l | tr -d ' ')"

uuid1=$(awk -F'\t' '$1 == 1 { print $2 }' "$tsv")
key="$STATE_DIR/labs/machine1_$uuid1.key"
crt="$STATE_DIR/labs/machine1_$uuid1.crt"
uuidfile="$STATE_DIR/labs/machine1_${uuid1}.uuid"
boot="$STATE_DIR/labs/machine1_${uuid1}.sh"
assert_eq "slot 1 key exists" "1" "$([ -f "$key" ] && echo 1 || echo 0)"
assert_eq "slot 1 cert exists" "1" "$([ -f "$crt" ] && echo 1 || echo 0)"
assert_eq "slot 1 uuid file exists" "1" "$([ -f "$uuidfile" ] && echo 1 || echo 0)"
assert_eq "no bootstrap script for labs" "0" "$([ -f "$boot" ] && echo 1 || echo 0)"

assert_eq "uuid file content matches the slot's uuid" "$uuid1" "$(cat "$uuidfile")"
assert_eq "uuid file has no trailing newline (agent's parse_uuid rejects it)" \
    "$(printf '%s' "$uuid1" | wc -c)" "$(wc -c < "$uuidfile" | tr -d ' ')"

cert_subject=$(openssl x509 -in "$crt" -noout -subject)
assert_eq "slot cert CN is the bare uuid" "subject=CN=$uuid1" "$cert_subject"

verify_out=$(openssl verify -CAfile "$STATE_DIR/ca.crt" "$crt" 2>&1)
assert_eq "slot cert verifies against the CA" "OK" "${verify_out##*: }"

before=$(cat "$tsv")
out=$(run seed_pool labs 3)
after=$(cat "$tsv")
assert_eq "re-running seed with the same count is a no-op" "$before" "$after"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
