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

(
    MIRROR_PROVISION_SOURCE=1
    MIRROR_PROVISION_SCRIPT_DIR="$REPO_ROOT/scripts"
    . "$SCRIPT"
    STATE_DIR="$TMP/state"
    NAD_HOST="192.168.1.78"
    AGENTS_TSV="$STATE_DIR/agents.tsv"
    MODE=count
    MODE_VALUE=2
    ensure_ca
    provision_agents
)

assert_eq "count mode provisions the requested total" "2" "$(wc -l < "$STATE_DIR/agents.tsv" | tr -d ' ')"

first_two=$(cat "$STATE_DIR/agents.tsv")

(
    MIRROR_PROVISION_SOURCE=1
    MIRROR_PROVISION_SCRIPT_DIR="$REPO_ROOT/scripts"
    . "$SCRIPT"
    STATE_DIR="$TMP/state"
    NAD_HOST="192.168.1.78"
    AGENTS_TSV="$STATE_DIR/agents.tsv"
    MODE=add
    MODE_VALUE=1
    provision_agents
)

assert_eq "add mode appends on top of existing agents" "3" "$(wc -l < "$STATE_DIR/agents.tsv" | tr -d ' ')"
assert_eq "existing agents are left untouched" "$first_two" "$(head -n2 "$STATE_DIR/agents.tsv")"
assert_eq "new agent gets index 3" "3" "$(sed -n '3p' "$STATE_DIR/agents.tsv" | cut -f1)"

(
    MIRROR_PROVISION_SOURCE=1
    MIRROR_PROVISION_SCRIPT_DIR="$REPO_ROOT/scripts"
    . "$SCRIPT"
    STATE_DIR="$TMP/state"
    NAD_HOST="192.168.1.78"
    AGENTS_TSV="$STATE_DIR/agents.tsv"
    MODE=count
    MODE_VALUE=3
    provision_agents
    print_summary > "$TMP/summary_noop.txt"
)

assert_eq "count already met stays at 3 rows" "3" "$(wc -l < "$STATE_DIR/agents.tsv" | tr -d ' ')"
assert_eq "print_summary reports no new agents when target already met" "1" "$(grep -c 'No new agents needed' "$TMP/summary_noop.txt")"

printf '\n%d passed, %d failed\n' "$pass" "$fail"
[ "$fail" -eq 0 ]
