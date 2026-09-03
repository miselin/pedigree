#!/bin/sh

# Build the bootable Pedigree products from a checkout running on Pedigree.

set -eu

script_dir=$(CDPATH= cd "$(dirname "$0")" && pwd -P)

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
for required_command in \
    bash make patch cp date grep mkdir mktemp mv rm rmdir; do
    if ! command -v "$required_command" >/dev/null 2>&1; then
        echo "Required build command was not found: $required_command" >&2
        exit 1
    fi
done

build_dir=${PEDIGREE_BUILD_DIR:-build-selfhost}
case "$build_dir" in
    /*) ;;
    *) build_dir="$script_dir/$build_dir" ;;
esac

musl_archive=${PEDIGREE_MUSL_ARCHIVE:-"$script_dir/musl-1.2.6.tar.gz"}
case "$musl_archive" in
    /*) ;;
    *) musl_archive="$script_dir/$musl_archive" ;;
esac
if [ ! -f "$musl_archive" ]; then
    echo "A local musl-1.2.6 archive is required; no download will be attempted." >&2
    echo "Set PEDIGREE_MUSL_ARCHIVE to the archive path (looked for: $musl_archive)." >&2
    exit 1
fi

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
    -DPEDIGREE_MUSL_ARCHIVE="$musl_archive" \
    -DPEDIGREE_BUILD_HDD_IMAGE=OFF \
    -DPEDIGREE_BUILD_ISO=OFF \
    -DPEDIGREE_BUILD_KEYMAPS=OFF \
    -DPEDIGREE_BUILD_TRANSLATIONS=OFF \
    -DPEDIGREE_REGENERATE_KEYMAP_SOURCES=OFF

"$cmake_command" "$@"
"$cmake_command" --build "$build_dir" --target boot-artifacts --parallel "$jobs"

echo
echo "Pedigree boot artifacts are ready in $build_dir."
echo "Nothing was installed or written to /boot."
