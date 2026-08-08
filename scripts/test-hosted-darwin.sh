#!/usr/bin/env bash

# Run the focused x86-64 hosted kernel lifecycle on Apple silicon through
# Rosetta. The kernel is Mach-O; its smoke module remains Pedigree ELF.

set -Eeuo pipefail

if (( $# != 4 )); then
    echo "usage: scripts/test-hosted-darwin.sh KERNEL MODULE CONFIGDB LOG" >&2
    exit 2
fi

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
kernel=$1
module=$2
configdb=$3
log_file=$4

if [[ $(uname -s) != Darwin ]]; then
    echo "The Darwin hosted lifecycle can only run on macOS." >&2
    exit 2
fi

for command in arch grep python3 tar tee; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required host command is unavailable: $command" >&2
        exit 1
    fi
done

for artifact in "$kernel" "$module" "$configdb"; do
    if [[ ! -f "$artifact" ]]; then
        echo "Required hosted artifact is unavailable: $artifact" >&2
        exit 1
    fi
done

if ! arch -x86_64 /usr/bin/true; then
    echo "Rosetta x86-64 execution is unavailable." >&2
    exit 1
fi

mkdir -p "$(dirname -- "$log_file")"
initrd="$(dirname -- "$log_file")/hosted-core-initrd.tar"
COPYFILE_DISABLE=1 /usr/bin/tar --format=ustar -cf "$initrd" \
    -C "$(dirname -- "$module")" "$(basename -- "$module")"

timeout_seconds=${PEDIGREE_HOSTED_DARWIN_TIMEOUT_SECONDS:-120}
if [[ ! "$timeout_seconds" =~ ^[1-9][0-9]*$ ]]; then
    echo "PEDIGREE_HOSTED_DARWIN_TIMEOUT_SECONDS must be a positive integer." >&2
    exit 2
fi

run_status=0
python3 "$script_dir/run-with-deadline.py" \
    --seconds "$timeout_seconds" --label "Darwin hosted core smoke" -- \
    arch -x86_64 "$kernel" "$initrd" "$configdb" \
    2>&1 | tee "$log_file" || run_status=$?
if (( run_status != 0 )); then
    echo "Darwin hosted core smoke failed with status $run_status." >&2
    exit "$run_status"
fi

required_markers=(
    "KERNELELF: Preloaded module hosted-core-smoke"
    "HOSTED-WAIT-TEST: PASS all"
    "HOSTED-SMOKE: Darwin core smoke executed"
    "HOSTED-SHUTDOWN: timers and signals quiesced"
    "All modules unloaded. Running destructors and terminating"
    "main() returned, cleaning up"
)
for marker in "${required_markers[@]}"; do
    if ! grep -aFq "$marker" "$log_file"; then
        echo "Darwin hosted log is missing marker: $marker" >&2
        exit 1
    fi
done

rejected_markers=(
    "HOSTED-WAIT-TEST: FAIL"
    "KERNELELF: Hit an invalid module"
    "KERNELELF: Module relocation failed"
    "Page Fault Exception"
)
for marker in "${rejected_markers[@]}"; do
    if grep -aFq "$marker" "$log_file"; then
        echo "Darwin hosted log contains failure marker: $marker" >&2
        exit 1
    fi
done

echo "Darwin hosted core lifecycle passed."
