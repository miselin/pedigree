#!/usr/bin/env bash

set -euo pipefail

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
toolchain_root=${PEDIGREE_TOOLCHAIN_ROOT:-$script_dir/pedigree-compiler-15.3.0-r2}
target_sysroot=${PEDIGREE_TARGET_SYSROOT:-$script_dir/scripts/alpine/build/aarch64/sysroot}
build_dir=${PEDIGREE_ARM64_BUILD_DIR:-$script_dir/build-arm64}
boot_profile=${PEDIGREE_ARM64_BOOT_PROFILE:-alpine}
compiler_target=aarch64-linux-musl
toolchain_refreshed=false

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

if [ ! -f "$script_dir/scripts/alpine/build/aarch64/rootfs.img" ] ||
   [ ! -f "$target_sysroot/usr/include/errno.h" ] ||
   [ ! -f "$target_sysroot/usr/lib/libc.a" ]; then
    "$script_dir/scripts/alpine/build.sh" aarch64
fi

if [ ! -x "$toolchain_root/bin/$compiler_target-gcc" ]; then
    python3 "$script_dir/scripts/bootstrap_toolchain.py" \
        "$compiler_target" "$toolchain_root" \
        --source-root "$script_dir" \
        --sysroot "$target_sysroot/usr"
    toolchain_refreshed=true
fi

toolchain_root=$(cd -P -- "$toolchain_root" && pwd -P)
target_sysroot=$(cd -P -- "$target_sysroot" && pwd -P)

gcc_version=$("$toolchain_root/bin/$compiler_target-g++" -dumpfullversion)
if [ ! -f "$toolchain_root/include/c++/$gcc_version/$compiler_target/bits/c++config.h" ]; then
    python3 "$script_dir/scripts/bootstrap_toolchain.py" \
        "$compiler_target" "$toolchain_root" \
        --source-root "$script_dir" \
        --sysroot "$target_sysroot/usr" \
        --libcpp
    toolchain_refreshed=true
fi

# CMake caches compiler and platform identity. Retain build outputs when the
# selected toolchain changes, but refresh the immutable configure metadata.
if [ -f "$build_dir/CMakeCache.txt" ] && \
    { [ "$toolchain_refreshed" = true ] || \
      ! grep -Fqx "PEDIGREE_TOOLCHAIN_ROOT:PATH=$toolchain_root" "$build_dir/CMakeCache.txt" || \
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
