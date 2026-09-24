#!/usr/bin/env bash
set -euo pipefail

repository=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
toolchain_root=${PEDIGREE_TOOLCHAIN_ROOT:-$repository/pedigree-compiler-15.3.0-r2}
alpine_root=${PEDIGREE_ALPINE_ROOT:-$repository/scripts/alpine/build/armv7}
case "$alpine_root" in
    /*) ;;
    *) alpine_root=$PWD/$alpine_root ;;
esac
target_sysroot=${PEDIGREE_TARGET_SYSROOT:-$alpine_root/sysroot}
boot_profile=${PEDIGREE_ARMV7_BOOT_PROFILE:-alpine}
compiler_target=armv7-alpine-linux-musleabihf
compiler="$toolchain_root/bin/$compiler_target-gcc"
compiler_cxx="$toolchain_root/bin/$compiler_target-g++"
source_dir="$repository/src/system/kernel/core/processor/armv7"

case "$boot_profile" in
    alpine|bootstrap) ;;
    *) echo "Unknown ARMv7 boot profile: $boot_profile (expected alpine or bootstrap)" >&2; exit 2 ;;
esac

"$repository/scripts/alpine/build.sh" armv7 "$alpine_root" --profile base
alpine_root=$(cd -P -- "$alpine_root" && pwd -P)
target_sysroot=$(cd -P -- "$target_sysroot" && pwd -P)
uv run python "$repository/scripts/bootstrap_toolchain.py" \
    "$compiler_target" "$toolchain_root" --source-root "$repository" \
    --sysroot "$target_sysroot/usr" --libcpp
toolchain_root=$(cd -P -- "$toolchain_root" && pwd -P)
compiler="$toolchain_root/bin/$compiler_target-gcc"
compiler_cxx="$toolchain_root/bin/$compiler_target-g++"

if [[ $boot_profile == alpine ]]; then
    build_dir=${PEDIGREE_ARMV7_BUILD_DIR:-$repository/build-armv7/full}
    if [[ -f $build_dir/CMakeCache.txt ]] && \
       { ! grep -Fqx "PEDIGREE_TOOLCHAIN_ROOT:PATH=$toolchain_root" "$build_dir/CMakeCache.txt" || \
         ! grep -Fqx "PEDIGREE_TARGET_SYSROOT:PATH=$target_sysroot" "$build_dir/CMakeCache.txt"; }; then
        cmake -E rm -f "$build_dir/CMakeCache.txt"
        cmake -E remove_directory "$build_dir/CMakeFiles"
    fi
    cmake -S "$repository" -B "$build_dir" \
        -U 'PEDIGREE_MODULE_*' \
        -DCMAKE_TOOLCHAIN_FILE="$repository/build-etc/cmake/pedigree_armv7.cmake" \
        -DPEDIGREE_TOOLCHAIN_ROOT:PATH="$toolchain_root" \
        -DPEDIGREE_TARGET_SYSROOT:PATH="$target_sysroot" \
        -DPEDIGREE_ALPINE_ROOT:PATH="$alpine_root" \
        -DPEDIGREE_ALPINE_PROFILE=base \
        -DBUILD_TESTING=OFF \
        -DPEDIGREE_BUILD_USER_DIR=OFF \
        -DPEDIGREE_STATIC_DRIVERS=ON \
        -DPEDIGREE_MULTIPROCESSOR=OFF \
        -DPEDIGREE_DEBUGGER=OFF \
        -DPEDIGREE_GRAPHICS=OFF \
        -DPEDIGREE_ARMV7_ALPINE=ON \
        -DPEDIGREE_BUILD_UEFI=ON \
        -DPEDIGREE_WITH_INIT=ON
    cmake --build "$build_dir" --target boot-artifacts -j "${PEDIGREE_BUILD_JOBS:-4}"
    exit 0
fi

build_dir=${PEDIGREE_ARMV7_BUILD_DIR:-$repository/build-armv7}
mkdir -p "$build_dir"
cat > "$build_dir/config.h" <<'EOF'
#define ARMV7 1
#define ARM64 0
#define X64 0
#define X86 0
#define X86_COMMON 0
#define HOSTED 0
#define MACH_VIRT 1
#define BITS_32 1
#define BITS_64 0
EOF
common=(-DARMV7_BOOTSTRAP -march=armv7-a -marm -mfpu=vfpv3-d16 -mfloat-abi=hard
        -ffreestanding -fno-builtin -fno-pic -fno-pie -g)
"$compiler" "${common[@]}" -c "$source_dir/boot.S" -o "$build_dir/boot.o"
"$compiler_cxx" "${common[@]}" -O2 -Wall -Wextra -Werror -fno-exceptions \
    -fno-rtti -fno-stack-protector -I "$build_dir" \
    -I "$repository/src/system/include" \
    -c "$source_dir/BootMain.cc" -o "$build_dir/BootMain.o"
"$compiler_cxx" "${common[@]}" -O2 -fno-exceptions -fno-rtti \
    -I "$build_dir" -I "$repository/src/system/include" \
    -c "$repository/src/system/kernel/core/BootstrapInfo.cc" \
    -o "$build_dir/BootstrapInfo.o"
"$compiler_cxx" "${common[@]}" -O2 -fno-exceptions -fno-rtti \
    -I "$build_dir" -I "$repository/src/system/include" \
    -c "$source_dir/BootstrapStub.cc" -o "$build_dir/BootstrapStub.o"
"$compiler_cxx" "${common[@]}" -nostdlib -nostartfiles -static -no-pie \
    -Wl,--build-id=none -Wl,-T,"$source_dir/kernel.ld" \
    "$build_dir/boot.o" "$build_dir/BootMain.o" "$build_dir/BootstrapInfo.o" \
    "$build_dir/BootstrapStub.o" \
    -o "$build_dir/armv7-bootstrap.elf"
