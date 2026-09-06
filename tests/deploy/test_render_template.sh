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

tmpl="$TMP/sample.tmpl"
cat > "$tmpl" <<'EOF'
name = @@NAME@@
host = @@HOST@@
literal = $NOT_A_PLACEHOLDER
EOF

out=$(run render_template "$tmpl" "NAME=agent-1" "HOST=192.168.1.78")
assert_eq "substitutes NAME" "name = agent-1" "$(printf '%s\n' "$out" | grep '^name = ')"
assert_eq "substitutes HOST" "host = 192.168.1.78" "$(printf '%s\n' "$out" | grep '^host = ')"
assert_eq "leaves real shell vars untouched" 'literal = $NOT_A_PLACEHOLDER' "$(printf '%s\n' "$out" | grep '^literal = ')"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
