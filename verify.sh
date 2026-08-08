#!/usr/bin/env bash

set -Eeuo pipefail

if (( $# )); then
    echo "usage: ./verify.sh" >&2
    exit 2
fi

script_dir=$(cd -P -- "$(dirname -- "$0")" && pwd -P)
cd "$script_dir"

for command in cmake ctest date git tee; do
    if ! command -v "$command" >/dev/null 2>&1; then
        echo "Required host command is unavailable: $command" >&2
        exit 1
    fi
done

build_root=${PEDIGREE_VERIFY_BUILD_ROOT:-"$script_dir/build-verify"}
log_root=${PEDIGREE_VERIFY_LOG_ROOT:-"$build_root/logs"}
run_id=${PEDIGREE_VERIFY_RUN_ID:-$(date -u "+%Y%m%dT%H%M%SZ")}
run_log_dir="$log_root/$run_id"
summary_file="$run_log_dir/summary.txt"
metadata_file="$run_log_dir/metadata.txt"

if ! mkdir -p "$run_log_dir"; then
    echo "Could not create verification log directory: $run_log_dir" >&2
    exit 1
fi
if [[ -e "$summary_file" ]]; then
    echo "Verification run already exists: $run_log_dir" >&2
    exit 2
fi

started_at=$(date -u "+%Y-%m-%dT%H:%M:%SZ")
started_seconds=$SECONDS
current_stage=setup

{
    echo "Pedigree native verification"
    echo "run: $run_id"
    echo "started: $started_at"
    echo "repository: $script_dir"
    echo
    printf "%-8s  %-24s  %10s  %s\n" "RESULT" "STAGE" "SECONDS" "LOG"
} >"$summary_file"

{
    echo "commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "uname=$(uname -a)"
    echo "cmake=$(cmake --version | sed -n '1p')"
    echo "ctest=$(ctest --version | sed -n '1p')"
    echo
    echo "working tree:"
    git status --short 2>/dev/null || true
} >"$metadata_file"

finish()
{
    local status=$?
    local result=PASS
    trap - EXIT
    if (( status != 0 )); then
        result=FAIL
    fi

    {
        echo
        echo "overall: $result"
        echo "finished: $(date -u "+%Y-%m-%dT%H:%M:%SZ")"
        echo "elapsed seconds: $((SECONDS - started_seconds))"
        if (( status != 0 )); then
            echo "failed stage: $current_stage"
        fi
    } >>"$summary_file"

    echo
    echo "Verification $result. Summary: $summary_file"
    exit "$status"
}
trap finish EXIT

run_stage()
{
    local stage=$1
    shift
    local log_file="$run_log_dir/$stage.log"
    local stage_started=$SECONDS
    local status=0
    local result=PASS

    current_stage=$stage
    echo
    echo "==> $stage"
    "$@" 2>&1 | tee "$log_file" || status=$?
    if (( status != 0 )); then
        result=FAIL
    fi
    printf "%-8s  %-24s  %10d  %s\n" \
        "$result" "$stage" "$((SECONDS - stage_started))" "$log_file" \
        >>"$summary_file"
    return "$status"
}

run_stage native-build-and-test \
    env PEDIGREE_NATIVE_BUILD_ROOT="$build_root/native" \
        "$script_dir/easy_build_hosted.sh"
