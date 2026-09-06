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

uuid1=$(run generate_uuid)
uuid2=$(run generate_uuid)

if printf '%s' "$uuid1" | grep -Eq '^[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}$'; then
    printf 'ok   - generated uuid matches canonical format\n'
    pass=$((pass + 1))
else
    printf 'FAIL - generated uuid matches canonical format (got %s)\n' "$uuid1"
    fail=$((fail + 1))
fi

assert_eq "two calls produce different uuids" "1" "$([ "$uuid1" != "$uuid2" ] && echo 1 || echo 0)"

printf 'hello world' > "$TMP/plain.txt"
encoded=$(run base64_flatten "$TMP/plain.txt")
assert_eq "base64_flatten produces expected encoding" "aGVsbG8gd29ybGQ=" "$encoded"

decoded=$(printf '%s' "$encoded" | openssl base64 -A -d)
assert_eq "base64_flatten output round-trips" "hello world" "$decoded"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
