#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
toolchain_root=${PEDIGREE_TOOLCHAIN_ROOT:-$script_dir/pedigree-compiler-15.3.0-r2}
alpine_root=${PEDIGREE_ALPINE_ROOT:-$script_dir/scripts/alpine/build/aarch64}
case "$alpine_root" in
    /*) ;;
    *) alpine_root=$PWD/$alpine_root ;;
esac
target_sysroot=${PEDIGREE_TARGET_SYSROOT:-$alpine_root/sysroot}
build_dir=${PEDIGREE_ARM64_BUILD_DIR:-$script_dir/build-arm64}
boot_profile=${PEDIGREE_ARM64_BOOT_PROFILE:-alpine}
compiler_target=aarch64-linux-musl

case "$boot_profile" in
    alpine)
        alpine_modules=ON
        with_init=ON
        graphics=ON
        ;;
    serial)
        alpine_modules=OFF
        with_init=OFF
        graphics=OFF
        ;;
    *)
        echo "Unknown ARM64 boot profile: $boot_profile (expected alpine or serial)" >&2
        exit 2
        ;;
esac

"$script_dir/scripts/alpine/build.sh" aarch64 "$alpine_root" --profile base
alpine_root=$(cd -P -- "$alpine_root" && pwd -P)
target_sysroot=$(cd -P -- "$target_sysroot" && pwd -P)
uv run python "$script_dir/scripts/bootstrap_toolchain.py" \
    "$compiler_target" "$toolchain_root" --source-root "$script_dir" \
    --sysroot "$target_sysroot/usr" --libcpp
toolchain_root=$(cd -P -- "$toolchain_root" && pwd -P)
gcc_version=$("$toolchain_root/bin/$compiler_target-g++" -dumpfullversion)

# CMake caches compiler and platform identity. Retain build outputs when the
# selected toolchain changes, but refresh the immutable configure metadata.
if [ -f "$build_dir/CMakeCache.txt" ] && \
    { ! grep -Fqx "PEDIGREE_TOOLCHAIN_ROOT:PATH=$toolchain_root" "$build_dir/CMakeCache.txt" || \
      ! grep -Fqx "PEDIGREE_TARGET_SYSROOT:PATH=$target_sysroot" "$build_dir/CMakeCache.txt" || \
      ! grep -Fqs "$toolchain_root/include/c++/$gcc_version/$compiler_target" \
          "$build_dir"/CMakeFiles/*/CMakeCXXCompiler.cmake; }; then
    cmake -E rm -f "$build_dir/CMakeCache.txt"
    cmake -E remove_directory "$build_dir/CMakeFiles"
fi

cmake -S "$script_dir" -B "$build_dir" \
    -U 'PEDIGREE_MODULE_*' \
    -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_arm64.cmake" \
    -DPEDIGREE_TOOLCHAIN_ROOT:PATH="$toolchain_root" \
    -DPEDIGREE_TARGET_SYSROOT:PATH="$target_sysroot" \
    -DPEDIGREE_ALPINE_ROOT:PATH="$alpine_root" \
    -DPEDIGREE_ALPINE_PROFILE=base \
    -DBUILD_TESTING=OFF \
    -DPEDIGREE_BUILD_USER_DIR=OFF \
    -DPEDIGREE_BUILD_UEFI=ON \
    -DPEDIGREE_STATIC_DRIVERS=ON \
    -DPEDIGREE_MULTIPROCESSOR=OFF \
    -DPEDIGREE_DEBUGGER=OFF \
    -DPEDIGREE_GRAPHICS="$graphics" \
    -DPEDIGREE_ARM64_ALPINE="$alpine_modules" \
    -DPEDIGREE_WITH_INIT="$with_init"

cmake --build "$build_dir" --target boot-artifacts \
    -j "${PEDIGREE_BUILD_JOBS:-4}"
