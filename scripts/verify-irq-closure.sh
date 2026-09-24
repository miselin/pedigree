#!/usr/bin/env bash

# Build and run the focused Linux-hosted IRQ and scheduler closure lane.

set -Eeuo pipefail

repository=$(cd -P -- "$(dirname -- "$0")/.." && pwd -P)
build_root=${PEDIGREE_IRQ_CLOSURE_BUILD_ROOT:-"$repository/build-verify/irq-closure"}

if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
    echo "IRQ closure verification requires a native x86-64 Linux host." >&2
    echo "Use ./verify.sh for the maintained validation on this host." >&2
    exit 2
fi
for command in cmake ctest grep python3; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required command is unavailable: $command" >&2
        exit 1
    fi
done

if [[ -n "${PEDIGREE_VERIFY_JOBS:-}" ]]; then
    if [[ ! "$PEDIGREE_VERIFY_JOBS" =~ ^[1-9][0-9]*$ ]]; then
        echo "PEDIGREE_VERIFY_JOBS must be a positive integer." >&2
        exit 2
    fi
    parallel_args=(--parallel "$PEDIGREE_VERIFY_JOBS")
else
    parallel_args=(--parallel)
fi

host_build="$build_root/host"
kernel_build="$build_root/linux-hosted"
log_file="$build_root/hosted-irq-closure.log"
mkdir -p "$build_root"

run_logged()
{
    local output=$1
    shift
    if ! "$@" >"$output" 2>&1; then
        cat "$output"
        return 1
    fi
}

echo "Configuring native host utilities."
run_logged "$build_root/host-configure.log" \
    cmake -S "$repository" -B "$host_build" \
    -DPEDIGREE_WARNINGS=ON -DPEDIGREE_BUILDUTILS_ASAN=OFF
run_logged "$build_root/host-build.log" \
    cmake --build "$host_build" "${parallel_args[@]}" \
    --target testsuite headerify pedigree-initrd-builder
run_logged "$build_root/host-tests.log" \
    ctest --test-dir "$host_build" --output-on-failure --no-tests=error

echo "Configuring the Linux-hosted IRQ closure kernel."
run_logged "$build_root/linux-hosted-configure.log" \
    cmake -S "$repository" -B "$kernel_build" \
    -DCMAKE_TOOLCHAIN_FILE="$repository/build-etc/cmake/pedigree_hosted.cmake" \
    -DPEDIGREE_HOST_TOOLS_MODE=IMPORTED \
    -DIMPORT_EXECUTABLES="$host_build/HostUtilities.cmake" \
    -DPEDIGREE_BUILD_USER_DIR=OFF \
    -DPEDIGREE_HOSTED_DYNAMIC_MODULES=ON \
    -DPEDIGREE_HOSTED_SMOKE_TESTS=ON \
    -DPEDIGREE_HOSTED_IRQ_CLOSURE_TESTS=ON \
    -DPEDIGREE_HOSTED_SYSTEM_MALLOC=ON \
    -DPEDIGREE_WARNINGS=ON \
    -DPEDIGREE_WITH_INIT=OFF
run_logged "$build_root/linux-hosted-build.log" \
    cmake --build "$kernel_build" "${parallel_args[@]}" \
    --target kernel

"$repository/scripts/test-hosted-irq-closure.sh" \
    "$kernel_build/src/system/kernel/kernel" \
    "$log_file"
