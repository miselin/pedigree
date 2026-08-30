#!/usr/bin/env bash

# Run the maintained host validation directly on macOS or Linux.

set -Eeuo pipefail

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "$script_dir"

if [[ -n "${PEDIGREE_VERIFY_JOBS:-}" ]]; then
    if [[ ! "$PEDIGREE_VERIFY_JOBS" =~ ^[1-9][0-9]*$ ]]; then
        echo "PEDIGREE_VERIFY_JOBS must be a positive integer." >&2
        exit 2
    fi
    parallel_args=(--parallel "$PEDIGREE_VERIFY_JOBS")
else
    parallel_args=(--parallel)
fi

for command in cmake ctest grep python3; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required host command is unavailable: $command" >&2
        exit 1
    fi
done

if [[ ! -f external/googletest/CMakeLists.txt ]]; then
    echo "The googletest submodule is unavailable." >&2
    echo "Initialise repository submodules before running this command." >&2
    exit 1
fi

build_root=${PEDIGREE_NATIVE_BUILD_ROOT:-"$script_dir/build-native"}
regular_build_dir="$build_root/regular"
asan_build_dir="$build_root/asan"
page1k_build_dir="$build_root/page1k"
page16k_build_dir="$build_root/page16k"
page16k_asan_build_dir="$build_root/page16k-asan"
darwin_hosted_build_dir="$build_root/darwin-hosted"
darwin_hosted_page16k_build_dir="$build_root/darwin-hosted-page16k"
toolchain_root=${PEDIGREE_TOOLCHAIN_ROOT:-"$script_dir/compilers/dir"}
cmake_options=(
    -DCMAKE_BUILD_TYPE=Debug
    -DPEDIGREE_WARNINGS=ON
)

python3 "$script_dir/scripts/check-target-page-assumptions.py"

run_native_lane()
{
    local label=$1
    local build_dir=$2
    local page_size=$3
    local use_asan=$4
    local targets=(testsuite)

    echo
    echo "Configuring $label."
    cmake -S "$script_dir" -B "$build_dir" \
        "${cmake_options[@]}" \
        -DPEDIGREE_BUILDUTILS_ASAN="$use_asan" \
        -DPEDIGREE_TARGET_PAGE_SIZE="$page_size"
    grep -q "^#define PEDIGREE_TARGET_PAGE_SIZE $page_size$" \
        "$build_dir/config.h"

    if [[ "$build_dir" == "$regular_build_dir" ]]; then
        targets+=(headerify ext2img keymap memorytracer)
    fi
    cmake --build "$build_dir" "${parallel_args[@]}" --target "${targets[@]}"

    if [[ "$use_asan" == ON ]]; then
        if ! grep -q '^HAVE_ASAN:INTERNAL=1$' "$build_dir/CMakeCache.txt"; then
            echo "The native compiler does not support AddressSanitizer." >&2
            exit 1
        fi
        env ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=1:abort_on_error=1:exitcode=99" \
            ctest --test-dir "$build_dir" \
                --output-on-failure --no-tests=error
    else
        ctest --test-dir "$build_dir" \
            --output-on-failure --no-tests=error
    fi
}

run_darwin_lane()
{
    local label=$1
    local build_dir=$2
    local page_size=$3

    echo
    echo "Configuring $label."
    cmake -S "$script_dir" -B "$build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_hosted_darwin.cmake" \
        -DIMPORT_EXECUTABLES="$regular_build_dir/HostUtilities.cmake" \
        -DPEDIGREE_TOOLCHAIN_ROOT="$toolchain_root" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DPEDIGREE_BUILD_USER_DIR=OFF \
        -DPEDIGREE_HOSTED_DYNAMIC_MODULES=ON \
        -DPEDIGREE_HOSTED_SMOKE_TESTS=ON \
        -DPEDIGREE_HOSTED_SYSTEM_MALLOC=ON \
        -DPEDIGREE_TARGET_PAGE_SIZE="$page_size" \
        -DPEDIGREE_WARNINGS=ON \
        -DPEDIGREE_WITH_INIT=OFF
    grep -q "^#define PEDIGREE_TARGET_PAGE_SIZE $page_size$" \
        "$build_dir/config.h"
    grep -q "ALIGN($page_size)" "$build_dir/src/modules/link.ld"

    cmake --build "$build_dir" "${parallel_args[@]}" \
        --target kernel configdb hosted-core-smoke
    python3 "$script_dir/scripts/check-elf-page-layout.py" \
        --page-size "$page_size" \
        "$build_dir/src/modules/hosted-core-smoke.o"
    "$script_dir/scripts/test-hosted-darwin.sh" \
        "$build_dir/src/system/kernel/kernel" \
        "$build_dir/src/modules/hosted-core-smoke.o" \
        "$build_dir/config.db" \
        "$build_dir/hosted-core-smoke.log" \
        "$page_size"
}

expect_configure_failure()
{
    local name=$1
    local expected_message=$2
    shift 2
    local build_dir="$build_root/contracts/$name"
    local log_file="$build_root/contracts/$name.log"

    if cmake -S "$script_dir" -B "$build_dir" "$@" >"$log_file" 2>&1; then
        echo "Invalid target-page configuration was accepted: $name" >&2
        exit 1
    fi
    grep -q "$expected_message" "$log_file"
}

echo
echo "Checking invalid target-page configurations."
mkdir -p "$build_root/contracts"
expect_configure_failure zero-page \
    "PEDIGREE_TARGET_PAGE_SIZE must be a positive integer" \
    -DPEDIGREE_TARGET_PAGE_SIZE=0
expect_configure_failure nonnumeric-page \
    "PEDIGREE_TARGET_PAGE_SIZE must be a positive integer" \
    -DPEDIGREE_TARGET_PAGE_SIZE=invalid
expect_configure_failure non-power-of-two-page \
    "PEDIGREE_TARGET_PAGE_SIZE must be a power of two" \
    -DPEDIGREE_TARGET_PAGE_SIZE=12288
expect_configure_failure x64-page16k \
    "The x86-64 backend requires 4096-byte base pages" \
    -DPEDIGREE_ARCH_TARGET=X64 \
    -DPEDIGREE_TARGET_PAGE_SIZE=16384

run_native_lane "native 4 KiB kernel support and utilities" \
    "$regular_build_dir" 4096 OFF
run_native_lane "native synthetic 1 KiB target-page validation" \
    "$page1k_build_dir" 1024 OFF
run_native_lane "native synthetic 16 KiB target-page validation" \
    "$page16k_build_dir" 16384 OFF
run_native_lane "native 4 KiB AddressSanitizer validation" \
    "$asan_build_dir" 4096 ON
run_native_lane "native 16 KiB AddressSanitizer validation" \
    "$page16k_asan_build_dir" 16384 ON

if [[ $(uname -s) == Darwin ]]; then
    for command in arch nasm python3 tar tee; do
        if ! command -v "$command" >/dev/null 2>&1; then
            echo "Required Darwin hosted command is unavailable: $command" >&2
            exit 1
        fi
    done
    for tool in \
        "$toolchain_root/bin/x86_64-pedigree-gcc" \
        "$toolchain_root/bin/x86_64-pedigree-g++" \
        "$toolchain_root/bin/x86_64-pedigree-objcopy" \
        "$toolchain_root/bin/x86_64-pedigree-strip"; do
        if [[ ! -x "$tool" ]]; then
            echo "Required Pedigree cross-tool is unavailable: $tool" >&2
            exit 1
        fi
    done

    run_darwin_lane "the x86-64 Darwin hosted 4 KiB core lifecycle" \
        "$darwin_hosted_build_dir" 4096
    run_darwin_lane "the x86-64 Darwin hosted 16 KiB core lifecycle" \
        "$darwin_hosted_page16k_build_dir" 16384
fi

echo
echo "Host validation passed."
echo "Regular build: $regular_build_dir"
echo "1 KiB build:   $page1k_build_dir"
echo "16 KiB build:  $page16k_build_dir"
echo "ASan build:    $asan_build_dir"
echo "16 KiB ASan:   $page16k_asan_build_dir"
if [[ $(uname -s) == Darwin ]]; then
    echo "Darwin hosted 4 KiB:  $darwin_hosted_build_dir"
    echo "Darwin hosted 16 KiB: $darwin_hosted_page16k_build_dir"
fi
echo
echo "This validates native kernel support and selected module code at 1, 4, and 16 KiB."
if [[ $(uname -s) == Darwin ]]; then
    echo "It also validates 4 and 16 KiB x86-64 Darwin hosted core lifecycles through Rosetta."
fi
echo "It does not build or run the legacy x86-64 Linux hosted kernel."
