#!/bin/sh

# Build the bootable Pedigree products from a checkout running on Pedigree.

set -eu

script_dir=$(CDPATH= cd -P "$(dirname "$0")" && pwd -P)

if host_system=$(uname -s 2>/dev/null); then
    :
else
    host_system=unknown
fi

case "$host_system" in
    Pedigree|pedigree)
        ;;
    *)
        echo "easy_build_selfhost.sh must be run on Pedigree (found: $host_system)." >&2
        exit 1
        ;;
esac

cmake_command=${PEDIGREE_CMAKE:-cmake}
if ! command -v "$cmake_command" >/dev/null 2>&1; then
    echo "CMake was not found: $cmake_command" >&2
    exit 1
fi
if ! command -v make >/dev/null 2>&1; then
    echo "Required build command was not found: make" >&2
    exit 1
fi

build_dir=${PEDIGREE_BUILD_DIR:-build-selfhost}
case "$build_dir" in
    /*) ;;
    *) build_dir="$script_dir/$build_dir" ;;
esac

jobs=${PEDIGREE_BUILD_JOBS:-1}
case "$jobs" in
    ''|*[!0-9]*|0)
        echo "PEDIGREE_BUILD_JOBS must be a positive integer (found: $jobs)." >&2
        exit 1
        ;;
esac

set --
if [ -n "${PEDIGREE_CMAKE_GENERATOR:-}" ]; then
    set -- "$@" -G "$PEDIGREE_CMAKE_GENERATOR"
fi

set -- "$@" \
    -S "$script_dir" \
    -B "$build_dir" \
    -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_selfhost.cmake" \
    -DCMAKE_BUILD_TYPE="${PEDIGREE_BUILD_TYPE:-Debug}" \
    -DPEDIGREE_BUILD_ROLE=TARGET \
    -DBUILD_TESTING=OFF \
    -DPEDIGREE_NATIVE_TOOL_ROOT="${PEDIGREE_NATIVE_TOOL_ROOT:-/usr}" \
    -DPEDIGREE_BUILD_HDD_IMAGE=OFF \
    -DPEDIGREE_BUILD_ISO=OFF \
    -DPEDIGREE_BUILD_UEFI=OFF \
    -DPEDIGREE_BUILD_KEYMAPS=OFF \
    -DPEDIGREE_BUILD_TRANSLATIONS=OFF \
    -DPEDIGREE_REGENERATE_KEYMAP_SOURCES=OFF

"$cmake_command" "$@"
"$cmake_command" --build "$build_dir" --target boot-artifacts --parallel "$jobs"

echo
echo "Pedigree boot artifacts are ready in $build_dir."
echo "Nothing was installed or written to /boot."
