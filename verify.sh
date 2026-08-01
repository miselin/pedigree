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

hosted_timeout=${PEDIGREE_VERIFY_HOSTED_TIMEOUT_SECONDS:-5400}
if [[ ! "$hosted_timeout" =~ ^[1-9][0-9]*$ ]]; then
    echo "PEDIGREE_VERIFY_HOSTED_TIMEOUT_SECONDS must be a positive integer." >&2
    exit 2
fi

for required_command in awk cmake ctest date git grep mkdir python3 rg rmdir sed tee uname; do
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

check_wait_api_boundaries()
{
    local matches
    local failed=0

    if ! python3 scripts/run-with-deadline.py --self-test; then
        echo "The process-group deadline helper failed its self-test."
        failed=1
    fi

    matches=$(rg -n -U \
        'schedule\(\s*Thread::Sleeping' src \
        --glob '*.{cc,h}' |
        rg -v '^src/system/kernel/core/process/PerProcessorScheduler\.cc:' ||
        true)
    if [[ -n "$matches" ]]; then
        echo "Sleeping was scheduled outside WaitQueue:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n 'blockCurrent\(' src --glob '*.{cc,h}' |
        rg -v \
            '^(src/system/kernel/core/process/(PerProcessorScheduler|WaitQueue)\.cc|src/system/include/pedigree/kernel/process/PerProcessorScheduler\.h):' ||
        true)
    if [[ -n "$matches" ]]; then
        echo "The scheduler blocking primitive escaped its intended boundary:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n '(->|\.)setStatus\((Thread::)?(Ready|Sleeping)' \
        src --glob '*.{cc,h}' |
        rg -v \
            '^src/system/kernel/core/process/(PerProcessorScheduler|Process|Thread)\.cc:' ||
        true)
    if [[ -n "$matches" ]]; then
        echo "A caller bypassed the wait/wake API with a raw status change:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n 'm_Status\s*=\s*(Thread::)?Sleeping' \
        src --glob '*.{cc,h}' || true)
    if [[ -n "$matches" ]]; then
        echo "A thread was put to sleep without publishing a WaitQueue waiter:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n \
        'Scheduler::instance\(\)\.threadStatusChanged' \
        src/system/kernel/core/process/WaitQueue.cc || true)
    if [[ -n "$matches" ]]; then
        echo "A WaitQueue wake re-entered the global scheduler registry:"
        echo "$matches"
        failed=1
    fi

    local generic_thread_publications
    generic_thread_publications=$(rg -c \
        'Scheduler::instance\(\)\.threadStatusChanged\(this\)' \
        src/system/kernel/core/process/Thread.cc || true)
    if [[ "$generic_thread_publications" != "2" ]]; then
        echo "Thread has an unexpected number of global scheduler publications:"
        rg -n 'Scheduler::instance\(\)\.threadStatusChanged' \
            src/system/kernel/core/process/Thread.cc || true
        failed=1
    fi

    matches=$(rg -n \
        '(\bMutex\s*\(\s*(true|false)\s*\)|\bMutex\s+[A-Za-z_][A-Za-z0-9_]*\s*\(\s*(true|false)\s*\))' \
        src --glob '*.{cc,h}' || true)
    if [[ -n "$matches" ]]; then
        echo "A boolean Mutex constructor reintroduced ambiguous completion semantics:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n \
        '(acquireWithResult|ConditionVariable::WaitResult)' \
        src/modules src/system/include/pedigree/kernel/utilities \
        --glob '*.{cc,h}' || true)
    if [[ -n "$matches" ]]; then
        echo "A module or module-instantiated template crossed the kernel ABI with a compiler-dependent Result aggregate:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n -U \
        '(ProcessLease|ThreadLease)[[:space:]]+(Scheduler::|Process::)?acquire(Process(ById)?|Thread)[[:space:]]*\(' \
        src/system/include/pedigree/kernel/process \
        src/system/kernel/core/process --glob '*.{cc,h}' || true)
    if [[ -n "$matches" ]]; then
        echo "An exported lifetime-lease acquisition returned a nontrivial RAII object by value:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n -U \
        'OperationBarrier::Lease[[:space:]\r\n]+[A-Za-z_][A-Za-z0-9_]*[[:space:]\r\n]*\(' \
        src --glob '*.{cc,h}' || true)
    if [[ -n "$matches" ]]; then
        echo "A non-trivial OperationBarrier lease was returned across a compiler boundary:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n \
        'OperationBarrier[^;\r\n]*tryAcquire\(\)|[A-Za-z_][A-Za-z0-9_]*Operations\.tryAcquire\(\)' \
        src --glob '*.{cc,h}' || true)
    if [[ -n "$matches" ]]; then
        echo "OperationBarrier admission bypassed its scalar/out ABI:"
        echo "$matches"
        failed=1
    fi

    if ! python3 scripts/check-explicit-template-result-abi.py \
        --self-test; then
        echo "The exported Result ABI detector failed its self-test."
        failed=1
    fi

    if ! python3 scripts/list-hosted-wait-markers.py --self-test; then
        echo "The hosted regression marker detector failed its self-test."
        failed=1
    fi

    if ! python3 scripts/check-syscall-registration.py --self-test; then
        echo "The syscall registration ownership detector failed its self-test."
        failed=1
    fi

    if ! python3 scripts/check-syscall-registration.py src; then
        echo "A syscall handler escaped token-owned registration."
        failed=1
    fi

    matches=$(
        {
            rg -n \
                '(killCurrentThread|eventHandlerReturned|Processor::contextSwitch|Processor::jumpUser|system_(reset|reboot)[[:space:]]*\()' \
                src/modules/subsys/posix/PosixSyscallManager.cc \
                src/modules/subsys/pedigree-c/PedigreeCSyscallManager.cc \
                src/modules/subsys/posix/linux-amd64-signal.cc \
                src/modules/subsys/pedigree-c/pedigree-syscalls.cc \
                src/modules/subsys/posix/signal-syscalls.cc || true
            rg -n '\bposix_exit[[:space:]]*\(' \
                src/modules/subsys/posix/PosixSyscallManager.cc || true
            rg -n 'Processor::jumpUser' \
                src/modules/subsys/posix/PosixSubsystem.cc || true
            rg -n 'system_(reset|reboot)[[:space:]]*\(' \
                src/modules/subsys/posix/system-syscalls.cc || true
        }
    )
    if [[ -n "$matches" ]]; then
        echo "A syscall callback directly performed a non-returning transition:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q -U \
        'LinuxAmd64Signal::sigreturn\(\s*state\s*\);[[:space:]]*return 0;' \
        src/modules/subsys/posix/PosixSyscallManager.cc; then
        echo "Linux rt_sigreturn can fall through and stage a second post-syscall action."
        failed=1
    fi

    if ! rg -q -U \
        '\(kind == ReturnFromEvent \|\| kind == PopEventState\)[[:space:]]*&&[[:space:]]*!target->stateLevel' \
        src/system/kernel/core/processor/SyscallManager.cc; then
        echo "Base-state event return/pop requests were not rejected at admission."
        failed=1
    fi

    matches=$(rg -n \
        'FATAL\("(Signal return|Signal unwind|Event return) was not dispatched\."\)' \
        src/modules/subsys/posix/signal-syscalls.cc \
        src/modules/subsys/pedigree-c/pedigree-syscalls.cc || true)
    if [[ -n "$matches" ]]; then
        echo "Expected base-state event-action rejection still panics:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q -U \
        'case PEDIGREE_EVENT_RETURN:[[:space:]]*return pedigree_event_return\(\);' \
        src/modules/subsys/pedigree-c/PedigreeCSyscallManager.cc; then
        echo "The staged Pedigree-C event return can fall through its syscall case."
        failed=1
    fi

    if ! rg -q -U \
        'int pedigree_event_return\(\)[[:space:]]*\{[[:space:]]*return \(int\) syscall0\(PEDIGREE_EVENT_RETURN\);' \
        src/modules/subsys/pedigree-c/pedigree-c-syscalls.c; then
        echo "The exported Pedigree-C event-return wrapper discarded its syscall result."
        failed=1
    fi

    if ! rg -q -U \
        'syscallState\.setSyscallErrno\(thread->getErrno\(\)\);[[:space:]]*thread->setErrno\(0\);' \
        src/system/kernel/core/processor/hosted/SyscallManager.cc; then
        echo "The hosted syscall boundary leaked a completed syscall errno."
        failed=1
    fi

    if ! rg -q -U \
        'moduleName\.compare\("pedigree-c"\)[^}]*SYSCALL_ERROR\(DeviceBusy\);[^}]*return -1;[^}]*\}[[:space:]]*pedigree_module_unload\([[:space:]]*const_cast<char \*>\(moduleName\.cstr\(\)\)\);' \
        src/modules/subsys/pedigree-c/PedigreeCSyscallManager.cc; then
        echo "The Pedigree-C syscall handler can request its own module unload."
        failed=1
    fi

    matches=$(rg -n -U \
        'case RestoreProcessorState:[[:space:]]*\{[^}]*abandonCurrentState' \
        src/system/kernel/core/processor/x64/SyscallManager.cc || true)
    if [[ -n "$matches" ]]; then
        echo "Linux rt_sigreturn discarded a Pedigree event state it does not own:"
        echo "$matches"
        failed=1
    fi

    if ! matches=$(
        python3 scripts/check-explicit-template-result-abi.py \
            src/system/include/pedigree/kernel/utilities
    ); then
        echo "An explicitly instantiated exported template exposes a compiler-dependent Result aggregate:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n \
        'volatile[[:space:]]+bool[[:space:]]+[A-Za-z_][A-Za-z0-9_]*(Lock|Mutex|Guard)' \
        src --glob '*.{cc,h}' || true)
    if [[ -n "$matches" ]]; then
        echo "A volatile flag was used as a hand-rolled lock:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n -U \
        'while[[:space:]]*\([^)]*(Lock|Mutex|Guard)[^)]*\)[[:space:]]*;' \
        src --glob '*.{cc,h}' || true)
    if [[ -n "$matches" ]]; then
        echo "A lock-shaped predicate was busy-spun without an ownership API:"
        echo "$matches"
        failed=1
    fi

    local cdi_irq_source=src/modules/drivers/common/cdi/CdiIrq.cc
    matches=$(rg -n \
        'if[[:space:]]*\([[:space:]]*irq[[:space:]]*>[[:space:]]*IRQ_COUNT' \
        "$cdi_irq_source" || true)
    if [[ -n "$matches" ]]; then
        echo "A CDI IRQ bounds check admitted IRQ_COUNT:"
        echo "$matches"
        failed=1
    fi

    local cdi_drain_pattern='while[[:space:]]*\([^)]*tryAcquire\([^)]*\)[[:space:]]*\)[[:space:]]*\{?[[:space:]]*[^;]*release[[:space:]]*\('
    if ! printf '%s\n' \
        'while (counter.tryAcquire())' '{' 'counter.release();' '}' |
        rg -q -U "$cdi_drain_pattern"; then
        echo "The CDI semaphore-drain detector failed its self-test."
        failed=1
    fi

    matches=$(rg -n -U "$cdi_drain_pattern" "$cdi_irq_source" || true)
    if [[ -n "$matches" ]]; then
        echo "A CDI semaphore drain reacquired the item it just removed:"
        echo "$matches"
        failed=1
    fi

    matches=$(rg -n \
        '(semaphore|counter)(->|\.)acquire[[:space:]]*\(|timeout[[:space:]]*\*[[:space:]]*1000' \
        "$cdi_irq_source" || true)
    if [[ -n "$matches" ]]; then
        echo "CDI bypassed the checked millisecond IRQ-wait contract:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q 'counter\.drainAvailable\(\)' "$cdi_irq_source" ||
        ! rg -q 'counter\.acquireWithError\(' "$cdi_irq_source" ||
        ! rg -q 'timeout == 0' "$cdi_irq_source"; then
        echo "CDI IRQ waits escaped the checked drain/wait APIs."
        failed=1
    fi

    local irq_registry_source=src/system/kernel/machine/IrqHandlerRegistry.cc
    matches=$(rg -n \
        'Scheduler::instance\(\)\.yield\(\)' \
        "$irq_registry_source" || true)
    if [[ -n "$matches" ]]; then
        echo "The IRQ callback drain reverted to scheduler yielding:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q -U \
        'm_DispatchWaiters\.acquire\(\)[^;]*;[[:space:]]*if[[:space:]]*\([^)]*!hasActiveDispatch[^)]*\)[^{]*\{[[:space:]]*break;[^}]*\}[[:space:]]*const WaitQueue::WakeReason[^=]*=[[:space:]]*guard\.waitForCompletion\(' \
        "$irq_registry_source"; then
        echo "The IRQ callback drain escaped its predicate-coupled wait."
        failed=1
    fi

    if ! rg -q 'mode == SlotMode::Draining' "$irq_registry_source" ||
        ! rg -q 'guard\.wakeAll\(' "$irq_registry_source" ||
        ! rg -q 'WaitQueue::Channel\(&slot\)' "$irq_registry_source"; then
        echo "The IRQ callback drain escaped its predicate-coupled wake."
        failed=1
    fi

    matches=$(rg -n -U \
        'while[[:space:]]*\([^;{}]*acquireLock\([^;{}]*\)[[:space:]]*\)[[:space:]]*;' \
        src --glob '*.{cc,h}' || true)
    if [[ -n "$matches" ]]; then
        echo "A blocking-capable operation gate was busy-spun:"
        echo "$matches"
        failed=1
    fi

    return "$failed"
}

cmake_options=(-DPEDIGREE_WARNINGS=ON)

run_stage submodules \
    git submodule update --init --recursive
run_stage wait-api-boundaries \
    check_wait_api_boundaries
run_stage native-configure \
    cmake -S "$script_dir" -B "$native_build_dir" \
    "${cmake_options[@]}" -DPEDIGREE_BUILDUTILS_ASAN=OFF
run_stage native-build \
    cmake --build "$native_build_dir" "${parallel_args[@]}" \
    --target testsuite memorytracer
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
    python3 "$script_dir/scripts/run-with-deadline.py" \
    --seconds "$hosted_timeout" \
    --label "complete hosted and x86-64 verification stage" -- \
    env PEDIGREE_HOSTED_CONTAINER=0 PEDIGREE_HOSTED_NATIVE=0 \
        PEDIGREE_VERIFY_LOG_DIR="$run_log_dir/hosted" \
        "$script_dir/easy_build_hosted.sh"
