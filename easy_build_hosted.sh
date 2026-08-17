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

for command in cmake ctest grep; do
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
darwin_hosted_build_dir="$build_root/darwin-hosted"
toolchain_root=${PEDIGREE_TOOLCHAIN_ROOT:-"$script_dir/compilers/dir"}
cmake_options=(
    -DCMAKE_BUILD_TYPE=Debug
    -DPEDIGREE_WARNINGS=ON
)

echo
echo "Configuring native kernel support and utilities."
cmake -S "$script_dir" -B "$regular_build_dir" \
    "${cmake_options[@]}" -DPEDIGREE_BUILDUTILS_ASAN=OFF
cmake --build "$regular_build_dir" "${parallel_args[@]}" \
    --target testsuite headerify ext2img keymap memorytracer
ctest --test-dir "$regular_build_dir" \
    --output-on-failure --no-tests=error

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

    echo
    echo "Configuring the x86-64 Darwin hosted core lifecycle."
    cmake -S "$script_dir" -B "$darwin_hosted_build_dir" \
        -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_hosted_darwin.cmake" \
        -DIMPORT_EXECUTABLES="$regular_build_dir/HostUtilities.cmake" \
        -DPEDIGREE_TOOLCHAIN_ROOT="$toolchain_root" \
        -DCMAKE_BUILD_TYPE=Debug \
        -DPEDIGREE_BUILD_USER_DIR=OFF \
        -DPEDIGREE_HOSTED_DYNAMIC_MODULES=ON \
        -DPEDIGREE_HOSTED_SMOKE_TESTS=ON \
        -DPEDIGREE_HOSTED_SYSTEM_MALLOC=ON \
        -DPEDIGREE_WARNINGS=ON \
        -DPEDIGREE_WITH_INIT=OFF
    cmake --build "$darwin_hosted_build_dir" "${parallel_args[@]}" \
        --target kernel configdb hosted-core-smoke
    "$script_dir/scripts/test-hosted-darwin.sh" \
        "$darwin_hosted_build_dir/src/system/kernel/kernel" \
        "$darwin_hosted_build_dir/src/modules/hosted-core-smoke.o" \
        "$darwin_hosted_build_dir/config.db" \
        "$darwin_hosted_build_dir/hosted-core-smoke.log"
fi

echo
echo "Configuring native AddressSanitizer validation."
cmake -S "$script_dir" -B "$asan_build_dir" \
    "${cmake_options[@]}" -DPEDIGREE_BUILDUTILS_ASAN=ON

if ! grep -q '^HAVE_ASAN:INTERNAL=1$' "$asan_build_dir/CMakeCache.txt"; then
    echo "The native compiler does not support AddressSanitizer." >&2
    exit 1
fi

cmake --build "$asan_build_dir" "${parallel_args[@]}" --target testsuite
env ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=1:abort_on_error=1:exitcode=99" \
    ctest --test-dir "$asan_build_dir" \
        --output-on-failure --no-tests=error

echo
echo "Host validation passed."
echo "Regular build: $regular_build_dir"
echo "ASan build:    $asan_build_dir"
if [[ $(uname -s) == Darwin ]]; then
    echo "Darwin hosted: $darwin_hosted_build_dir"
fi
echo
echo "This validates native kernel support code and selected module code."
if [[ $(uname -s) == Darwin ]]; then
    echo "It also validates the x86-64 Darwin hosted core lifecycle through Rosetta."
fi
echo "It does not build or run the legacy x86-64 Linux hosted kernel."
