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

# The analyzer requires the modern cross-compiler and is intentionally opt-in.
verify_sarif=${PEDIGREE_VERIFY_SARIF:-0}
if [[ "$verify_sarif" != 0 && "$verify_sarif" != 1 ]]; then
    echo "PEDIGREE_VERIFY_SARIF must be either 0 or 1." >&2
    exit 2
fi
if [[ "$verify_sarif" == 1 ]] && ! command -v python3 >/dev/null 2>&1; then
    echo "Required SARIF analysis command is unavailable: python3" >&2
    exit 1
fi

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
    echo "Pedigree verification"
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

run_sarif_analysis()
{
    local compile_commands=${PEDIGREE_SARIF_COMPILE_COMMANDS:-}
    local sarif_build_dir="$build_root/sarif-x64"
    local sarif_output_dir="$run_log_dir/sarif"
    local toolchain_root=${PEDIGREE_TOOLCHAIN_ROOT:-"$script_dir/compilers/dir"}
    local musl_archive=${PEDIGREE_SARIF_MUSL_ARCHIVE:-}
    local jobs=()
    local parallel_args=(--parallel)

    if [[ -n "${PEDIGREE_VERIFY_JOBS:-}" ]]; then
        jobs=(--jobs "$PEDIGREE_VERIFY_JOBS")
        parallel_args=(--parallel "$PEDIGREE_VERIFY_JOBS")
    fi

    if [[ -n "$compile_commands" ]]; then
        if [[ "$compile_commands" != /* ]]; then
            compile_commands="$script_dir/$compile_commands"
        fi
    else
        compile_commands="$sarif_build_dir/compile_commands.json"
        if [[ -z "$musl_archive" ]]; then
            if [[ -f "$build_root/native/darwin-hosted/src/modules/musl-1.2.6.tar.gz" ]]; then
                musl_archive="$build_root/native/darwin-hosted/src/modules/musl-1.2.6.tar.gz"
            elif [[ -f "$build_root/native/regular/src/modules/musl-1.2.6.tar.gz" ]]; then
                musl_archive="$build_root/native/regular/src/modules/musl-1.2.6.tar.gz"
            fi
        fi
        if [[ -n "${PEDIGREE_SARIF_MUSL_ARCHIVE:-}" && "$musl_archive" != /* ]]; then
            musl_archive="$script_dir/$musl_archive"
        fi
        if [[ -n "${PEDIGREE_SARIF_MUSL_ARCHIVE:-}" && ! -f "$musl_archive" ]]; then
            echo "PEDIGREE_SARIF_MUSL_ARCHIVE is unavailable: $musl_archive" >&2
            return 1
        fi
        if [[ -f "$musl_archive" ]]; then
            cmake -E make_directory "$sarif_build_dir/src/modules"
            cmake -E copy_if_different "$musl_archive" \
                "$sarif_build_dir/src/modules/musl-1.2.6.tar.gz"
        fi
        cmake -S "$script_dir" -B "$sarif_build_dir" \
            -DCMAKE_TOOLCHAIN_FILE="$script_dir/build-etc/cmake/pedigree_amd64.cmake" \
            -DIMPORT_EXECUTABLES="$build_root/native/regular/HostUtilities.cmake" \
            -DPEDIGREE_TOOLCHAIN_ROOT="$toolchain_root" \
            -DPEDIGREE_BUILD_USER_DIR=ON \
            -DPEDIGREE_WARNINGS=ON \
            -DPEDIGREE_WITH_INIT=ON
        cmake --build "$sarif_build_dir" "${parallel_args[@]}" \
            --target vdso-header
    fi

    python3 "$script_dir/scripts/run_sarif_analysis.py" \
        --compile-commands "$compile_commands" \
        --output-dir "$sarif_output_dir" \
        --source-root "$script_dir" \
        "${jobs[@]}"
}

run_stage host-build-and-test \
    env PEDIGREE_NATIVE_BUILD_ROOT="$build_root/native" \
        "$script_dir/easy_build_hosted.sh"

if [[ "$verify_sarif" == 1 ]]; then
    run_stage gcc15-sarif-analysis run_sarif_analysis
fi
