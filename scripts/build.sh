#!/bin/sh
set -eu

usage() {
    cat <<'EOF'
Usage: scripts/build.sh [OPTIONS]

Build mirror-agent and mirror-receiver on the current machine.

Options:
  --clean       Remove the previous build first
  --no-tests    Do not run unit tests after the build
  --install     Install binaries and systemd units with CMake
  --jobs N      Use N parallel compiler jobs
  -h, --help    Show this help

Examples:
  ./scripts/build.sh --clean
  ./scripts/build.sh --clean --jobs 4
  ./scripts/build.sh --clean --install
EOF
}

clean=false
run_tests=true
install=false
jobs=

while [ "$#" -gt 0 ]; do
    case "$1" in
        --clean)
            clean=true
            ;;
        --no-tests)
            run_tests=false
            ;;
        --install)
            install=true
            ;;
        --jobs)
            shift
            if [ "$#" -eq 0 ]; then
                echo "--jobs requires a positive integer" >&2
                exit 2
            fi
            jobs=$1
            case "$jobs" in
                *[!0-9]*|0|'')
                    echo "Invalid job count: $jobs" >&2
                    exit 2
                    ;;
            esac
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
    shift
done

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
project_dir=$(dirname "$script_dir")
cd "$project_dir"

if [ -z "$jobs" ]; then
    if command -v nproc >/dev/null 2>&1; then
        jobs=$(nproc)
    else
        jobs=1
    fi
fi

echo "Project: $project_dir"
echo "Checking build dependencies..."
./scripts/check-deps.sh

if [ "$clean" = true ]; then
    echo "Removing previous build..."
    rm -rf build-cmake
fi

echo "Building mirror-agent and mirror-receiver with $jobs job(s)..."
cmake -S . -B build-cmake -DCMAKE_BUILD_TYPE=RelWithDebInfo
cmake --build build-cmake --parallel "$jobs"

if [ "$run_tests" = true ]; then
    echo "Running unit tests..."
    ctest --test-dir build-cmake --output-on-failure
fi

if [ "$install" = true ]; then
    echo "Installing binaries and systemd units..."
    sudo cmake --install build-cmake
    sudo systemctl daemon-reload
fi

echo
echo "Build complete:"
ls -lh build-cmake/bin/mirror-agent build-cmake/bin/mirror-receiver

if [ "$install" = false ]; then
    echo "Install later with: sudo cmake --install build-cmake"
fi
