#!/bin/bash

# Prepare Alpine userspace, bootstrap the cross-toolchain, and build Pedigree.
set -e

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
invoking_dir=$PWD
cd "$script_dir"

COMPILER_DIR=${PEDIGREE_TOOLCHAIN_ROOT:-$script_dir/pedigree-compiler-15.3.0-r2}
alpine_root=${PEDIGREE_ALPINE_ROOT:-$script_dir/scripts/alpine/build/x86_64}
case $COMPILER_DIR in
    /*) ;;
    *) COMPILER_DIR=$invoking_dir/$COMPILER_DIR ;;
esac
case $alpine_root in
    /*) ;;
    *) alpine_root=$invoking_dir/$alpine_root ;;
esac
target_sysroot=${PEDIGREE_TARGET_SYSROOT:-$alpine_root/sysroot}
case $target_sysroot in
    /*) ;;
    *) target_sysroot=$invoking_dir/$target_sysroot ;;
esac
profile=${PEDIGREE_ALPINE_PROFILE:-base}
case "$profile" in
    base) desktop=OFF ;;
    desktop) desktop=ON ;;
    *) echo "Unknown Alpine profile: $profile (expected base or desktop)" >&2; exit 2 ;;
esac

[ -d ".venv" ] || uv venv
. "$script_dir/scripts/easy_build_deps.sh"
git submodule update --init

"$script_dir/scripts/alpine/build.sh" x86_64 "$alpine_root" --profile "$profile"
alpine_root=$(cd -P -- "$alpine_root" && pwd -P)
target_sysroot=$(cd -P -- "$target_sysroot" && pwd -P)

# The bootstrap checks its recipe before reusing an installed compiler/runtime.
uv run python "$script_dir/scripts/bootstrap_toolchain.py" \
    x86_64-pedigree "$COMPILER_DIR" --source-root "$script_dir" \
    --sysroot "$target_sysroot/usr" --activate --libcpp
COMPILER_DIR=$(cd -P -- "$COMPILER_DIR" && pwd -P)

refresh_cmake_metadata=false
if [ -f build/CMakeCache.txt ]; then
    if ! grep -Fqx \
        "PEDIGREE_TOOLCHAIN_ROOT:PATH=$COMPILER_DIR" build/CMakeCache.txt; then
        refresh_cmake_metadata=true
    elif ! grep -Fqx \
        "PEDIGREE_TARGET_SYSROOT:PATH=$target_sysroot" build/CMakeCache.txt; then
        refresh_cmake_metadata=true
    elif ! grep -Fqs 'set(CMAKE_SYSTEM_NAME "Pedigree")' \
        build/CMakeFiles/*/CMakeSystem.cmake 2>/dev/null; then
        refresh_cmake_metadata=true
    fi
fi

if [ "$refresh_cmake_metadata" = true ]; then
    cmake -E rm -f build/CMakeCache.txt
    cmake -E remove_directory build/CMakeFiles
fi
cmake -S "$script_dir" -B "$script_dir/build" \
    -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_amd64.cmake" \
    -DPEDIGREE_TOOLCHAIN_ROOT:PATH="$COMPILER_DIR" \
    -DPEDIGREE_TARGET_SYSROOT:PATH="$target_sysroot" \
    -DPEDIGREE_ALPINE_ROOT:PATH="$alpine_root" \
    -DPEDIGREE_ALPINE_PROFILE="$profile" \
    -DPEDIGREE_GRAPHICS="$desktop" \
    -DPEDIGREE_BUILD_USER_DIR="$desktop" \
    -DPEDIGREE_BUILD_KEYMAPS="$desktop" \
    -DPEDIGREE_BUILD_TRANSLATIONS="$desktop" \
    -DPEDIGREE_BUILD_EXTERNAL_WINMAN="$desktop"
cmake --build "$script_dir/build" --parallel "${PEDIGREE_BUILD_JOBS:-4}"

echo "Pedigree is ready. Incremental builds: cmake --build build"
