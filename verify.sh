#!/usr/bin/env bash

set -Eeuo pipefail

if (( $# )); then
    echo "usage: ./verify.sh" >&2
    exit 2
fi

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

for required_command in awk cmake ctest date git grep mkdir rmdir sed tee uname; do
    if ! command -v "$required_command" >/dev/null 2>&1; then
        echo "Required host command is unavailable: $required_command" >&2
        exit 1
    fi
done

build_root=${PEDIGREE_VERIFY_BUILD_ROOT:-"$script_dir/build-verify"}
log_root=${PEDIGREE_VERIFY_LOG_ROOT:-"$build_root/logs"}
run_id=${PEDIGREE_VERIFY_RUN_ID:-$(date -u "+%Y%m%dT%H%M%SZ")}
run_log_dir="$log_root/$run_id"
native_build_dir="$build_root/native"
asan_build_dir="$build_root/asan"
verify_lock_root="$script_dir/build-verify"
verify_lock_dir="$verify_lock_root/.verify.lock"

mkdir -p "$log_root" "$verify_lock_root"
if ! mkdir "$verify_lock_dir"; then
    echo "Another verification is using this checkout: $verify_lock_dir" >&2
    exit 2
fi

if ! mkdir "$run_log_dir"; then
    rmdir "$verify_lock_dir"
    echo "Verification log directory already exists: $run_log_dir" >&2
    exit 2
fi

summary_file="$run_log_dir/summary.txt"
metadata_file="$run_log_dir/metadata.txt"
started_at=$(date -u "+%Y-%m-%dT%H:%M:%SZ")
started_seconds=$SECONDS
current_stage=setup
if ! : >"$summary_file"; then
    rmdir "$run_log_dir" "$verify_lock_dir" 2>/dev/null || true
    echo "Could not create verification summary: $summary_file" >&2
    exit 1
fi

finish()
{
    local exit_status=$?
    local finished_at elapsed result

    trap - EXIT
    finished_at=$(date -u "+%Y-%m-%dT%H:%M:%SZ")
    elapsed=$((SECONDS - started_seconds))
    if (( exit_status == 0 )); then
        result=PASS
    else
        result=FAIL
    fi

    {
        echo
        echo "overall: $result"
        echo "finished: $finished_at"
        echo "elapsed seconds: $elapsed"
        if (( exit_status != 0 )); then
            echo "failed stage: $current_stage"
        fi
    } >>"$summary_file"

    rmdir "$verify_lock_dir" 2>/dev/null || true
    echo
    echo "Verification $result. Summary: $summary_file"
    exit "$exit_status"
}
trap finish EXIT

mkdir "$run_log_dir/hosted"

{
    echo "Pedigree verification"
    echo "run: $run_id"
    echo "started: $started_at"
    echo "repository: $script_dir"
    echo "build root: $build_root"
    echo "log directory: $run_log_dir"
    echo
    printf "%-8s  %-24s  %10s  %s\n" "RESULT" "STAGE" "SECONDS" "LOG"
} >"$summary_file"

{
    echo "run=$run_id"
    echo "started=$started_at"
    echo "repository=$script_dir"
    echo "commit=$(git rev-parse HEAD 2>/dev/null || echo unknown)"
    echo "uname=$(uname -a)"
    echo "cmake=$(cmake --version | sed -n '1p')"
    echo "ctest=$(ctest --version | sed -n '1p')"
    echo
    echo "working tree:"
    git status --short 2>/dev/null || true
} >"$metadata_file"

run_stage()
{
    local stage=$1
    shift

    local log_file="$run_log_dir/$stage.log"
    local stage_started=$SECONDS
    local stage_status result elapsed
    current_stage=$stage

    echo
    echo "==> $stage"
    if "$@" 2>&1 | tee "$log_file"; then
        stage_status=0
        result=PASS
    else
        stage_status=$?
        result=FAIL
    fi

    elapsed=$((SECONDS - stage_started))
    printf "%-8s  %-24s  %10d  %s\n" \
        "$result" "$stage" "$elapsed" "$log_file" >>"$summary_file"
    return "$stage_status"
}

cmake_options=(-DPEDIGREE_WARNINGS=ON)

run_stage submodules \
    git submodule update --init --recursive
run_stage native-configure \
    cmake -S "$script_dir" -B "$native_build_dir" \
    "${cmake_options[@]}" -DPEDIGREE_BUILDUTILS_ASAN=OFF
run_stage native-build \
    cmake --build "$native_build_dir" "${parallel_args[@]}" --target testsuite
run_stage native-tests \
    ctest --test-dir "$native_build_dir" --output-on-failure --no-tests=error

run_stage asan-configure \
    cmake -S "$script_dir" -B "$asan_build_dir" \
    "${cmake_options[@]}" -DPEDIGREE_BUILDUTILS_ASAN=ON
run_stage asan-available \
    awk '
        $0 == "HAVE_ASAN:INTERNAL=1" { found = 1 }
        END {
            if (!found) {
                print "The native compiler does not support AddressSanitizer."
                exit 1
            }
            print "AddressSanitizer is available."
        }
    ' "$asan_build_dir/CMakeCache.txt"
run_stage asan-build \
    cmake --build "$asan_build_dir" "${parallel_args[@]}" --target testsuite
run_stage asan-tests \
    env ASAN_OPTIONS="${ASAN_OPTIONS:+$ASAN_OPTIONS:}halt_on_error=1:abort_on_error=1:exitcode=99" \
    ctest --test-dir "$asan_build_dir" --output-on-failure --no-tests=error

reject_sanitizer_report()
{
    local log=$1
    if grep -aEn \
        "ERROR: AddressSanitizer|AddressSanitizer:DEADLYSIGNAL|SUMMARY: AddressSanitizer" \
        "$log"
    then
        echo "AddressSanitizer reported an error." >&2
        return 1
    fi
}

run_stage asan-log-check \
    reject_sanitizer_report "$run_log_dir/asan-tests.log"

run_stage hosted-build-and-smoke \
    env PEDIGREE_HOSTED_CONTAINER=0 PEDIGREE_HOSTED_NATIVE=0 \
    PEDIGREE_VERIFY_LOG_DIR="$run_log_dir/hosted" \
    "$script_dir/easy_build_hosted.sh"
