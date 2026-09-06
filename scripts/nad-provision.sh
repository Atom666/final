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

is_positive_int() {
    case "$1" in
        ''|*[!0-9]*) return 1 ;;
        0) return 1 ;;
        *) return 0 ;;
    esac
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

pool_label() {
    case "$1" in
        students) echo student ;;
        labs) echo machine ;;
        *) echo "unknown pool: $1" >&2; exit 1 ;;
    esac
}

expand_slot_spec() {
    spec=$1
    IFS=','
    for part in $spec; do
        case "$part" in
            *-*)
                lo=${part%-*}
                hi=${part#*-}
                is_positive_int "$lo" && is_positive_int "$hi" || {
                    echo "invalid slot range: $part" >&2
                    exit 2
                }
                [ "$lo" -le "$hi" ] || {
                    echo "invalid slot range: $part" >&2
                    exit 2
                }
                i=$lo
                while [ "$i" -le "$hi" ]; do
                    echo "$i"
                    i=$((i + 1))
                done
                ;;
            *)
                is_positive_int "$part" || {
                    echo "invalid slot: $part" >&2
                    exit 2
                }
                echo "$part"
                ;;
        esac
    done
}

write_pool_bootstrap_script() {
    pool=$1
    idx=$2
    uuid=$3
    crt=$4
    key=$5
    label=$(pool_label "$pool")
    out="$STATE_DIR/$pool/${label}${idx}_${uuid}.sh"
    render_template "$TEMPLATE_DIR/agent-bootstrap.sh.tmpl" \
        "AGENT_UUID=$uuid" \
        "RECEIVER_HOST=$NAD_HOST" \
        "RECEIVER_PORT=9443" \
        "TLS_SERVER_NAME=$TLS_SERVER_NAME" \
        "CA_CRT_B64=$(base64_flatten "$STATE_DIR/ca.crt")" \
        "CLIENT_CRT_B64=$(base64_flatten "$crt")" \
        "CLIENT_KEY_B64=$(base64_flatten "$key")" \
        > "$out"
    chmod 700 "$out"
}

issue_pool_cert() {
    pool=$1
    idx=$2
    label=$(pool_label "$pool")
    uuid=$(generate_uuid)
    key="$STATE_DIR/$pool/${label}${idx}_${uuid}.key"
    crt="$STATE_DIR/$pool/${label}${idx}_${uuid}.crt"
    csr="$STATE_DIR/$pool/${label}${idx}_${uuid}.csr"
    ( umask 077
      openssl genrsa -out "$key" 2048 2>/dev/null
      openssl req -new -key "$key" -out "$csr" -subj "/CN=$uuid" )
    openssl x509 -req -in "$csr" -CA "$STATE_DIR/ca.crt" -CAkey "$STATE_DIR/ca.key" \
        -CAcreateserial -out "$crt" -days 365 -sha256
    rm -f "$csr"
    case "$pool" in
        students)
            write_pool_bootstrap_script "$pool" "$idx" "$uuid" "$crt" "$key"
            ;;
        labs)
            # No bootstrap script: start-agent.ps1 already refreshes certs and
            # capture_iface on every boot. The agent reads its UUID from this
            # file (agent_uuid = auto, uuid_file = mirror-agent.uuid) when it
            # doesn't have one of its own yet; it must match the cert's CN or
            # the receiver's mTLS check rejects the connection.
            printf '%s\n' "$uuid" > "$STATE_DIR/$pool/${label}${idx}_${uuid}.uuid"
            ;;
    esac
    printf '%s\t%s\tfree\t%s\n' "$idx" "$uuid" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
        >> "$STATE_DIR/$pool/$pool.tsv"
}

seed_pool() {
    pool=$1
    count=$2
    [ -f "$STATE_DIR/ca.key" ] && [ -f "$STATE_DIR/ca.crt" ] || {
        echo "CA not found in $STATE_DIR; run '$0 init' first" >&2
        exit 1
    }
    [ -f "$STATE_DIR/nad-host" ] || { echo "nad-host not recorded in $STATE_DIR; run '$0 init' first" >&2; exit 1; }
    NAD_HOST=$(cat "$STATE_DIR/nad-host")
    pool_dir="$STATE_DIR/$pool"
    tsv="$pool_dir/$pool.tsv"
    mkdir -p "$pool_dir"
    chmod 700 "$pool_dir"
    if [ -f "$tsv" ]; then
        existing=$(wc -l < "$tsv" | tr -d ' ')
        if [ "$existing" -eq "$count" ]; then
            printf 'Pool %s already seeded with %d slot(s).\n' "$pool" "$existing"
            return 0
        fi
        echo "Pool '$pool' already has $existing slot(s); growing/shrinking a seeded pool is not supported." >&2
        echo "Use a fresh --state-dir if you need a different size." >&2
        exit 1
    fi
    idx=1
    while [ "$idx" -le "$count" ]; do
        issue_pool_cert "$pool" "$idx"
        idx=$((idx + 1))
    done
    printf 'Seeded pool %s with %d slot(s) in %s\n' "$pool" "$count" "$pool_dir"
}

mark_slots() {
    pool=$1
    spec=$2
    target_status=$3
    require_status=$4
    tsv="$STATE_DIR/$pool/$pool.tsv"
    [ -f "$tsv" ] || { echo "Pool '$pool' has not been seeded (no $tsv)" >&2; exit 1; }
    slots=$(expand_slot_spec "$spec")
    slots=$(printf '%s\n' "$slots" | tr '\n' ',')
    tmp="$tsv.tmp.$$"
    failflag="$tmp.fail"
    rm -f "$failflag"
    awk -F'\t' -v OFS='\t' \
        -v slots="$slots" -v target="$target_status" -v require="$require_status" -v failflag="$failflag" -v tmpfile="$tmp" '
        BEGIN {
            n = split(slots, arr, ",")
            for (i = 1; i <= n; i++) if (arr[i] != "") want[arr[i]] = 1
        }
        {
            if ($1 in want) {
                seen[$1] = 1
                if ($3 == require) {
                    print "OK   - slot " $1 " -> " target
                    $3 = target
                } else {
                    print "FAIL - slot " $1 " is already " $3 > "/dev/stderr"
                    system("touch \"" failflag "\"")
                }
            }
            print > tmpfile
        }
        END {
            for (s in want) if (!(s in seen)) {
                print "FAIL - slot " s " does not exist in pool" > "/dev/stderr"
                system("touch \"" failflag "\"")
            }
        }
    ' "$tsv"
    mv "$tmp" "$tsv"
    ok=1
    [ ! -f "$failflag" ] || ok=0
    rm -f "$failflag"
    [ "$ok" -eq 1 ]
}

occupy_pool() {
    mark_slots "$1" "$2" occupied free
}

release_pool() {
    mark_slots "$1" "$2" free occupied
}

status_pool() {
    pool=$1
    tsv="$STATE_DIR/$pool/$pool.tsv"
    [ -f "$tsv" ] || { echo "Pool '$pool' has not been seeded (no $tsv)" >&2; exit 1; }
    printf 'slot\tuuid\tstatus\tissued_at\n'
    cat "$tsv"
    awk -F'\t' '
        { if ($3 == "free") free++; else occupied++ }
        END { printf "\nfree: %d, occupied: %d\n", free + 0, occupied + 0 }
    ' "$tsv"
}

export_pool() {
    pool=$1
    spec=$2
    out=$3
    pool_dir="$STATE_DIR/$pool"
    tsv="$pool_dir/$pool.tsv"
    [ -f "$tsv" ] || { echo "Pool '$pool' has not been seeded (no $tsv)" >&2; exit 1; }
    label=$(pool_label "$pool")
    if [ -n "$spec" ]; then
        slots=$(expand_slot_spec "$spec")
    else
        slots=$(cut -f1 "$tsv")
    fi
    names=""
    for idx in $slots; do
        uuid=$(awk -F'\t' -v idx="$idx" '$1 == idx { print $2 }' "$tsv")
        [ -n "$uuid" ] || { echo "slot $idx does not exist in pool '$pool'" >&2; exit 1; }
        case "$pool" in
            students)
                name="${label}${idx}_${uuid}.sh"
                [ -f "$pool_dir/$name" ] || { echo "bootstrap script missing for slot $idx: $pool_dir/$name" >&2; exit 1; }
                names="$names $name"
                ;;
            labs)
                for ext in crt key uuid; do
                    name="${label}${idx}_${uuid}.$ext"
                    [ -f "$pool_dir/$name" ] || { echo "$ext file missing for slot $idx: $pool_dir/$name" >&2; exit 1; }
                    names="$names $name"
                done
                ;;
        esac
    done
    if [ "$pool" = labs ] && [ -n "$names" ]; then
        [ -f "$STATE_DIR/ca.crt" ] || { echo "ca.crt not found in $STATE_DIR; run '$0 init' first" >&2; exit 1; }
        # shellcheck disable=SC2086 -- $names is a list of known-safe generated filenames
        tar -czf "$out" -C "$pool_dir" $names -C "$STATE_DIR" ca.crt
    else
        # shellcheck disable=SC2086 -- $names is a list of known-safe generated filenames
        tar -czf "$out" -C "$pool_dir" $names
    fi
    printf 'Exported %d slot(s) to %s\n' "$(printf '%s\n' $slots | wc -l | tr -d ' ')" "$out"
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

usage() {
    cat <<'EOF'
Usage: scripts/nad-provision.sh COMMAND [OPTIONS]

Commands:
  init    --nad-host HOST [--state-dir DIR]
          One-time: generate the CA and server certificate, install and
          enable the receiver's systemd units.

  seed    --pool NAME --count N [--state-dir DIR]
          One-time per pool: issue exactly N permanent client certs plus
          one self-contained bootstrap script per slot. Re-running with
          the same --count is a no-op; a different --count is an error.

  occupy  --pool NAME --slots SPEC [--state-dir DIR]
  release --pool NAME --slots SPEC [--state-dir DIR]
          Manually flip a slot (or comma/range list of slots, e.g.
          "1,3,5-9") between free and occupied. Bookkeeping only.

  status  --pool NAME [--state-dir DIR]
          List every slot in a pool with its free/occupied status.

  export  --pool NAME [--slots SPEC] --out FILE [--state-dir DIR]
          Package bootstrap scripts (default: the whole pool) into a
          tar.gz. Never reads or changes free/occupied status.

  -h, --help
          Show this help.

--state-dir defaults to $HOME/mirror-certs.
EOF
}

require_state_dir_option() {
    [ "$#" -ge 2 ] || { echo "--state-dir requires a value" >&2; exit 2; }
}

cmd_init() {
    STATE_DIR="$HOME/mirror-certs"
    NAD_HOST=
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
            --state-dir)
                require_state_dir_option "$@"
                STATE_DIR=$2
                shift 2
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    [ -n "$NAD_HOST" ] || { echo "--nad-host is required" >&2; exit 2; }
    [ "$(id -u)" -eq 0 ] || { echo "must run as root" >&2; exit 1; }
    check_receiver_installed
    mkdir -p "$STATE_DIR"
    chmod 700 "$STATE_DIR"
    check_nad_host_stable
    ensure_ca
    ensure_server_cert
    install_receiver_files
    enable_receiver_services
    printf 'nad initialized: CA and server cert in %s, receiver enabled.\n' "$STATE_DIR"
}

cmd_seed() {
    STATE_DIR="$HOME/mirror-certs"
    POOL=
    COUNT=
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --pool)
                [ "$#" -ge 2 ] || { echo "--pool requires a value" >&2; exit 2; }
                POOL=$2
                shift 2
                ;;
            --count)
                [ "$#" -ge 2 ] || { echo "--count requires a value" >&2; exit 2; }
                is_positive_int "$2" || { echo "--count must be a positive integer" >&2; exit 2; }
                COUNT=$2
                shift 2
                ;;
            --state-dir)
                require_state_dir_option "$@"
                STATE_DIR=$2
                shift 2
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    [ -n "$POOL" ] || { echo "--pool is required" >&2; exit 2; }
    [ -n "$COUNT" ] || { echo "--count is required" >&2; exit 2; }
    seed_pool "$POOL" "$COUNT"
}

cmd_occupy_or_release() {
    action=$1
    shift
    STATE_DIR="$HOME/mirror-certs"
    POOL=
    SLOTS=
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --pool)
                [ "$#" -ge 2 ] || { echo "--pool requires a value" >&2; exit 2; }
                POOL=$2
                shift 2
                ;;
            --slots)
                [ "$#" -ge 2 ] || { echo "--slots requires a value" >&2; exit 2; }
                SLOTS=$2
                shift 2
                ;;
            --state-dir)
                require_state_dir_option "$@"
                STATE_DIR=$2
                shift 2
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    [ -n "$POOL" ] || { echo "--pool is required" >&2; exit 2; }
    [ -n "$SLOTS" ] || { echo "--slots is required" >&2; exit 2; }
    if [ "$action" = occupy ]; then
        occupy_pool "$POOL" "$SLOTS"
    else
        release_pool "$POOL" "$SLOTS"
    fi
}

cmd_occupy() { cmd_occupy_or_release occupy "$@"; }
cmd_release() { cmd_occupy_or_release release "$@"; }

cmd_status() {
    STATE_DIR="$HOME/mirror-certs"
    POOL=
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --pool)
                [ "$#" -ge 2 ] || { echo "--pool requires a value" >&2; exit 2; }
                POOL=$2
                shift 2
                ;;
            --state-dir)
                require_state_dir_option "$@"
                STATE_DIR=$2
                shift 2
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    [ -n "$POOL" ] || { echo "--pool is required" >&2; exit 2; }
    status_pool "$POOL"
}

cmd_export() {
    STATE_DIR="$HOME/mirror-certs"
    POOL=
    SLOTS=
    OUT=
    while [ "$#" -gt 0 ]; do
        case "$1" in
            --pool)
                [ "$#" -ge 2 ] || { echo "--pool requires a value" >&2; exit 2; }
                POOL=$2
                shift 2
                ;;
            --slots)
                [ "$#" -ge 2 ] || { echo "--slots requires a value" >&2; exit 2; }
                SLOTS=$2
                shift 2
                ;;
            --out)
                [ "$#" -ge 2 ] || { echo "--out requires a value" >&2; exit 2; }
                OUT=$2
                shift 2
                ;;
            --state-dir)
                require_state_dir_option "$@"
                STATE_DIR=$2
                shift 2
                ;;
            *)
                echo "Unknown option: $1" >&2
                exit 2
                ;;
        esac
    done
    [ -n "$POOL" ] || { echo "--pool is required" >&2; exit 2; }
    [ -n "$OUT" ] || { echo "--out is required" >&2; exit 2; }
    export_pool "$POOL" "$SLOTS" "$OUT"
}

main() {
    [ "$#" -ge 1 ] || { usage >&2; exit 2; }
    cmd=$1
    shift
    case "$cmd" in
        init) cmd_init "$@" ;;
        seed) cmd_seed "$@" ;;
        occupy) cmd_occupy "$@" ;;
        release) cmd_release "$@" ;;
        status) cmd_status "$@" ;;
        export) cmd_export "$@" ;;
        -h|--help) usage; exit 0 ;;
        *)
            echo "Unknown command: $cmd" >&2
            usage >&2
            exit 2
            ;;
    esac
}

if [ "${MIRROR_PROVISION_SOURCE:-0}" != "1" ]; then
    main "$@"
fi
