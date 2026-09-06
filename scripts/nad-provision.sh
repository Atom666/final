#!/bin/sh
set -eu

# MSYS/Git-Bash (dev machines only) rewrites arguments that look like absolute
# POSIX paths into Windows paths, which would mangle openssl's -subj "/CN=...".
# Excluding that prefix keeps the correct single-slash form working locally.
# On Linux this is an unused, harmless environment variable.
export MSYS2_ARG_CONV_EXCL='/CN='

: "${MIRROR_PROVISION_SCRIPT_DIR:=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)}"
TEMPLATE_DIR="$MIRROR_PROVISION_SCRIPT_DIR/templates"
PROJECT_DIR=$(dirname "$MIRROR_PROVISION_SCRIPT_DIR")
TLS_SERVER_NAME="pt-nad-rt.edtechlab.local"

usage() {
    cat <<'EOF'
Usage: scripts/nad-provision.sh --nad-host HOST (--count N | --add K) [--state-dir DIR]

Provisions the receiver side of a mirror-agent deployment on this host and
emits one self-contained bootstrap script per new agent.

  --nad-host HOST   Address agents will use to reach this host. Baked into
                     the server certificate SAN and every generated
                     agent.conf.
  --count N         Ensure at least N agents are provisioned in total.
  --add K           Provision K additional agents on top of however many
                     already exist.
  --state-dir DIR   Where certs/state/generated scripts live.
                     Default: $HOME/mirror-certs
  -h, --help        Show this help
EOF
}

NAD_HOST=
MODE=
MODE_VALUE=
STATE_DIR="$HOME/mirror-certs"
AGENTS_TSV=
NEW_AGENTS=

is_positive_int() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        0) return 1 ;;
        *) return 0 ;;
    esac
}

parse_args() {
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --nad-host)
                [ "$#" -ge 2 ] || { echo "--nad-host requires a value" >&2; exit 2; }
                case "$2" in
                    *[!A-Za-z0-9.:-]*|'')
                        echo "--nad-host must be a valid IP address or hostname" >&2
                        exit 2
                        ;;
                esac
                NAD_HOST=$2
                shift 2
                ;;
            --count)
                [ "$#" -ge 2 ] || { echo "--count requires a value" >&2; exit 2; }
                [ -z "$MODE" ] || { echo "--count and --add are mutually exclusive" >&2; exit 2; }
                is_positive_int "$2" || { echo "--count must be a positive integer" >&2; exit 2; }
                MODE=count
                MODE_VALUE=$2
                shift 2
                ;;
            --add)
                [ "$#" -ge 2 ] || { echo "--add requires a value" >&2; exit 2; }
                [ -z "$MODE" ] || { echo "--count and --add are mutually exclusive" >&2; exit 2; }
                is_positive_int "$2" || { echo "--add must be a positive integer" >&2; exit 2; }
                MODE=add
                MODE_VALUE=$2
                shift 2
                ;;
            --state-dir)
                [ "$#" -ge 2 ] || { echo "--state-dir requires a value" >&2; exit 2; }
                STATE_DIR=$2
                shift 2
                ;;
            -h|--help)
                usage
                exit 0
                ;;
            *)
                echo "Unknown option: $1" >&2
                usage >&2
                exit 2
                ;;
        esac
    done
    [ -n "$NAD_HOST" ] || { echo "--nad-host is required" >&2; exit 2; }
    [ -n "$MODE" ] || { echo "one of --count or --add is required" >&2; exit 2; }
}

current_agent_count() {
    [ -f "$1" ] || { echo 0; return; }
    wc -l < "$1" | tr -d ' '
}

next_agent_index() {
    if [ ! -f "$1" ]; then
        echo 1
        return
    fi
    awk -F'\t' 'BEGIN { max = 0 } { if ($1 + 0 > max) max = $1 + 0 } END { print max + 1 }' "$1"
}

render_template() {
    template=$1
    shift
    content=$(cat "$template")
    for pair in "$@"; do
        name=${pair%%=*}
        value=${pair#*=}
        content=$(printf '%s' "$content" | sed "s|@@${name}@@|${value}|g")
    done
    printf '%s\n' "$content"
}

generate_uuid() {
    hex=$(openssl rand -hex 16)
    printf '%s-%s-%s-%s-%s\n' \
        "$(printf '%s' "$hex" | cut -c1-8)" \
        "$(printf '%s' "$hex" | cut -c9-12)" \
        "$(printf '%s' "$hex" | cut -c13-16)" \
        "$(printf '%s' "$hex" | cut -c17-20)" \
        "$(printf '%s' "$hex" | cut -c21-32)"
}

base64_flatten() {
    openssl base64 -A -in "$1"
}

check_nad_host_stable() {
    host_file="$STATE_DIR/nad-host"
    if [ -f "$host_file" ]; then
        stored=$(cat "$host_file")
        if [ "$stored" != "$NAD_HOST" ]; then
            echo "Stored nad-host ($stored) does not match --nad-host ($NAD_HOST)." >&2
            echo "Remove $STATE_DIR to start over with a new host." >&2
            exit 1
        fi
    else
        printf '%s\n' "$NAD_HOST" > "$host_file"
    fi
}

check_receiver_installed() {
    if [ ! -x /usr/local/sbin/mirror-receiver ]; then
        echo "mirror-receiver binary not found at /usr/local/sbin/mirror-receiver" >&2
        echo "Build and install it first: ./scripts/build.sh --clean --install" >&2
        exit 1
    fi
    if [ ! -f /etc/systemd/system/mirror-receiver.service ]; then
        echo "mirror-receiver.service not found; run ./scripts/build.sh --clean --install first" >&2
        exit 1
    fi
    if [ ! -f /etc/systemd/system/mirror-interface.service ]; then
        echo "mirror-interface.service not found; run ./scripts/build.sh --clean --install first" >&2
        exit 1
    fi
}

ensure_ca() {
    if [ -f "$STATE_DIR/ca.key" ] && [ -f "$STATE_DIR/ca.crt" ]; then
        return
    fi
    ( umask 077
      openssl genrsa -out "$STATE_DIR/ca.key" 4096 2>/dev/null
      openssl req -x509 -new -key "$STATE_DIR/ca.key" -sha256 -days 3650 \
          -out "$STATE_DIR/ca.crt" -subj "/CN=mirror-provision-ca" )
}

ensure_server_cert() {
    if [ -f "$STATE_DIR/server.key" ] && [ -f "$STATE_DIR/server.crt" ]; then
        return
    fi
    ( umask 077
      openssl genrsa -out "$STATE_DIR/server.key" 2048 2>/dev/null
      openssl req -new -key "$STATE_DIR/server.key" -out "$STATE_DIR/server.csr" \
          -subj "/CN=$TLS_SERVER_NAME" \
          -addext "subjectAltName=DNS:$TLS_SERVER_NAME,IP:$NAD_HOST"
      openssl x509 -req -in "$STATE_DIR/server.csr" -CA "$STATE_DIR/ca.crt" -CAkey "$STATE_DIR/ca.key" \
          -CAcreateserial -out "$STATE_DIR/server.crt" -days 365 -sha256 -copy_extensions copy )
    rm -f "$STATE_DIR/server.csr"
}

issue_agent() {
    idx=$1
    uuid=$(generate_uuid)
    key="$STATE_DIR/agents/$uuid.key"
    crt="$STATE_DIR/agents/$uuid.crt"
    csr="$STATE_DIR/agents/$uuid.csr"
    ( umask 077
      openssl genrsa -out "$key" 2048 2>/dev/null
      openssl req -new -key "$key" -out "$csr" -subj "/CN=$uuid" )
    openssl x509 -req -in "$csr" -CA "$STATE_DIR/ca.crt" -CAkey "$STATE_DIR/ca.key" \
        -CAcreateserial -out "$crt" -days 365 -sha256
    rm -f "$csr"
    # Record the agent only after its bootstrap script exists, so a failed
    # render leaves no orphan row and the next run retries this index.
    write_bootstrap_script "$idx" "$uuid" "$crt" "$key"
    printf '%s\t%s\t%s\n' "$idx" "$uuid" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" >> "$AGENTS_TSV"
}

write_bootstrap_script() {
    idx=$1
    uuid=$2
    crt=$3
    key=$4
    out="$STATE_DIR/agents/agent-$idx-bootstrap.sh"
    render_template "$TEMPLATE_DIR/agent-bootstrap.sh.tmpl" \
        "AGENT_UUID=$uuid" \
        "RECEIVER_HOST=$NAD_HOST" \
        "RECEIVER_PORT=9443" \
        "TLS_SERVER_NAME=nad-mirror.internal" \
        "CA_CRT_B64=$(base64_flatten "$STATE_DIR/ca.crt")" \
        "CLIENT_CRT_B64=$(base64_flatten "$crt")" \
        "CLIENT_KEY_B64=$(base64_flatten "$key")" \
        > "$out"
    chmod 700 "$out"
}

provision_agents() {
    mkdir -p "$STATE_DIR/agents"
    chmod 700 "$STATE_DIR/agents"
    have=$(current_agent_count "$AGENTS_TSV")
    case "$MODE" in
        count) target=$MODE_VALUE ;;
        add) target=$((have + MODE_VALUE)) ;;
    esac
    while [ "$have" -lt "$target" ]; do
        idx=$(next_agent_index "$AGENTS_TSV")
        issue_agent "$idx"
        have=$((have + 1))
        NEW_AGENTS="$NEW_AGENTS $idx"
    done
}

print_summary() {
    total=$(current_agent_count "$AGENTS_TSV")
    printf '\nProvisioned %d agent(s) total.\n' "$total"
    if [ -n "$NEW_AGENTS" ]; then
        printf 'New bootstrap scripts:\n'
        for idx in $NEW_AGENTS; do
            printf '  %s\n' "$STATE_DIR/agents/agent-$idx-bootstrap.sh"
        done
        printf '\nCopy each script to its target agent VM and run it there as root.\n'
    else
        printf 'No new agents needed (already at or above target).\n'
    fi
}

install_receiver_files() {
    install -d -m 0755 /etc/mirror-receiver
    install -m 0644 "$STATE_DIR/ca.crt" /etc/mirror-receiver/agents-ca.crt
    install -m 0644 "$STATE_DIR/server.crt" /etc/mirror-receiver/server.crt
    install -m 0600 "$STATE_DIR/server.key" /etc/mirror-receiver/server.key
    if [ ! -f /etc/mirror-receiver/receiver.conf ]; then
        install -m 0644 "$PROJECT_DIR/examples/receiver.conf" /etc/mirror-receiver/receiver.conf
    fi
}

enable_receiver_services() {
    systemctl daemon-reload
    systemctl enable --now mirror-interface.service
    systemctl enable --now mirror-receiver.service
}

main() {
    parse_args "$@"
    [ "$(id -u)" -eq 0 ] || { echo "must run as root" >&2; exit 1; }
    check_receiver_installed
    mkdir -p "$STATE_DIR"
    chmod 700 "$STATE_DIR"
    AGENTS_TSV="$STATE_DIR/agents.tsv"
    check_nad_host_stable
    ensure_ca
    ensure_server_cert
    install_receiver_files
    enable_receiver_services
    provision_agents
    print_summary
}

if [ "${MIRROR_PROVISION_SOURCE:-0}" != "1" ]; then
    main "$@"
fi
