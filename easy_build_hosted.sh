#!/usr/bin/env bash

# Build and test the kernel support code that runs natively on the host.

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
cmake_options=(
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
echo "Native hosted validation passed."
echo "Regular build: $regular_build_dir"
echo "ASan build:    $asan_build_dir"
echo
echo "This validates native kernel support code and selected module code."
echo "It does not build or run the legacy x86-64 Linux hosted kernel."
