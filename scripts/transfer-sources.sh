#!/bin/sh
set -eu

usage() {
    cat <<'EOF'
Usage: scripts/transfer-sources.sh USER@HOST [REMOTE_DIR]

Transfer the project sources to a remote Linux host over SSH.
REMOTE_DIR defaults to "traff-mirror" relative to the remote user's home.

Examples:
  ./scripts/transfer-sources.sh ubuntu@192.168.1.36
  ./scripts/transfer-sources.sh debian@192.168.1.78 /home/debian/traff-mirror

The build directory, Git metadata, editor files, logs, packet captures, and
local certificate material are not transferred. Existing remote files are
updated, but unrelated files in REMOTE_DIR are not deleted.
EOF
}

if [ "${1:-}" = "-h" ] || [ "${1:-}" = "--help" ]; then
    usage
    exit 0
fi

if [ "$#" -lt 1 ] || [ "$#" -gt 2 ]; then
    usage >&2
    exit 2
fi

remote=$1
remote_dir=${2:-traff-mirror}

case "$remote" in
    *[!A-Za-z0-9_.@:-]*|'')
        echo "Invalid USER@HOST: $remote" >&2
        exit 2
        ;;
esac

case "$remote_dir" in
    *[!A-Za-z0-9_./-]*|'')
        echo "Invalid REMOTE_DIR: $remote_dir" >&2
        echo "Use a relative path or an absolute path without spaces or '~'." >&2
        exit 2
        ;;
esac

for command in tar ssh; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command not found: $command" >&2
        exit 1
    fi
done

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(dirname "$script_dir")

echo "Transferring sources from $project_dir"
echo "Remote destination: $remote:$remote_dir"

tar \
    --exclude='./build' \
    --exclude='./.git' \
    --exclude='./.idea' \
    --exclude='./.vscode' \
    --exclude='*.o' \
    --exclude='*.log' \
    --exclude='*.pcap' \
    --exclude='*.pcapng' \
    --exclude='*.crt' \
    --exclude='*.key' \
    --exclude='*.csr' \
    --exclude='*.srl' \
    -C "$project_dir" -czf - . \
    | ssh "$remote" \
        "mkdir -p '$remote_dir' && tar -xzf - -C '$remote_dir'"

echo "Transfer complete: $remote:$remote_dir"
echo "Next: ssh $remote 'cd $remote_dir && ./scripts/build.sh --clean'"
