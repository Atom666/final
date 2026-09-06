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
mkdir -p "$STATE_DIR/agents"

(
    MIRROR_PROVISION_SOURCE=1
    MIRROR_PROVISION_SCRIPT_DIR="$REPO_ROOT/scripts"
    . "$SCRIPT"
    STATE_DIR="$TMP/state"
    NAD_HOST="192.168.1.78"
    AGENTS_TSV="$STATE_DIR/agents.tsv"
    ensure_ca
    issue_agent 1
)

assert_eq "agents.tsv has one row" "1" "$(wc -l < "$STATE_DIR/agents.tsv" | tr -d ' ')"

uuid=$(cut -f2 "$STATE_DIR/agents.tsv")
assert_eq "agent key file exists" "1" "$([ -f "$STATE_DIR/agents/$uuid.key" ] && echo 1 || echo 0)"
assert_eq "agent cert file exists" "1" "$([ -f "$STATE_DIR/agents/$uuid.crt" ] && echo 1 || echo 0)"
assert_eq "csr cleaned up" "0" "$([ -f "$STATE_DIR/agents/$uuid.csr" ] && echo 1 || echo 0)"

agent_subject=$(openssl x509 -in "$STATE_DIR/agents/$uuid.crt" -noout -subject)
assert_eq "issued agent certificate CN equals its UUID" "subject=CN=$uuid" "$agent_subject"

# openssl echoes back the path it was given; under MSYS that comes back
# Windows-style, so compare only the verdict after the trailing "<path>: ".
verify_out=$(openssl verify -CAfile "$STATE_DIR/ca.crt" "$STATE_DIR/agents/$uuid.crt" 2>&1)
assert_eq "issued agent certificate verifies against the CA" "OK" "${verify_out##*: }"

boot="$STATE_DIR/agents/agent-1-bootstrap.sh"
assert_eq "bootstrap script created" "1" "$([ -f "$boot" ] && echo 1 || echo 0)"

out=$(
    MIRROR_BOOTSTRAP_SOURCE=1
    . "$boot"
    printf 'AGENT_UUID=%s\n' "$AGENT_UUID"
    printf 'RECEIVER_HOST=%s\n' "$RECEIVER_HOST"
    printf 'RECEIVER_PORT=%s\n' "$RECEIVER_PORT"
    printf 'TLS_SERVER_NAME=%s\n' "$TLS_SERVER_NAME"
)
assert_eq "bootstrap script embeds the issued uuid" "AGENT_UUID=$uuid" "$(printf '%s\n' "$out" | grep '^AGENT_UUID=')"
assert_eq "bootstrap script embeds the nad host" "RECEIVER_HOST=192.168.1.78" "$(printf '%s\n' "$out" | grep '^RECEIVER_HOST=')"
assert_eq "bootstrap script embeds the default port" "RECEIVER_PORT=9443" "$(printf '%s\n' "$out" | grep '^RECEIVER_PORT=')"
assert_eq "bootstrap script embeds the tls server name" "TLS_SERVER_NAME=nad-mirror.internal" "$(printf '%s\n' "$out" | grep '^TLS_SERVER_NAME=')"

ca_b64=$(
    MIRROR_BOOTSTRAP_SOURCE=1
    . "$boot"
    printf '%s' "$CA_CRT_B64"
)
decoded_ca=$(printf '%s' "$ca_b64" | openssl base64 -A -d)
original_ca=$(cat "$STATE_DIR/ca.crt")
assert_eq "embedded CA cert round-trips to the original" "$original_ca" "$decoded_ca"

probe="$TMP/chmod-probe"
: > "$probe"
chmod 700 "$probe"
probe_mode=$(stat -c '%a' "$probe" 2>/dev/null || stat -f '%Lp' "$probe")
rm -f "$probe"

perm=$(stat -c '%a' "$boot" 2>/dev/null || stat -f '%Lp' "$boot")
if [ "$probe_mode" = "700" ]; then
    assert_eq "bootstrap script is only readable by owner" "700" "$perm"
else
    printf 'skip - bootstrap script permission check (chmod not honored on this filesystem)\n'
fi

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
