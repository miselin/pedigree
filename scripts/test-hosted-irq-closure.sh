#!/usr/bin/env bash

# Run the focused Linux-hosted IRQ and scheduler closure kernel.

set -Eeuo pipefail

if (( $# != 3 )); then
    echo "usage: scripts/test-hosted-irq-closure.sh KERNEL CONFIGDB LOG" >&2
    exit 2
fi

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
kernel=$(realpath "$1")
configdb=$(realpath "$2")
log_file=$3
timeout_seconds=${PEDIGREE_HOSTED_IRQ_TIMEOUT_SECONDS:-30}

if [[ $(uname -s) != Linux || $(uname -m) != x86_64 ]]; then
    echo "The hosted IRQ closure kernel requires x86-64 Linux." >&2
    exit 2
fi
if [[ ! "$timeout_seconds" =~ ^[1-9][0-9]*$ ]]; then
    echo "PEDIGREE_HOSTED_IRQ_TIMEOUT_SECONDS must be a positive integer." >&2
    exit 2
fi
for artifact in "$kernel" "$configdb"; do
    if [[ ! -f "$artifact" ]]; then
        echo "Required hosted artifact is unavailable: $artifact" >&2
        exit 1
    fi
done

mkdir -p "$(dirname -- "$log_file")"
log_file=$(realpath "$log_file")
scratch_dir=$(mktemp -d)
trap 'rm -rf "$scratch_dir"' EXIT
mkdir "$scratch_dir/empty-initrd"
(
    cd "$scratch_dir/empty-initrd"
    cmake -E tar cf "$scratch_dir/empty-initrd.tar" --format=gnutar -- .
)

run_status=0
(
    cd "$scratch_dir"
    python3 "$script_dir/run-with-deadline.py" \
        --seconds "$timeout_seconds" --label "hosted IRQ closure" -- \
        "$kernel" "$scratch_dir/empty-initrd.tar" "$configdb"
) >"$log_file" 2>&1 || run_status=$?
if (( run_status != 0 )); then
    cat "$log_file"
    echo "Hosted IRQ closure failed with status $run_status." >&2
    exit "$run_status"
fi

required_markers=(
    "HOSTED-IRQ-CLOSURE: PASS hard-irq-operation-guards"
    "HOSTED-IRQ-CLOSURE: PASS threaded-dispatcher-lifecycle"
    "HOSTED-IRQ-CLOSURE: PASS split-handler-lifecycle"
    "HOSTED-IRQ-CLOSURE: PASS hosted-timer-split-delivery"
    "HOSTED-WAIT-TEST: PASS interrupt-manager-lock-independent"
    "HOSTED-WAIT-TEST: PASS hosted-signal-autodisarm-preemption"
    "HOSTED-WAIT-TEST: PASS hosted-scheduler-timer-single-owner"
    "HOSTED-WAIT-TEST: PASS hosted-scheduler-timer-abandoned-admission-cleanup"
    "HOSTED-WAIT-TEST: PASS hosted-scheduler-timer-self-removal-rejected"
    "HOSTED-WAIT-TEST: PASS hosted-scheduler-route-dedicated"
    "HOSTED-WAIT-TEST: PASS hosted-scheduler-timer-hard-context"
    "HOSTED-WAIT-TEST: PASS scheduler-timer-exit-return-tail"
    "HOSTED-WAIT-TEST: PASS deferred-time-accounting-worker"
    "HOSTED-WAIT-TEST: PASS context-switch-interrupt-restore"
    "HOSTED-IRQ-CLOSURE: PASS all"
    "HOSTED-SHUTDOWN: timers and signals quiesced"
    "main() returned, cleaning up"
)
for marker in "${required_markers[@]}"; do
    if ! grep -aFq "$marker" "$log_file"; then
        cat "$log_file"
        echo "Hosted IRQ closure log is missing marker: $marker" >&2
        exit 1
    fi
done

rejected_markers=(
    "HOSTED-IRQ-CLOSURE: FAIL"
    "HOSTED-WAIT-TEST: FAIL"
    "Page Fault Exception"
    "Double Fault Exception"
    "loading module #"
)
for marker in "${rejected_markers[@]}"; do
    if grep -aFq "$marker" "$log_file"; then
        cat "$log_file"
        echo "Hosted IRQ closure log contains failure marker: $marker" >&2
        exit 1
    fi
done

echo "Linux-hosted IRQ closure passed."
echo "Log: $log_file"
