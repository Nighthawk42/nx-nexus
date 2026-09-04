#!/usr/bin/env bash
# Build NX-Nexus in the devkitPro container.
#
# No local devkitA64 install needed -- just Docker. Works from WSL, Linux and
# macOS. The image is multi-arch, so x86-64 and arm64 hosts both build natively.
#
#   ./scripts/build.sh              # normal build
#   ./scripts/build.sh --strict     # treat warnings as errors (what CI does)
#   ./scripts/build.sh --no-intr-ep # 2-endpoint fallback, no MTP events
#   ./scripts/build.sh --clean      # remove build output first
#   ./scripts/build.sh --shell      # interactive shell in the container
set -euo pipefail

IMAGE="${NEXUS_IMAGE:-devkitpro/devkita64:latest}"
PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"

clean=0
shell=0
cflags_extra=""

for arg in "$@"; do
    case "$arg" in
        --clean)      clean=1 ;;
        --shell)      shell=1 ;;
        --strict)     cflags_extra="$cflags_extra -Werror" ;;
        --no-intr-ep) cflags_extra="$cflags_extra -DNEXUS_USB_NO_INTERRUPT_EP=1" ;;
        -h|--help)    sed -n '2,14p' "${BASH_SOURCE[0]}" | sed 's|^# \?||'; exit 0 ;;
        *)            echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

# Trim the leading space left by appending to an initially empty string, so the
# echoed command below reads correctly and make gets a clean assignment.
cflags_extra="${cflags_extra# }"

if ! docker info >/dev/null 2>&1; then
    echo "error: cannot reach the Docker daemon." >&2
    echo "       Start Docker, or run inside WSL where dockerd is available." >&2
    exit 1
fi

# Parallelism: nproc on Linux, sysctl on macOS, else a safe default.
jobs="$( (nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 4) )"

run() {
    docker run --rm -v "$PROJECT_DIR:/project" -w /project "$IMAGE" "$@"
}

if [ "$shell" -eq 1 ]; then
    exec docker run --rm -it -v "$PROJECT_DIR:/project" -w /project "$IMAGE" bash
fi

if [ "$clean" -eq 1 ]; then
    echo ">> make clean"
    run make clean
fi

echo ">> make -j$jobs${cflags_extra:+ CFLAGS_EXTRA=$cflags_extra}"
run make -j"$jobs" CFLAGS_EXTRA="$cflags_extra"

nro="$PROJECT_DIR/NX-Nexus.nro"
if [ ! -f "$nro" ]; then
    echo "error: build reported success but NX-Nexus.nro is missing" >&2
    exit 1
fi

# An NRO carries its magic at offset 0x10, not 0. A file that links but has a
# bad header will fail silently on the console, so check it here.
magic="$(dd if="$nro" bs=1 skip=16 count=4 2>/dev/null)"
if [ "$magic" != "NRO0" ]; then
    echo "error: bad NRO magic (got '$magic', expected 'NRO0')" >&2
    exit 1
fi

echo
echo "built $nro ($(wc -c <"$nro") bytes), NRO0 header OK"
echo "copy it to sdmc:/switch/NX-Nexus.nro and launch from the homebrew menu"
