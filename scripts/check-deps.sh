#!/bin/sh
set -u

ok=0
missing=0

say_ok() {
    printf 'OK      %s\n' "$1"
    ok=$((ok + 1))
}

say_warn() {
    printf 'WARN    %s\n' "$1"
}

say_fail() {
    printf 'MISSING %s\n' "$1"
    missing=$((missing + 1))
}

need_cmd() {
    if command -v "$1" >/dev/null 2>&1; then
        say_ok "$1: $(command -v "$1")"
    else
        say_fail "$1 command"
    fi
}

need_header() {
    hdr=$1
    code="#include <$hdr>
int main(void) { return 0; }"
    tmp=${TMPDIR:-/tmp}/mirror-check-$$
    printf '%s\n' "$code" > "$tmp.c"
    if ${CC:-cc} -c "$tmp.c" -o "$tmp.o" >/dev/null 2>&1; then
        say_ok "header <$hdr>"
    else
        say_fail "header <$hdr>"
    fi
    rm -f "$tmp.c" "$tmp.o"
}

need_openssl_link() {
    tmp=${TMPDIR:-/tmp}/mirror-check-ssl-$$
    cat > "$tmp.c" <<'EOF'
#include <openssl/ssl.h>
int main(void) {
    SSL_CTX *ctx = SSL_CTX_new(TLS_method());
    SSL_CTX_free(ctx);
    return 0;
}
EOF
    if ${CC:-cc} "$tmp.c" -o "$tmp" -lssl -lcrypto >/dev/null 2>&1; then
        say_ok "OpenSSL headers and libssl/libcrypto link"
    else
        say_fail "OpenSSL development package link test"
    fi
    rm -f "$tmp.c" "$tmp"
}

need_xxhash_link() {
    tmp=${TMPDIR:-/tmp}/mirror-check-xxhash-$$
    cat > "$tmp.c" <<'EOF'
#include <xxhash.h>
int main(void) {
    XXH128_hash_t h = XXH3_128bits("mirror", 6);
    return h.low64 == 0 && h.high64 == 0;
}
EOF
    if ${CC:-cc} "$tmp.c" -o "$tmp" -lxxhash >/dev/null 2>&1; then
        say_ok "xxHash headers and libxxhash link"
    else
        say_fail "xxHash development package link test"
    fi
    rm -f "$tmp.c" "$tmp"
}

printf 'Mirror PoC dependency check\n'
printf '===========================\n'

need_cmd cc
need_cmd gcc
need_cmd make
need_cmd cmake
need_cmd openssl
need_cmd ip
need_cmd systemctl
need_cmd pkg-config

need_header linux/if_packet.h
need_header linux/if_ether.h
need_header net/if.h
need_header openssl/ssl.h
need_header xxhash.h
need_openssl_link
need_xxhash_link

if [ "$(id -u)" -eq 0 ]; then
    say_ok "running as root for runtime checks"
else
    say_warn "not root: build is fine, but runtime capture/injection needs root or CAP_NET_RAW"
fi

if [ -d /sys/class/net ]; then
    say_ok "/sys/class/net exists"
else
    say_fail "/sys/class/net"
fi

printf '\nSummary: %d OK, %d missing\n' "$ok" "$missing"

if [ "$missing" -ne 0 ]; then
    cat <<'EOF'

Likely package names:
  Debian/Ubuntu:
    sudo apt-get install build-essential cmake libssl-dev libxxhash-dev iproute2 systemd pkg-config

  RHEL/CentOS/Fedora:
    sudo dnf install gcc make cmake openssl-devel xxhash-devel iproute systemd pkgconf-pkg-config

Runtime notes:
  mirror-agent needs CAP_NET_RAW for AF_PACKET capture.
  mirror-receiver needs CAP_NET_RAW for AF_PACKET injection.
  mirror-interface.service/setup script needs CAP_NET_ADMIN to create veth interfaces.
  mTLS cert/key files must exist before starting services.
EOF
    exit 1
fi

exit 0
