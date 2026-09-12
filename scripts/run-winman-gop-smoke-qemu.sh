#!/bin/bash

set -eu

REPOSITORY=$(cd "$(dirname "$0")/.." && pwd)
MARKER_DIR="$REPOSITORY/images/local/etc"
MARKER="$MARKER_DIR/winman-gop-smoke"
CREATED_MARKER=0

cleanup_marker() {
    if [ "$CREATED_MARKER" -eq 1 ]; then
        rm -f "$MARKER"
        rmdir "$MARKER_DIR" 2>/dev/null || true
    fi
}

trap cleanup_marker EXIT HUP INT TERM

mkdir -p "$MARKER_DIR"
if [ ! -e "$MARKER" ]; then
    touch "$MARKER"
    CREATED_MARKER=1
fi

# The image custom command does not track arbitrary files in images/local.
rm -f "$REPOSITORY/build/pedigree-uefi-root.img" \
    "$REPOSITORY/build/pedigree-uefi.img"
cmake --build "$REPOSITORY/build" --target uefi-image -j2

cleanup_marker
CREATED_MARKER=0
cd "$REPOSITORY"
exec "$REPOSITORY/scripts/qemu" --serial
