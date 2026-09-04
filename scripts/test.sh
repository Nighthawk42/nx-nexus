#!/usr/bin/env bash
# Run the host-side unit tests for the container parsers.
#
# These need only a normal C compiler -- the parsers depend on
# nexus/nx_types.h rather than libnx, so no devkitA64 or Docker is involved.
#
#   ./scripts/test.sh              # build and run, with ASan + UBSan
#   ./scripts/test.sh --no-sanitize  # skip sanitizers (older toolchains)
#   ./scripts/test.sh --clean      # rebuild from scratch first
set -euo pipefail

PROJECT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
TESTS_DIR="$PROJECT_DIR/tests"

sanitize=1
clean=0

for arg in "$@"; do
    case "$arg" in
        --clean)        clean=1 ;;
        --no-sanitize)  sanitize=0 ;;
        -h|--help)      sed -n '2,10p' "${BASH_SOURCE[0]}" | sed 's|^# \?||'; exit 0 ;;
        *)              echo "unknown option: $arg" >&2; exit 2 ;;
    esac
done

if ! command -v "${CC:-cc}" >/dev/null 2>&1; then
    echo "error: no C compiler found (set CC, or install build-essential)" >&2
    exit 1
fi

[ "$clean" -eq 1 ] && make -C "$TESTS_DIR" clean

make -C "$TESTS_DIR" run SANITIZE="$sanitize"
