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
      STATE_DIR=$1
      NAD_HOST=$2
      "$3" )
}

STATE_DIR="$TMP/state"
mkdir -p "$STATE_DIR"

run "$STATE_DIR" "192.168.1.78" ensure_ca
assert_eq "ca.key created" "1" "$([ -f "$STATE_DIR/ca.key" ] && echo 1 || echo 0)"
assert_eq "ca.crt created" "1" "$([ -f "$STATE_DIR/ca.crt" ] && echo 1 || echo 0)"

before=$(cat "$STATE_DIR/ca.crt")
run "$STATE_DIR" "192.168.1.78" ensure_ca
after=$(cat "$STATE_DIR/ca.crt")
assert_eq "re-running ensure_ca does not regenerate the CA" "$before" "$after"

run "$STATE_DIR" "192.168.1.78" ensure_server_cert
assert_eq "server.key created" "1" "$([ -f "$STATE_DIR/server.key" ] && echo 1 || echo 0)"
assert_eq "server.crt created" "1" "$([ -f "$STATE_DIR/server.crt" ] && echo 1 || echo 0)"
assert_eq "server.csr cleaned up" "0" "$([ -f "$STATE_DIR/server.csr" ] && echo 1 || echo 0)"

san=$(openssl x509 -in "$STATE_DIR/server.crt" -noout -text | grep -c "IP Address:192.168.1.78")
assert_eq "server cert SAN includes nad host IP" "1" "$san"

ca_subject=$(openssl x509 -in "$STATE_DIR/ca.crt" -noout -subject)
assert_eq "CA certificate has the expected CN" "subject=CN=mirror-provision-ca" "$ca_subject"

server_subject=$(openssl x509 -in "$STATE_DIR/server.crt" -noout -subject)
assert_eq "server certificate has the expected CN" "subject=CN=nad-mirror.internal" "$server_subject"

before=$(cat "$STATE_DIR/server.crt")
run "$STATE_DIR" "192.168.1.78" ensure_server_cert
after=$(cat "$STATE_DIR/server.crt")
assert_eq "re-running ensure_server_cert does not regenerate it" "$before" "$after"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
