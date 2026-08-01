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

    local usb_port_change_header=src/modules/drivers/common/usb-hcd/PortChangeRequest.h
    matches=$(rg -n \
        'Scheduler::instance\(\)\.yield\(\)' \
        "$usb_port_change_header" || true)
    if [[ -n "$matches" ]]; then
        echo "A USB port-change worker reverted to acknowledgement polling:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q 'WaitQueue m_AcknowledgementWaiters' \
        "$usb_port_change_header" ||
        ! rg -q 'guard\.waitForCompletion\(' \
            "$usb_port_change_header" ||
        ! rg -q 'WaitQueue::Channel\(this\), Thread::CallbackDrain' \
            "$usb_port_change_header"; then
        echo "The USB port-change worker escaped its acknowledgement queue."
        failed=1
    fi

    if ! rg -q -U \
        'void acknowledge\(size_t generation\)[^{]*\{[^}]*m_AcknowledgementWaiters\.acquire\(\)[^}]*advance\(m_Acknowledged, generation\)[^}]*guard\.wakeAll\([^}]*WaitQueue::Channel\(this\)' \
        "$usb_port_change_header" ||
        ! rg -q -U \
            'void stopAfterQuiesce\(\)[^{]*\{[^}]*m_AcknowledgementWaiters\.acquire\(\)[^}]*m_Stopping = 1[^}]*guard\.wakeAll\([^}]*WaitQueue::Channel\(this\)' \
            "$usb_port_change_header"; then
        echo "A USB ACK or stop escaped its predicate-coupled wake."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)void released\(\).*m_AcknowledgementWaiters\.acquire\(\).*m_Stopping.*publishGeneration\(observed, false\)' \
        "$usb_port_change_header"; then
        echo "USB port stop no longer serialises follow-up republication."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)waitUntilAcknowledged\(size_t generation\).*m_AcknowledgementWaiters\.acquire\(\).*m_Stopping.*m_Acknowledged >= generation.*guard\.waitForCompletion\(' \
        "$usb_port_change_header" ||
        ! rg -q \
            'reinterpret_cast<uintptr_t>\(this\)' \
            "$usb_port_change_header"; then
        echo "The USB acknowledgement predicate escaped its wait guard."
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

    local pic_header=src/system/kernel/machine/mach_pc/Pic.h
    local pic_source=src/system/kernel/machine/mach_pc/Pic.cc
    local pic_state=src/system/kernel/machine/mach_pc/PicIrqState.h
    matches=$(rg -n -U \
        'uint8_t[[:space:]]+m_InterruptMask|m_HandlerEdge|m_(Master|Slave)Port\.read8\(1\)|setEnabledLocked\(irq, true\);[[:space:]]*eoiLocked\(irq\)' \
        "$pic_header" "$pic_source" || true)
    if [[ -n "$matches" ]]; then
        echo "The dual PIC reverted to partial or hardware-derived line state:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q 'uint16_t m_Mask' "$pic_state" ||
        ! rg -q '0xFFFB' "$pic_state" ||
        ! rg -q 'm_DispatchGenerations' "$pic_state" ||
        ! rg -q 'm_AcknowledgedGenerations' "$pic_state" ||
        ! rg -q 'PicIrqState m_IrqState' "$pic_header" ||
        ! rg -q 'm_MasterPort\.write8\(m_IrqState\.masterMask\(\), 1\)' \
            "$pic_source" ||
        ! rg -q 'm_SlavePort\.write8\(m_IrqState\.slaveMask\(\), 1\)' \
            "$pic_source"; then
        echo "The dual PIC lost its authoritative sixteen-line mask."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)registerHardIsaIrqHandler.*?LockGuard<Spinlock> guard\(m_Lock\);.*?canRegister\(.*?registerHardHandler\(.*?handlerRegistered\(.*?applyMaskLocked\(' \
        "$pic_source" ||
        ! rg -q -U \
            '(?s)unregisterHandler.*?m_Handlers\.unregisterHandler\(.*?LockGuard<Spinlock> guard\(m_Lock\);.*?handlerUnregistered\(' \
            "$pic_source"; then
        echo "PIC registration accounting escaped its line-state lock."
        failed=1
    fi

    if ! rg -q -U \
        'Get ISR for master\.(?s:.*?)m_MasterPort\.write8\(0x0A, 0\)' \
        "$pic_source" ||
        ! rg -q \
            'irq == 7 \|\| irq == 15' "$pic_source" ||
        ! rg -q -U \
            'spuriousLocked\(irq\)(?s:.*?)irq > 7(?s:.*?)m_MasterPort\.write8\(0x62, 0\)' \
            "$pic_source"; then
        echo "PIC spurious-vector handling lost its master acknowledgement."
        failed=1
    fi

    if ! rg -q \
        'edgeTriggered = m_IrqState\.edgeTriggered\(irq\)' "$pic_source" ||
        ! rg -q 'if \(edgeTriggered\)' "$pic_source" ||
        ! rg -q 'if \(!edgeTriggered\)' "$pic_source"; then
        echo "PIC dispatch no longer uses one trigger-mode snapshot."
        failed=1
    fi

    if ! rg -q 'm_IrqState\.beginDispatch\(irq\)' "$pic_source" ||
        ! rg -q 'm_IrqState\.completeDispatch\(' "$pic_source" ||
        ! rg -q 'const bool firstHandler = m_HandlerCounts\[irq\] == 0' \
            "$pic_state"; then
        echo "PIC acknowledgement ordering lost its dispatch generation."
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

    if ! rg -q \
        'modeOf\(finalPublication\) == SlotMode::Draining' \
        "$irq_registry_source" ||
        ! rg -q 'guard\.wakeAll\(' "$irq_registry_source" ||
        ! rg -q 'WaitQueue::Channel\(&slot\)' "$irq_registry_source"; then
        echo "The IRQ callback drain escaped its predicate-coupled wake."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)findCurrentDispatch\(owner, nullptr, 0, callbackContext\);.*?if \(!canYield \|\| callbackContext\).*?m_HandlerLock\.acquire\(\)' \
        "$irq_registry_source" ||
        ! rg -q \
            'canYield = canYield && !current->getHostedSignalDepth\(\)' \
            "$irq_registry_source"; then
        echo "IRQ callback removal can reach the blocking writer-lock path."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)unpublishDispatch\([^)]*bool required\).*?if \(required\).*?released more than once' \
        "$irq_registry_source" ||
        ! rg -q -U \
            '(?s)abandonDispatch\(void \*context\).*?unpublishDispatch\([^;]*false\);' \
            "$irq_registry_source"; then
        echo "IRQ abandonment lost partial-publication cleanup semantics."
        failed=1
    fi

    if ! rg -q 'size_t admittedPublication' \
        src/system/include/pedigree/kernel/machine/IrqHandlerRegistry.h ||
        ! rg -q -U \
            'generationOf\(dispatchPublication\)[[:space:]]*==[[:space:]]*generationOf\(admittedPublication\)' \
            "$irq_registry_source" ||
        ! rg -q \
            'hasActiveDispatch\(slot, admittedPublication\)' \
            "$irq_registry_source" ||
        ! rg -q 'HandlerHazardStage::Committed' "$irq_registry_source"; then
        echo "IRQ hazards lost generation-aware slot reuse protection."
        failed=1
    fi

    local irq_handler_header=src/system/include/pedigree/kernel/machine/IrqHandler.h
    local irq_registry_header=src/system/include/pedigree/kernel/machine/IrqHandlerRegistry.h
    local split_irq_header=src/system/include/pedigree/kernel/machine/SplitIrqHandler.h
    local split_irq_source=src/system/kernel/machine/SplitIrqHandler.cc
    if ! rg -q -U \
        'class EXPORTED_PUBLIC IrqHandler[[:space:]]*:[[:space:]]*public IrqHandlerBase' \
        "$irq_handler_header" ||
        ! rg -q \
            'virtual IrqDisposition irq\(irq_id_t number\)' \
            "$irq_handler_header" ||
        ! rg -q -U \
            'class EXPORTED_PUBLIC HardIrqHandler[[:space:]]*:[[:space:]]*public IrqHandlerBase' \
            "$irq_handler_header" ||
        ! rg -q -U \
            'virtual bool irq\(irq_id_t number, InterruptState &state\)' \
            "$irq_handler_header"; then
        echo "The IRQ API lost its explicit thread and hard-context types."
        failed=1
    fi

    if ! rg -q 'enum class Delivery' "$irq_registry_header" ||
        ! rg -q 'registerThreadedHandler\(uint8_t irq, IrqHandler \*handler\)' \
            "$irq_registry_header" ||
        ! rg -q 'registerHardHandler\(uint8_t irq, HardIrqHandler \*handler\)' \
            "$irq_registry_header" ||
        ! rg -q 'dispatchHard\(' "$irq_registry_header" ||
        ! rg -q 'dispatchThreaded\(' "$irq_registry_header" ||
        ! rg -q 'deliveryOf\(publication\) != delivery' \
            "$irq_registry_source" ||
        ! rg -q 'deliveryOf\(publication\) != Delivery::HardOnly' \
            "$irq_registry_source" ||
        ! rg -q 'deliveryOf\(publication\) != Delivery::Threaded' \
            "$irq_registry_source"; then
        echo "The IRQ registry lost typed delivery or mixed-line rejection."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)dispatchThreaded\([^)]*\).*?if \(!dispatchThread \|\| !Processor::getInterrupts\(\)\).*?getHostedSignalDepth\(\)' \
        "$irq_registry_source"; then
        echo "Threaded IRQ dispatch no longer rejects atomic or signal context."
        failed=1
    fi

    local threaded_irq_header=src/system/include/pedigree/kernel/machine/ThreadedIrqDispatcher.h
    local threaded_irq_source=src/system/kernel/machine/ThreadedIrqDispatcher.cc
    local hosted_irq_source=src/system/kernel/machine/hosted/IrqManager.cc
    local hosted_machine_source=src/system/kernel/machine/hosted/Machine.cc
    local threaded_irq_regressions=src/modules/system/hosted-smoke/threaded-irq-regressions.cc
    if ! rg -q 'mutable Spinlock m_StateLock' "$threaded_irq_header" ||
        ! rg -q -U \
            '(?s)Line::markPending\(size_t cookie\).*?LockGuard<Spinlock> guard\(m_StateLock\).*?if \(!m_Started \|\| m_Stopping\).*?m_PendingCookie = cookie.*?m_WakePublished = true' \
            "$threaded_irq_source" ||
        ! rg -q -U \
            '(?s)Line::beginStop\(\).*?LockGuard<Spinlock> guard\(m_StateLock\).*?m_Stopping = true.*?m_Work\.release\(\)' \
            "$threaded_irq_source"; then
        echo "Threaded IRQ publication and shutdown lost their shared admission lock."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)Line::run\(\).*?TerminationDeferral workerLifetime.*?acquireForCompletion\(\).*?m_Callback\(.*?m_CompletedCookie = cookie' \
        "$threaded_irq_source" ||
        ! rg -q 'completedCookieForTest' "$threaded_irq_header"; then
        echo "Threaded IRQ workers lost owned lifetime or completion generations."
        failed=1
    fi

    if rg -q 'SIGWINCH' "$hosted_irq_source" ||
        ! rg -q -U \
            '(?s)dispatchThreadedLine\(.*?cookie != __atomic_load_n\(.*?m_ThreadedCookies.*?dispatchThreaded\(' \
            "$hosted_irq_source" ||
        ! rg -q -U \
            '(?s)shutdownThreaded\(\).*?HostedSchedulerTimer::instance\(\)\.uninitialise\(\)' \
            "$hosted_machine_source"; then
        echo "Hosted threaded IRQ delivery lost stale-work or live-service teardown protection."
        failed=1
    fi

    if ! rg -q 'Candidate candidates\[MaxHandlerSlots\]' \
        "$irq_registry_source" ||
        ! rg -q 'irq-threaded-dispatcher-coalescing' \
            "$threaded_irq_regressions"; then
        echo "Threaded IRQ callback snapshots or deterministic coalescing coverage are missing."
        failed=1
    fi

    matches=$(rg -n \
        '(->|\.)register(Isa|Pci)IrqHandler\(' \
        src --glob '*.{cc,h}' || true)
    if [[ -n "$matches" ]]; then
        echo "A legacy hard callback entered the not-yet-enabled threaded registration API:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q -U \
        'class EXPORTED_PUBLIC SplitIrqHandler[[:space:]]*:[[:space:]]*private HardIrqHandler,[[:space:]]*private RequestQueue' \
        "$split_irq_header" ||
        ! rg -q -U \
            'virtual HardIrqDisposition[[:space:]]+hardIrq\(' \
            "$split_irq_header" ||
        ! rg -q \
            'virtual void threadedIrq\(size_t work\)' \
            "$split_irq_header" ||
        ! rg -q \
            'virtual bool quiesceIrqSources\(\)' \
            "$split_irq_header" ||
        ! rg -q \
            'virtual void rearmIrqSources\(size_t work\)' \
            "$split_irq_header"; then
        echo "The split IRQ adapter lost its hard/thread context boundary."
        failed=1
    fi

    matches=$(rg -n -U \
        'threadedIrq\([^)]*InterruptState' \
        "$split_irq_header" "$split_irq_source" || true)
    if [[ -n "$matches" ]]; then
        echo "InterruptState escaped into a split IRQ bottom half:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q -U \
        '(?s)tryPublishWork\(\).*republishWhileReleasing\(m_WorkRequest, 0\).*enqueueFromInterrupt\(m_WorkRequest, 0\)' \
        "$split_irq_source"; then
        echo "The split IRQ adapter lost its release-seam publication order."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)publishWork\(size_t work\).*?__atomic_or_fetch\(&m_PendingWork, work, __ATOMIC_ACQ_REL\).*?tryPublishWork\(\)' \
        "$split_irq_source"; then
        echo "The split IRQ adapter published before recording pending work."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)shutdownSplitIrq\(\).*?Thread \*current.*?if \(!current \|\| !Processor::getInterrupts\(\)\).*?getHostedSignalDepth\(\).*?m_pThread == current.*?m_Quiescing = true.*?quiesceIrqSources\(\).*?unregisterHandler\(registration\.id, this\)' \
        "$split_irq_source"; then
        echo "Split IRQ shutdown can mutate state from an atomic callback context."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)shutdownSplitIrq\(\).*?m_Quiescing = true.*?quiesceIrqSources\(\).*?unregisterHandler\(registration\.id, this\).*?drain\(\).*?quiesceIrqSources\(\).*?m_Stopping = 1.*?RequestQueue::destroy\(\)' \
        "$split_irq_source"; then
        echo "The split IRQ adapter lost its quiesce, callback-drain, or stop order."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)executeRequest\(.*?threadedIrq\(work\).*?LockGuard<Spinlock> guard\(m_StateLock\).*?if \(!m_Quiescing\).*?rearmIrqSources\(work\).*?m_CompletedBatches' \
        "$split_irq_source"; then
        echo "A split IRQ bottom half can rearm outside the shutdown gate."
        failed=1
    fi

    local split_irq_regressions=src/modules/system/hosted-smoke/split-irq-regressions.cc
    if ! rg -q 'setHandlerPinHook\(holdPinnedHardCallback\)' \
        "$split_irq_regressions" ||
        ! rg -q 'hasCallbackDrainWait\(' "$split_irq_regressions" ||
        ! rg -q 'split-irq-hard-callback-drain' \
            "$split_irq_regressions" ||
        ! rg -q 'split-irq-atomic-shutdown-rejected' \
            "$split_irq_regressions" ||
        ! rg -q 'split-irq-hard-shutdown-rejected' \
            "$split_irq_regressions"; then
        echo "Hosted split IRQ lifecycle race coverage is incomplete."
        failed=1
    fi

    if ! rg -q 'Explicit hard-IRQ callback interface' "$irq_handler_header" ||
        ! rg -q 'it is not the normal threaded-delivery API' \
            "$irq_handler_header"; then
        echo "The explicit hard-IRQ API lost its context warning."
        failed=1
    fi

    local timer_registry_source=src/system/kernel/machine/TimerHandlerRegistry.cc
    local timer_registry_header=src/system/include/pedigree/kernel/machine/TimerHandlerRegistry.h
    matches=$(rg -n \
        'Scheduler::instance\(\)\.yield\(\)' \
        "$timer_registry_source" || true)
    if [[ -n "$matches" ]]; then
        echo "The timer callback drain reverted to scheduler yielding:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q 'WaitQueue m_DispatchWaiters' "$timer_registry_header" ||
        ! rg -q 'm_DispatchWaiters\.acquire\(\)' \
            "$timer_registry_source" ||
        ! rg -q 'guard\.waitForCompletion\(' \
            "$timer_registry_source" ||
        ! rg -q 'WaitQueue::Channel\(slot, drainGeneration\)' \
            "$timer_registry_source"; then
        echo "The timer callback drain escaped its generation-keyed wait."
        failed=1
    fi

    if ! rg -q 'guard\.wakeAll\(' "$timer_registry_source" ||
        ! rg -q 'WaitQueue::Channel\(&slot, drainGeneration\)' \
            "$timer_registry_source" ||
        ! rg -q -U \
            'mode != SlotMode::Draining && mode != SlotMode::Deferred[^}]*!selfRemovalOf\(publication\)[^{]*\{[[:space:]]*return true;[[:space:]]*\}[^}]*m_DispatchWaiters\.acquire\(\)' \
            "$timer_registry_source"; then
        echo "The timer callback drain escaped its closed-mode wake."
        failed=1
    fi

    if ! rg -q \
        'generationOf\(finalPublication\) != drainGeneration' \
        "$timer_registry_source" ||
        ! rg -q 'synchronousDrainOf\(finalPublication\)' \
            "$timer_registry_source" ||
        ! rg -q \
            'selfRemovalOf\(finalPublication\) && !synchronousDrain' \
            "$timer_registry_source" ||
        ! rg -q 'retireSlot\(\*slot, finalPublication, handler\)' \
            "$timer_registry_source"; then
        echo "The timer callback drain lost its slot-reuse boundary."
        failed=1
    fi

    local pagefault_registry_source=src/system/kernel/core/processor/PageFaultHandler.cc
    matches=$(rg -n \
        'Scheduler::instance\(\)\.yield\(\)|Processor::pause\(\)' \
        "$pagefault_registry_source" || true)
    if [[ -n "$matches" ]]; then
        echo "The page-fault callback drain reverted to busy waiting:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q 'm_DispatchWaiters\.acquire\(\)' \
        "$pagefault_registry_source" ||
        ! rg -q 'guard\.waitForCompletion\(' \
            "$pagefault_registry_source" ||
        ! rg -q 'WaitQueue::Channel\(slot, drainGeneration\)' \
            "$pagefault_registry_source"; then
        echo "The page-fault callback drain escaped its generation-keyed wait."
        failed=1
    fi

    if ! rg -q 'guard\.wakeAll\(' "$pagefault_registry_source" ||
        ! rg -q \
            'WaitQueue::Channel\(releasedSlot, drainGeneration\)' \
            "$pagefault_registry_source" ||
        ! rg -q -U \
            'mode != SlotMode::Draining &&[[:space:]]*mode != SlotMode::Deferred\)[[:space:]]*\{[[:space:]]*return;[[:space:]]*\}[^}]*m_DispatchWaiters\.acquire\(\)' \
            "$pagefault_registry_source"; then
        echo "The page-fault callback drain escaped its closed-mode wake."
        failed=1
    fi

    if ! rg -q \
        'generationOf\(finalPublication\) != drainGeneration' \
        "$pagefault_registry_source" ||
        ! rg -q 'retireSlot\(\*slot, finalPublication, pHandler\)' \
            "$pagefault_registry_source"; then
        echo "The page-fault callback drain lost its slot-reuse boundary."
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

    if ! rg -q -U \
        '(?s)\{[[:space:]]*LockGuard<Spinlock> guard\(m_Lock\);[[:space:]]*m_bDestroying = true;[[:space:]]*\}[[:space:]]*for \(Vector<Thread \*>::Iterator.*?closeExternalLeaseAdmissionAndDrain\(\)' \
        src/system/kernel/core/process/Process.cc; then
        echo "Process teardown can retain its topology spinlock while draining."
        failed=1
    fi

    if ! rg -q -U \
        'Machine::instance\(\)\.deinitialise\(\);[[:space:]]*Processor::setInterrupts\(false\);[[:space:]]*// Shut down the various pieces created by Processor' \
        src/system/kernel/core/main.cc; then
        echo "Machine teardown no longer runs while callback drains can schedule."
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
