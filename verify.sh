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

    local request_queue_header=src/system/include/pedigree/kernel/utilities/RequestQueue.h
    local request_queue_source=src/system/kernel/utilities/RequestQueue.cc
    local request_queue_regressions=src/modules/system/hosted-smoke/requestqueue-regressions.cc
    local request_queue_sample
    request_queue_sample=$(sed -n \
        '/RequestQueueOverrunChecker::sample(/,/^void RequestQueue::RequestQueueOverrunChecker::timer/p' \
        "$request_queue_source")
    if ! rg -q 'm_WorkerProgressGeneration' "$request_queue_header" ||
        ! rg -q -U \
            '(?s)RequestQueueOverrunChecker::sample\(.*?m_nTotalRequests\.value\(\).*?m_nActiveRequests\.value\(\).*?currentSize = total - active.*?m_WorkerProgressGeneration.*?m_HasBacklogBaseline.*?OverrunStatus::Stalled.*?OverrunStatus::Overloaded' \
            "$request_queue_source" ||
        rg -q 'm_pRequestQueue\[' <<<"$request_queue_sample" ||
        ! rg -q -U \
            '(?s)RequestQueueOverrunChecker::timer\(.*?OverrunStatus::Stalled.*?OverrunStatus::Overloaded' \
            "$request_queue_source" ||
        ! rg -q 'requestqueue-watchdog-progress' \
            "$request_queue_regressions"; then
        echo "RequestQueue watchdog snapshots again confuse admission with stalled progress."
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
    if rg -q 'HardIrqHandler|registerHard(Isa|Pci)IrqHandler' \
            "$cdi_irq_source" ||
        ! rg -q 'class CdiIrqHandler : public IrqHandler' \
            "$cdi_irq_source" ||
        ! rg -q 'IrqPolicy::levelThreaded\(\)' "$cdi_irq_source" ||
        ! rg -q 'IrqDisposition::Handled' "$cdi_irq_source" ||
        ! rg -q 'LockGuard<Mutex> lock\(irqSlotLock\)' \
            "$cdi_irq_source"; then
        echo "CDI callbacks escaped their ordinary threaded IRQ boundary."
        failed=1
    fi

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

    local cdi_cmos_source=src/modules/drivers/common/cdi/CdiCmos.cc
    local rtc_header=src/system/kernel/machine/mach_pc/Rtc.h
    local raw_cdi_cmos_pattern='cdi_(inb|outb)[[:space:]]*\([[:space:]]*0[xX]0*7[01]'
    if ! printf '%s\n' 'cdi_outb(0x70, index);' \
        'return cdi_inb(0X0071);' | rg -q "$raw_cdi_cmos_pattern"; then
        echo "The raw CDI CMOS-port detector failed its self-test."
        failed=1
    fi

    matches=$(rg -n \
        "$raw_cdi_cmos_pattern" \
        src/modules/drivers/common/cdi || true)
    if [[ -n "$matches" ]]; then
        echo "CDI bypassed the RTC-owned CMOS selector/data transaction:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q 'return Rtc::readCmos\(index\)' "$cdi_cmos_source" ||
        ! rg -q 'Rtc::writeCmos\(index, value\)' "$cdi_cmos_source"; then
        echo "CDI CMOS access escaped the RTC-owned compatibility API."
        failed=1
    fi

    if ! rg -q \
        'static EXPORTED_PUBLIC uint8_t readCmos\(uint8_t index\)' \
        "$rtc_header" ||
        ! rg -q \
            'static EXPORTED_PUBLIC void writeCmos\(uint8_t index, uint8_t value\)' \
            "$rtc_header" ||
        ! rg -q 'drivers/common/cdi/CdiCmos\.cc' src/modules/CMakeLists.txt; then
        echo "The exported or compile-checked RTC CMOS boundary regressed."
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
        ! rg -q 'm_RequestedEnabled\[2\] = true' "$pic_state" ||
        ! rg -q 'void rebuildMask\(\)' "$pic_state" ||
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
        'controllerAck = m_IrqState\.controllerAck\(irq\)' "$pic_source" ||
        ! rg -q \
            'lineRelease = m_IrqState\.lineRelease\(irq\)' "$pic_source" ||
        ! rg -q \
            'controllerAck == IrqControllerAck::BeforeHardStage' \
            "$pic_source" ||
        ! rg -q \
            'controllerAck == IrqControllerAck::AfterHardStage' \
            "$pic_source" ||
        ! rg -q \
            'lineRelease == IrqLineRelease::AfterThreadedCompletion' \
            "$pic_source"; then
        echo "PIC dispatch no longer uses one typed policy snapshot."
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
    local irq_unpublish_body
    irq_unpublish_body=$(sed -n \
        '/IrqHandlerRegistry::unpublishDispatch(/,/^}/p' \
        "$irq_registry_source")
    matches=$(printf '%s\n' "$irq_unpublish_body" | \
        rg -n 'WaitQueue|Scheduler::|schedule\(|wake(All|One)?\(' || true)
    if [[ -n "$matches" ]]; then
        echo "Hard IRQ callback release entered a wait or scheduler path:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q -U \
        '(?s)unregisterHandler\(uint8_t irq, IrqHandlerBase \*handler\).*?setDebugState\(.*?Thread::CallbackDrain.*?while \(hasActiveDispatch\(\*slot, drainingPublication\)\).*?Scheduler::instance\(\)\.yield\(\).*?setDebugState\(previousDebugState, previousDebugAddress\)' \
        "$irq_registry_source"; then
        echo "The IRQ callback drain lost its ordinary-context cooperative wait."
        failed=1
    fi

    local processor_header=src/system/include/pedigree/kernel/processor/Processor.h
    local processor_source=src/system/kernel/core/processor/Processor.cc
    local hard_irq_context_header=src/system/kernel/core/processor/DeviceHardIrqContext.h
    if ! rg -q 'static bool inDeviceHardIrq\(\)' "$processor_header" ||
        ! rg -q \
            'return information\(\)\.m_DeviceHardIrqDepth != 0' \
            "$processor_source" ||
        ! rg -q \
            'static size_t deviceHardIrqDepthForTest\(\)' \
            "$processor_header" ||
        ! rg -q 'class DeviceHardIrqContext' "$hard_irq_context_header" ||
        ! rg -q \
            'class SuspendDeviceHardIrqContext' "$hard_irq_context_header" ||
        ! rg -q -U \
            '(?s)DeviceHardIrqContext::DeviceHardIrqContext\([^)]*previousDepth[^)]*restorationArmed\).*?previousDepth = m_PreviousDepth.*?m_RestorationArmed = true.*?\+\+m_Information\.m_DeviceHardIrqDepth.*?DeviceHardIrqContext::~DeviceHardIrqContext\(\).*?restoreDepth\(m_PreviousDepth\).*?m_RestorationArmed = false' \
            "$processor_source" ||
        ! rg -q -U \
            '(?s)SuspendDeviceHardIrqContext::SuspendDeviceHardIrqContext\(\).*?m_DeviceHardIrqDepth == 1.*?m_DeviceHardIrqDepth = 0.*?SuspendDeviceHardIrqContext::~SuspendDeviceHardIrqContext\(\).*?m_DeviceHardIrqDepth == 0.*?m_DeviceHardIrqDepth = 1' \
            "$processor_source"; then
        echo "The per-processor device hard-IRQ context boundary regressed."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)DeviceHardIrqContext deviceHardIrqContext\([^;]*previousDeviceHardIrqDepth[^;]*restoreDeviceHardIrqDepth[^;]*\);.*?static_cast<HardIrqHandler \*>\(handler\)->irq\(irq, state\);.*?unpublishDispatch' \
        "$irq_registry_source" ||
        ! rg -q -U \
            '(?s)abandonDispatch\(void \*context\).*?restoreDeviceHardIrqDepth.*?DeviceHardIrqContext::restoreDepth\([^;]*previousDeviceHardIrqDepth\);.*?restoreDeviceHardIrqDepth = false;.*?unpublishDispatch' \
        "$irq_registry_source"; then
        echo "Hard IRQ callbacks escaped their return-or-abandon depth scope."
        failed=1
    fi

    if ! rg -q 'size_t controllerGeneration' \
            src/system/include/pedigree/kernel/machine/IrqHandlerRegistry.h ||
        ! rg -q 'hardDispatchState' "$irq_registry_source" ||
        ! rg -q 'nested\.activeCount == 2' \
            src/modules/system/hosted-smoke/irq-regressions.cc ||
        ! rg -q 'irq-hosted-deferred-retiring-diagnostics' \
            src/modules/system/hosted-smoke/threaded-irq-regressions.cc; then
        echo "Hard IRQ diagnostics lost exact-or-ambiguous hazard generations."
        failed=1
    fi

    if ! rg -q 'abandonedNestedDispatchDepthCleanup' \
        src/modules/system/hosted-smoke/irq-regressions.cc ||
        ! rg -q \
            'Processor::deviceHardIrqDepthForTest\(\) == 0' \
            src/modules/system/hosted-smoke/irq-regressions.cc; then
        echo "Hosted IRQ abandonment lost exact depth restoration coverage."
        failed=1
    fi

    local hard_irq_suspend_users
    hard_irq_suspend_users=$(rg -l \
        'SuspendDeviceHardIrqContext schedulerTimerContext' \
        src/system/kernel/machine --glob '*.cc' | sort || true)
    local expected_hard_irq_suspend_users
    expected_hard_irq_suspend_users=$(printf '%s\n' \
        src/system/kernel/machine/hosted/SchedulerTimer.cc \
        src/system/kernel/machine/mach_pc/Pit.cc)
    if [[ "$hard_irq_suspend_users" != "$expected_hard_irq_suspend_users" ]] ||
        ! rg -q -U \
            '(?s)SuspendDeviceHardIrqContext schedulerTimerContext;.*?m_Handler->timer\(0, state\)' \
            src/system/kernel/machine/mach_pc/Pit.cc ||
        ! rg -q -U \
            '(?s)SuspendDeviceHardIrqContext schedulerTimerContext;.*?hook\(delta, state\).*?m_Handler->timer\(delta, state\)' \
            src/system/kernel/machine/hosted/SchedulerTimer.cc; then
        echo "The scheduler-timer device hard-IRQ exception escaped its two audited call sites."
        failed=1
    fi

    local irq_regressions=src/modules/system/hosted-smoke/irq-regressions.cc
    if ! rg -q 'dispatchPinnedHandler' "$irq_regressions" ||
        ! rg -q 'dispatchHandlerForTest\(' "$irq_regressions"; then
        echo "IRQ callback-drain coverage no longer uses ordinary test dispatch."
        failed=1
    fi
    local irq_pin_hook_body
    irq_pin_hook_body=$(sed -n '/void handlerPinHook(/,/^}/p' "$irq_regressions")
    matches=$(printf '%s\n' "$irq_pin_hook_body" | \
        rg -n 'Scheduler::|schedule\(|raise\(' || true)
    if [[ -n "$matches" ]]; then
        echo "The IRQ drain harness schedules from its handler-pin hook:"
        echo "$matches"
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
    local irq_manager_header=src/system/include/pedigree/kernel/machine/IrqManager.h
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
            "$irq_handler_header" ||
        ! rg -q 'Quiesced' "$irq_handler_header" ||
        ! rg -q 'struct ThreadedDispatchResult' "$irq_registry_header" ||
        ! rg -q 'bool allowRearm' "$irq_registry_header" ||
        ! rg -q 'result\.admitted && result\.allowRearm' \
            src/system/kernel/machine/mach_pc/Pic.cc ||
        ! rg -q 'irq-threaded-quiesced-rearm' \
            src/modules/system/hosted-smoke/irq-regressions.cc; then
        echo "The IRQ API lost its explicit thread and hard-context types."
        failed=1
    fi

    local ata_controller_header=src/modules/drivers/common/ata/AtaController.h
    local isa_ata_source=src/modules/drivers/common/ata/IsaAtaController.cc
    local pci_ata_source=src/modules/drivers/common/ata/PciAtaController.cc
    if ! rg -q \
            'class AtaController : public ScsiController, public IrqHandler' \
            "$ata_controller_header" ||
        rg -q 'registerHard(Isa|Pci)IrqHandler|HardIrqHandler' \
            "$ata_controller_header" "$isa_ata_source" "$pci_ata_source" ||
        ! rg -q 'IrqPolicy::edgeThreaded\(\)' "$isa_ata_source" ||
        ! rg -q 'IrqPolicy::pciIntxThreaded\(\)' "$pci_ata_source" ||
        ! rg -q 'IrqDisposition::Handled' \
            "$isa_ata_source" "$pci_ata_source"; then
        echo "ATA completion escaped its ordinary threaded IRQ boundary."
        failed=1
    fi

    local ne2k_header=src/modules/drivers/x86/ne2k/Ne2k.h
    local ne2k_source=src/modules/drivers/x86/ne2k/Ne2k.cc
    if ! rg -q 'class Ne2k : public Network, public IrqHandler' \
            "$ne2k_header" ||
        rg -q 'registerHard(Isa|Pci)IrqHandler|HardIrqHandler' \
            "$ne2k_header" "$ne2k_source" ||
        ! rg -q 'IrqPolicy::pciIntxThreaded\(\)' "$ne2k_source" ||
        ! rg -q 'constexpr size_t PassLimit' "$ne2k_source" ||
        ! rg -q 'constexpr size_t RingPageCount' "$ne2k_source" ||
        ! rg -q 'bool Ne2k::waitForRemoteDma\(\)' "$ne2k_source" ||
        ! rg -q 'bool Ne2k::recoverReceiveOverflow' "$ne2k_source" ||
        ! rg -q 'CommandPage1Start' "$ne2k_source" ||
        ! rg -q -U \
            '(?s)m_pBase->write8\(irqStatus, NE_ISR\);.*?if \(irqStatus & \(InterruptReceive \| InterruptReceiveError\)\).*?recv\(\)' \
            "$ne2k_source"; then
        echo "NE2K escaped its bounded threaded IRQ and ring-drain boundary."
        failed=1
    fi

    local threecom_header=src/modules/drivers/common/3c90x/3Com90x.h
    local threecom_source=src/modules/drivers/common/3c90x/3Com90x.cc
    if ! rg -q 'class Nic3C90x : public Network, public IrqHandler' \
            "$threecom_header" ||
        rg -q 'HardIrqHandler|m_PendingPackets|m_ReceiveThread' \
            "$threecom_header" "$threecom_source" ||
        ! rg -q 'IrqPolicy::pciIntxThreaded\(\)' "$threecom_source" ||
        ! rg -q 'constexpr size_t PassLimit' "$threecom_source" ||
        ! rg -q 'constexpr size_t ResetCommandPollLimit' "$threecom_source" ||
        ! rg -q 'volatile uint32_t UpPktStatus' "$threecom_header" ||
        ! rg -q 'm_RxConsumerIndex' \
            "$threecom_header" "$threecom_source" ||
        ! rg -q 'FENCE\(\)' "$threecom_source"; then
        echo "3C90x escaped its bounded threaded IRQ and DMA ownership boundary."
        failed=1
    fi

    local ehci_header=src/modules/drivers/common/usb-hcd/Ehci.h
    local ehci_source=src/modules/drivers/common/usb-hcd/Ehci.cc
    local ohci_header=src/modules/drivers/common/usb-hcd/Ohci.h
    local ohci_source=src/modules/drivers/common/usb-hcd/Ohci.cc
    local uhci_header=src/modules/drivers/common/usb-hcd/Uhci.h
    local uhci_source=src/modules/drivers/common/usb-hcd/Uhci.cc
    local usb_callback_delivery=src/modules/drivers/common/usb-hcd/CallbackDelivery.h
    local usb_hub_header=src/modules/system/usb/UsbHub.h
    local usb_hub_source=src/modules/system/usb/UsbHub.cc
    local usb_hub_device=src/modules/drivers/common/usb-hub/UsbHubDevice.cc
    local usb_callback_regressions=src/modules/system/hosted-smoke/usb-callback-delivery-regressions.cc
    local usb_port_regressions=src/modules/system/hosted-smoke/usb-hcd-port-change-regressions.cc
    if rg -q 'registerHard(Isa|Pci)IrqHandler|HardIrqHandler' \
            "$ehci_header" "$ehci_source" "$ohci_header" "$ohci_source" \
            "$uhci_header" "$uhci_source" ||
        rg -q 'm_IgnoredPorts|m_CompletionDeliveryLock' \
            "$ehci_header" "$ehci_source" "$ohci_header" "$ohci_source" \
            "$uhci_header" "$uhci_source" "$usb_hub_header" \
            "$usb_hub_source" ||
        ! rg -q 'IrqPolicy::pciIntxThreaded\(\)' "$ehci_source" ||
        ! rg -q 'IrqPolicy::pciIntxThreaded\(\)' "$ohci_source" ||
        ! rg -q 'IrqPolicy::pciIntxThreaded\(\)' "$uhci_source" ||
        ! rg -q 'IrqDisposition::Quiesced' "$ehci_source" ||
        ! rg -q 'IrqDisposition::Quiesced' "$ohci_source" ||
        ! rg -q 'IrqDisposition::Quiesced' "$uhci_source" ||
        ! rg -q 'constexpr size_t HaltPollLimit' "$ehci_source" ||
        ! rg -q 'constexpr size_t EdListCount' "$ohci_source" ||
        ! rg -q 'constexpr size_t TransitionPollLimit' "$uhci_source" ||
        ! rg -q 'setLegacySupportControl\(0x8F00\)' "$uhci_source" ||
        ! rg -q 'class CallbackDeliveryQueue' "$usb_callback_delivery" ||
        ! rg -q 'bool drain\(const Key &key\)' "$usb_callback_delivery" ||
        ! rg -q 'm_CompletionDeliveries\.publish' \
            "$ehci_source" "$ohci_source" "$uhci_source" ||
        ! rg -q 'deferConnectionChangeIfSuppressed' \
            "$ehci_source" "$ohci_source" "$uhci_source" ||
        ! rg -q 'replaySuppressedConnectionChange' \
            "$ehci_source" "$ohci_source" "$uhci_source" ||
        ! rg -q 'attachToUpstreamHub\(m_pHub\)' "$usb_hub_device" ||
        ! rg -q 'UsbHub \*m_RootHub' "$usb_hub_header" ||
        ! rg -q 'usb-callback-pending-steal' "$usb_callback_regressions" ||
        ! rg -q 'usb-callback-running-drain' "$usb_callback_regressions" ||
        ! rg -q 'usb-hcd-port-change-suppression-state' \
            "$usb_port_regressions" ||
        ! rg -q 'nested hubs did not retain their root-controller association' \
            "$usb_port_regressions"; then
        echo "USB HCD interrupt work escaped its threaded and bounded teardown boundary."
        failed=1
    fi

    if ! rg -q 'enum class IrqTrigger' "$irq_manager_header" ||
        ! rg -q 'enum class IrqControllerAck' "$irq_manager_header" ||
        ! rg -q 'enum class IrqLineRelease' "$irq_manager_header" ||
        ! rg -q 'class IrqPolicy' "$irq_manager_header" ||
        ! rg -q 'static constexpr IrqPolicy pciIntxThreaded' \
            "$irq_manager_header" ||
        ! rg -q 'static constexpr IrqPolicy syntheticHard' \
            "$irq_manager_header" ||
        rg -q 'bool bEdge|legacy(Hard|Threaded)' "$irq_manager_header" ||
        ! python3 scripts/check-irq-policy-registrations.py; then
        echo "IRQ registration escaped the explicit named-policy API."
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
    local thread_header=src/system/include/pedigree/kernel/process/Thread.h
    local thread_source=src/system/kernel/core/process/Thread.cc
    local scheduler_header=src/system/include/pedigree/kernel/process/PerProcessorScheduler.h
    local scheduler_source=src/system/kernel/core/process/PerProcessorScheduler.cc
    local round_robin_source=src/system/kernel/core/process/RoundRobin.cc
    if rg -q 'Semaphore|Spinlock m_StateLock|m_WakePublished' \
            "$threaded_irq_header" ||
        ! rg -q 'SchedulerReadyPredicate' "$thread_header" ||
        ! rg -q 'setSchedulerReadyPredicate\(workerReady, this\)' \
            "$threaded_irq_source" ||
        ! rg -q -U \
            '(?s)ThreadedIrqDispatcher::shutdown\(\).*?isCurrentWorker\(\)' \
            "$threaded_irq_source" ||
        ! rg -q 'isEligible\(pThread\)' "$round_robin_source" ||
        ! rg -q -U \
            '(?s)void Thread::shutdown\(\).*?setStatus\(Thread::AwaitingJoin\)' \
            "$thread_source" ||
        ! rg -q -U \
            '(?s)g_HostedSchedulerPredicateReady = false.*?Thread::AwaitingJoin.*?threadStatusChanged\(pThread\).*?!pThread->m_bReadyQueued' \
            "$round_robin_source" ||
        ! rg -q 'ringIrqWorkDoorbell' "$scheduler_header" ||
        ! rg -q -U \
            '(?s)ringIrqWorkDoorbell\(\).*?m_IrqWorkDoorbell = 1' \
            "$scheduler_source" ||
        ! rg -q -U \
            '(?s)PerProcessorScheduler::timer\([^)]*\).*?m_IrqWorkDoorbell\.compareAndSwap\(1, 0\).*?schedule\(\)' \
            "$scheduler_source" ||
        rg -q 'serviceIrqWorkDoorbell\(' \
            src/system/kernel/core/processor/hosted/InterruptManager.cc \
            src/system/kernel/core/processor/x64/InterruptManager.cc ||
        ! rg -q -U \
            '(?s)Line::publishFromInterrupt\(size_t cookie\).*?__atomic_fetch_add\(.*?m_PublicationState.*?PublicationClosed.*?__atomic_store_n\(&m_PendingCookie, cookie.*?ringIrqWorkDoorbell\(\).*?__atomic_fetch_sub\(.*?m_PublicationState' \
            "$threaded_irq_source" ||
        ! rg -q -U \
            '(?s)Line::beginStop\(\).*?__atomic_fetch_or\(.*?m_PublicationState, PublicationClosed' \
            "$threaded_irq_source"; then
        echo "Threaded IRQ publication lost its atomic scheduler doorbell."
        failed=1
    fi

    local threaded_publish_body
    threaded_publish_body=$(sed -n \
        '/Line::publishFromInterrupt(size_t cookie)/,/Line::hasPending() const/p' \
        "$threaded_irq_source")
    matches=$(printf '%s\n' "$threaded_publish_body" | \
        rg -n \
            'LockGuard|Spinlock|Semaphore|WaitQueue|RequestQueue|new[[:space:]]|delete[[:space:]]|FATAL|ERROR|WARNING|NOTICE|while[[:space:]]*\(|for[[:space:]]*\(' || true)
    if [[ -n "$matches" ]]; then
        echo "Threaded IRQ hard publication contains a blocking or unbounded operation:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q -U \
        '(?s)Line::run\(\).*?TerminationDeferral workerLifetime.*?__atomic_store_n\(.*?m_CallbackActive.*?__atomic_exchange_n\(.*?m_Callback\(.*?m_CompletedCookie.*?m_CallbackActive.*?Scheduler::instance\(\)\.yield\(\)' \
            "$threaded_irq_source" ||
        ! rg -q 'completedCookie' "$threaded_irq_header"; then
        echo "Threaded IRQ workers lost owned lifetime or completion generations."
        failed=1
    fi

    if rg -q 'SIGWINCH' "$hosted_irq_source" ||
        ! rg -q -U \
            '(?s)dispatchThreadedLine\(.*?cookie\s*!=\s*__atomic_load_n\(.*?m_ThreadedCookies.*?dispatchThreaded\(' \
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
            "$threaded_irq_regressions" ||
        ! rg -q 'currentIrqWorkDoorbellPendingForTest' \
            "$threaded_irq_regressions" ||
        ! rg -q 'selfShutdownRejected' \
            "$threaded_irq_regressions" ||
        ! rg -q 'irq-threaded-hosted-signal' \
            "$threaded_irq_regressions"; then
        echo "Threaded IRQ callback snapshots or deterministic coalescing coverage are missing."
        failed=1
    fi

    if rg -q 'm_LinePolicies' \
            src/system/kernel/machine/hosted/IrqManager.{cc,h} ||
        ! rg -q 'snapshotLineConfiguration' "$irq_registry_source" ||
        ! rg -q -U \
            '(?s)deliveryOf\(publication\) != delivery.*?slot\.policy.*?!= policy' \
            "$irq_registry_source" ||
        ! rg -q \
            'current\.mutationGeneration == configuration\.mutationGeneration' \
            "$hosted_irq_source" ||
        ! rg -q 'recordMissedPublication' \
            src/system/include/pedigree/kernel/machine/IrqDiagnosticSnapshotStore.h \
            "$hosted_irq_source" ||
        ! rg -q 'irq-hosted-diagnostic-policy-lifecycle' \
            "$threaded_irq_regressions" ||
        ! rg -q 'irq-hosted-diagnostic-missed-registry-snapshot' \
            "$threaded_irq_regressions"; then
        echo "Hosted IRQ diagnostics lost registry-owned policy or stale-publication repair."
        failed=1
    fi

    local hosted_diagnostic_publish_body
    hosted_diagnostic_publish_body=$(sed -n \
        '/HostedIrqManager::publishDiagnosticLine(uint8_t irq)/,/^}/p' \
        "$hosted_irq_source")
    local registry_line_snapshot_body
    registry_line_snapshot_body=$(sed -n \
        '/IrqHandlerRegistry::snapshotLineConfiguration(/,/^}/p' \
        "$irq_registry_source")
    matches=$(printf '%s\n%s\n' \
        "$hosted_diagnostic_publish_body" "$registry_line_snapshot_body" | \
        rg -n \
            'LockGuard|Spinlock|m_HandlerLock|Semaphore|WaitQueue|RequestQueue|Scheduler::|schedule\(|new[[:space:]]|delete[[:space:]]|while[[:space:]]*\(' || true)
    if [[ -n "$matches" ]]; then
        echo "Hosted IRQ diagnostic publication crossed its bounded hard-path boundary:"
        echo "$matches"
        failed=1
    fi

    local pic_source=src/system/kernel/machine/mach_pc/Pic.cc
    local pic_header=src/system/kernel/machine/mach_pc/Pic.h
    local pic_state=src/system/kernel/machine/mach_pc/PicIrqState.h
    local pc_source=src/system/kernel/machine/mach_pc/Pc.cc
    if ! rg -q 'ThreadedIrqDispatcher m_ThreadedDispatcher' "$pic_header" ||
        ! rg -q -U \
            '(?s)registerIsaIrqHandler\([^)]*IrqHandler \*handler, const IrqPolicy &policy.*?registerThreadedHandler\(irq, handler\).*?handlerRegistered\(irq, policy\)' \
            "$pic_source" ||
        ! rg -q -U \
            '(?s)registerPciIrqHandler\(.*?const IrqPolicy &policy.*?registerThreadedHandler\(irq, handler\).*?handlerRegistered\(irq, policy\)' \
            "$pic_source"; then
        echo "The PIC normal registration path escaped manager-owned threaded delivery."
        failed=1
    fi

    if rg -q -U \
        '(?s)Pic::unregisterHandler\(.*?\+\+m_UnregisterReservations\[irq\].*?advanceThreadedCookieLocked\(irq\).*?m_Handlers\.unregisterHandler\(' \
        "$pic_source" ||
        ! rg -q -U \
        '(?s)registerIsaIrqHandler\(.*?m_UnregisterReservations\[irq\].*?registerThreadedHandler' \
        "$pic_source" ||
        ! rg -q -U \
            '(?s)unregisterHandler\(.*?\+\+m_UnregisterReservations\[irq\].*?m_Handlers\.unregisterHandler\(.*?--m_UnregisterReservations\[irq\].*?UnregisterResult::Completed.*?handlerUnregistered\(irq\).*?advanceThreadedCookieLocked\(irq\)' \
            "$pic_source"; then
        echo "PIC registration can cross final-unregister accounting or invalidate failed removals."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)Pic::interrupt\(.*?beginThreadedDispatch\(irq\).*?lineRelease == IrqLineRelease::AfterThreadedCompletion.*?applyMaskLocked\(\).*?publishFromInterrupt\(.*?threadedCookie\).*?controllerAck == IrqControllerAck::AfterHardStage.*?eoiLocked\(irq\)' \
        "$pic_source" ||
        ! rg -q -U \
            '(?s)if \(!threadedPublished\).*?__atomic_add_fetch\(.*?m_ThreadedPublicationFailures' \
            "$pic_source" ||
        ! rg -q -U \
            '(?s)dispatchThreadedLine\(.*?dispatchThreaded\(irq\).*?completeThreadedDispatch\([^;]*result\.admitted && result\.allowRearm\)' \
            "$pic_source"; then
        echo "The PIC threaded path lost mask, EOI, publication, or rearm ordering."
        failed=1
    fi

    local pic_threaded_tail
    pic_threaded_tail=$(sed -n \
        '/^    if (threaded)$/,/^    bool bHandled/p' "$pic_source")
    matches=$(printf '%s\n' "$pic_threaded_tail" | \
        rg -n '(ERROR|WARNING|NOTICE|FATAL)(_NOLOCK)?\(' || true)
    if [[ -n "$matches" ]]; then
        echo "The PIC threaded publication tail logs from hard IRQ context:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q 'bool m_ThreadedPending\[LineCount\]' "$pic_state" ||
        ! rg -q 'bool m_RequestedEnabled\[LineCount\]' "$pic_state" ||
        ! rg -q 'IrqControllerAck m_ControllerAck\[LineCount\]' \
            "$pic_state" ||
        ! rg -q 'IrqLineRelease m_LineRelease\[LineCount\]' "$pic_state" ||
        ! rg -q 'm_TriggerModes\[irq\] = TriggerMode::Unconfigured' \
            "$pic_state" ||
        ! rg -q 'm_DispatchGenerations\[irq\] != dispatchGeneration' \
            "$pic_state" ||
        ! rg -q 'pic-threaded-trigger-policy' "$threaded_irq_regressions" ||
        ! rg -q 'irq-policy-orthogonality' "$irq_regressions"; then
        echo "PIC mask reasons, stale-generation protection, or trigger tests are incomplete."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)Pc::initialise3\(\).*?Pic::instance\(\)\.initialiseThreaded\(\).*?startReaderThread\(\)' \
        "$pc_source" ||
        ! rg -q -U \
            '(?s)Pc::deinitialise\(\).*?Pic::instance\(\)\.shutdownThreaded\(\).*?m_bInitialised = false' \
            "$pc_source"; then
        echo "PIC bottom-half workers escaped the schedulable machine lifecycle."
        failed=1
    fi

    local threaded_irq_registration_users
    threaded_irq_registration_users=$(rg -l \
        '(->|\.)register(Isa|Pci)IrqHandler\(' \
        src --glob '*.{cc,h}' \
        --glob '!src/modules/system/hosted-smoke/**' | sort || true)
    local expected_threaded_irq_registration_users
    expected_threaded_irq_registration_users=$(printf '%s\n' \
        src/modules/drivers/common/3c90x/3Com90x.cc \
        src/modules/drivers/common/ata/IsaAtaController.cc \
        src/modules/drivers/common/ata/PciAtaController.cc \
        src/modules/drivers/common/cdi/CdiIrq.cc \
        src/modules/drivers/common/usb-hcd/Ehci.cc \
        src/modules/drivers/common/usb-hcd/Ohci.cc \
        src/modules/drivers/common/usb-hcd/Uhci.cc \
        src/modules/drivers/x86/ne2k/Ne2k.cc)
    if [[ "$threaded_irq_registration_users" != \
        "$expected_threaded_irq_registration_users" ]]; then
        echo "Threaded IRQ registration escaped its audited production users:"
        echo "$threaded_irq_registration_users"
        failed=1
    fi

    if ! rg -q -U \
        'class EXPORTED_PUBLIC SplitIrqHandler[[:space:]]*:[[:space:]]*private HardIrqHandler' \
        "$split_irq_header" ||
        ! rg -q 'ThreadedIrqDispatcher m_Dispatcher' \
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

    matches=$(rg -n \
        'RequestQueue|InterruptRequest|enqueueFromInterrupt|republishWhileReleasing|m_RequestQueueWaiters|m_WorkRequest' \
        "$split_irq_header" "$split_irq_source" || true)
    if [[ -n "$matches" ]]; then
        echo "The split IRQ adapter still reaches the RequestQueue hard wake path:"
        echo "$matches"
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
        '(?s)publishWork\(size_t work\).*?__atomic_or_fetch\(&m_PendingWork, work, __ATOMIC_ACQ_REL\).*?m_Dispatcher\.publishFromInterrupt\(0, 1\)' \
        "$split_irq_source"; then
        echo "The split IRQ adapter published before recording pending work."
        failed=1
    fi

    if ! rg -q 'm_LifecycleBusy' "$split_irq_header" ||
        ! rg -q 'm_AcceptingRegistrations' "$split_irq_header" ||
        ! rg -q -U \
            '(?s)registerIsaSplitIrq\([^)]*\).*?SplitLifecycleGuard lifecycle\(m_LifecycleBusy\).*?!m_AcceptingRegistrations.*?registerHardIsaIrqHandler.*?m_RegistrationCount\+\+' \
            "$split_irq_source" ||
        ! rg -q -U \
            '(?s)shutdownSplitIrq\(\).*?SplitLifecycleGuard lifecycle\(m_LifecycleBusy\).*?m_AcceptingRegistrations = 0.*?m_Quiescing = true' \
            "$split_irq_source"; then
        echo "Split IRQ registration and shutdown lost lifecycle serialization."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)shutdownSplitIrq\(\).*?getHostedSignalDepth\(\).*?TerminationDeferral lifecycleTermination;.*?SplitLifecycleGuard lifecycle\(m_LifecycleBusy\)' \
        "$split_irq_source"; then
        echo "Split IRQ teardown can unwind while retaining lifecycle ownership."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)initialiseSplitIrq\(\).*?TerminationDeferral lifecycleTermination;.*?SplitLifecycleGuard lifecycle\(m_LifecycleBusy\)' \
        "$split_irq_source" ||
        ! rg -q -U \
            '(?s)registerIsaSplitIrq\([^)]*\).*?TerminationDeferral lifecycleTermination;.*?SplitLifecycleGuard lifecycle\(m_LifecycleBusy\)' \
            "$split_irq_source" ||
        ! rg -q -U \
            '(?s)registerPciSplitIrq\([^)]*\).*?TerminationDeferral lifecycleTermination;.*?SplitLifecycleGuard lifecycle\(m_LifecycleBusy\)' \
            "$split_irq_source"; then
        echo "A split IRQ lifecycle operation can unwind while retaining ownership."
        failed=1
    fi

    local split_publish_body
    split_publish_body=$(sed -n \
        '/SplitIrqHandler::publishWork(size_t work)/,/^#if HOSTED/p' \
        "$split_irq_source")
    matches=$(printf '%s\n' "$split_publish_body" | \
        rg -n \
            'LockGuard|Spinlock|Semaphore|WaitQueue|RequestQueue|new[[:space:]]|delete[[:space:]]|FATAL|ERROR|WARNING|NOTICE|while[[:space:]]*\(|for[[:space:]]*\(' || true)
    if [[ -n "$matches" ]]; then
        echo "Split IRQ hard publication contains a blocking or unbounded operation:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q -U \
        '(?s)shutdownSplitIrq\(\).*?Thread \*current.*?if \(!current \|\| !Processor::getInterrupts\(\)\).*?getHostedSignalDepth\(\).*?m_Dispatcher\.isCurrentWorker\(\).*?m_Quiescing = true.*?quiesceIrqSources\(\).*?unregisterHandler\(registration\.id, this\)' \
        "$split_irq_source"; then
        echo "Split IRQ shutdown can mutate state from an atomic callback context."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)shutdownSplitIrq\(\).*?m_Quiescing = true.*?quiesceIrqSources\(\).*?unregisterHandler\(registration\.id, this\).*?m_Stopping = 1.*?m_Dispatcher\.shutdown\(\).*?quiesceIrqSources\(\).*?m_PendingWork.*?m_Started = false' \
        "$split_irq_source"; then
        echo "The split IRQ adapter lost its quiesce, callback-drain, or stop order."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)dispatchThreaded\(\).*?__atomic_exchange_n\(.*?m_PendingWork.*?threadedIrq\(work\).*?LockGuard<Spinlock> guard\(m_StateLock\).*?if \(!m_Quiescing\).*?rearmIrqSources\(work\).*?m_CompletedBatches' \
        "$split_irq_source"; then
        echo "A split IRQ bottom half can rearm outside the shutdown gate."
        failed=1
    fi

    local split_irq_regressions=src/modules/system/hosted-smoke/split-irq-regressions.cc
    if ! rg -q 'setHandlerPinHook\(holdPinnedTestDispatch\)' \
        "$split_irq_regressions" ||
        ! rg -q 'hasCallbackDrainState\(' "$split_irq_regressions" ||
        ! rg -q 'registrationPublishedBeforeBookkeeping' \
            "$split_irq_regressions" ||
        ! rg -q 'split-irq-lifecycle-serialization' \
            "$split_irq_regressions" ||
        ! rg -q 'split-irq-hard-callback-drain' \
            "$split_irq_regressions" ||
        ! rg -q 'split-irq-atomic-shutdown-rejected' \
            "$split_irq_regressions" ||
        ! rg -q 'split-irq-hard-shutdown-rejected' \
            "$split_irq_regressions" ||
        ! rg -q 'split-irq-worker-shutdown-rejected' \
            "$split_irq_regressions" ||
        ! rg -q 'split-irq-shutdown-retry' \
            "$split_irq_regressions"; then
        echo "Hosted split IRQ lifecycle race coverage is incomplete."
        failed=1
    fi

    local split_pin_hook_body
    split_pin_hook_body=$(sed -n \
        '/void holdPinnedTestDispatch(/,/^}/p' \
        "$split_irq_regressions")
    matches=$(printf '%s\n' "$split_pin_hook_body" | \
        rg -n 'Scheduler::|schedule\(|raise\(' || true)
    if [[ -n "$matches" ]] ||
        ! rg -q 'dispatchPinnedSplitHandler' "$split_irq_regressions"; then
        echo "The split IRQ drain harness schedules from hard signal context."
        if [[ -n "$matches" ]]; then
            echo "$matches"
        fi
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
    local timer_handler_header=src/system/include/pedigree/kernel/machine/TimerHandler.h
    local scheduler_timer_handler_header=src/system/include/pedigree/kernel/machine/SchedulerTimerHandler.h
    local scheduler_timer_header=src/system/include/pedigree/kernel/machine/SchedulerTimer.h
    if ! rg -q 'virtual void timer\(uint64_t delta\) = 0' \
            "$timer_handler_header" ||
        rg -q 'InterruptState' "$timer_handler_header" ||
        ! rg -q 'timer\(uint64_t delta, InterruptState &state\) = 0' \
            "$scheduler_timer_handler_header" ||
        ! rg -q 'registerHandler\(SchedulerTimerHandler \*handler\)' \
            "$scheduler_timer_header" ||
        ! rg -q 'handler->timer\(delta\)' "$timer_registry_source"; then
        echo "Ordinary and hard scheduler timer callback contexts were conflated."
        failed=1
    fi

    local hosted_timer_source=src/system/kernel/machine/hosted/Timer.cc
    local hosted_timer_header=src/system/kernel/machine/hosted/Timer.h
    local hosted_scheduler_timer_source=src/system/kernel/machine/hosted/SchedulerTimer.cc
    local hosted_scheduler_timer_header=src/system/kernel/machine/hosted/SchedulerTimer.h
    local hosted_processor_source=src/system/kernel/core/processor/hosted/Processor.cc
    local timer_regressions=src/modules/system/hosted-smoke/timer-regressions.cc
    local scheduler_regressions=src/modules/system/hosted-smoke/scheduler-regressions.cc
    local hosted_timer_hard
    hosted_timer_hard=$(sed -n \
        '/HostedTimer::hardIrq(/,/^void HostedTimer::threadedIrq/p' \
        "$hosted_timer_source")
    if ! rg -q \
            'class HostedTimer : public Timer, private SplitIrqHandler' \
            "$hosted_timer_header" ||
        ! rg -q -U \
            '(?s)HostedTimer::hardIrq\(.*?getInterruptNumber\(\) != SIGUSR1.*?si_signo != SIGUSR1.*?si_code != SI_TIMER.*?si_overrun.*?recordFromInterrupt\(expirations\).*?work = 1.*?HardIrqDisposition::Deferred' \
            "$hosted_timer_source" ||
        rg -q \
            'sendEvent|m_Alarm|synchronise|getIrqManager|m_HandlerRegistry|while[[:space:]]*\(|compareAndSwap' \
            <<<"$hosted_timer_hard" ||
        ! rg -q -U \
            '(?s)HostedTimer::threadedIrq\(.*?m_PendingExpirations\.takeAll\(\).*?MaximumDelta / INTERVAL.*?FATAL\("HostedTimer elapsed-time batch overflowed"\).*?processTimerBatch\(delta\)' \
            "$hosted_timer_source" ||
        ! rg -q -U \
            '(?s)HostedTimer::processTimerBatch\(.*?m_AlarmLock\.acquire\(\).*?sendEvent\(.*?m_Alarms\.erase\(it\).*?m_AlarmLock\.release\(\)' \
            "$hosted_timer_source" ||
        ! rg -q -U \
            '(?s)HostedTimer::initialise3\(\).*?initialiseSplitIrq\(\).*?registerIsaSplitIrq\(.*?timer_settime\(' \
            "$hosted_timer_source"; then
        echo "HostedTimer work escaped its counted hard top half and ordinary bottom half."
        failed=1
    fi

    if ! rg -q -U \
            '(?s)HostedMachine::initialise3\(\).*?initialiseThreaded\(\).*?HostedTimer::instance\(\)\.initialise3\(\)' \
            "$hosted_machine_source" ||
        ! rg -q -U \
            '(?s)HostedMachine::deinitialise\(\).*?HostedTimer::instance\(\)\.uninitialise\(\).*?shutdownThreaded\(\).*?HostedSchedulerTimer::instance\(\)\.uninitialise\(\)' \
            "$hosted_machine_source"; then
        echo "Hosted timer workers escaped their schedulable machine lifecycle."
        failed=1
    fi

    if ! rg -q \
            'class HostedSchedulerTimer : public SchedulerTimer, private HardIrqHandler' \
            "$hosted_scheduler_timer_header" ||
        ! rg -q -U \
            '(?s)HostedSchedulerTimer::initialise\(\).*?timer_create\(CLOCK_MONOTONIC.*?registerHardIsaIrqHandler\(.*?1, this, IrqPolicy::syntheticHard\(\)\).*?timer_settime\(' \
            "$hosted_scheduler_timer_source" ||
        ! rg -q -U \
            '(?s)HostedSchedulerTimer::uninitialise\(\).*?timer_settime\(.*?unregisterHandler\(m_IrqId, this\).*?timer_delete\(' \
            "$hosted_scheduler_timer_source" ||
        ! rg -q -U \
            '(?s)HostedSchedulerTimer::irq\(.*?getInterruptNumber\(\) != SIGUSR2.*?si_signo != SIGUSR2.*?si_code != SI_TIMER.*?si_overrun.*?m_Handler->timer\(delta, state\)' \
            "$hosted_scheduler_timer_source" ||
        ! rg -q 'hosted-scheduler-timer-hard-context' \
            "$scheduler_regressions" ||
        ! rg -q 'hosted-timer-thread-context' "$timer_regressions" ||
        ! rg -q 'hosted-timer-overrun-accounting' "$timer_regressions" ||
        ! rg -q 'timer-alarm-send-linearization' "$timer_regressions" ||
        ! rg -q 'alarmLockHeldForTest' "$timer_regressions" ||
        ! rg -q 'dispatchHandlerForTest' "$timer_regressions" ||
        ! rg -q 'TimerHandlerRegistry registry' "$timer_regressions"; then
        echo "Hosted hard scheduler and ordinary timer context coverage is incomplete."
        failed=1
    fi

    if ! rg -q -U \
        '(?s)static void threadWrapper\(.*?\(void\) bInterrupts;.*?Processor::setInterrupts\(true\)' \
        "$hosted_processor_source"; then
        echo "A new hosted kernel thread can inherit an atomic creator context."
        failed=1
    fi

    local rtc_source=src/system/kernel/machine/mach_pc/Rtc.cc
    local rtc_header=src/system/kernel/machine/mach_pc/Rtc.h
    local irq_event_counter=src/system/include/pedigree/kernel/machine/IrqEventCounter.h
    local pc_source=src/system/kernel/machine/mach_pc/Pc.cc
    local rtc_hard
    rtc_hard=$(sed -n '/Rtc::hardIrq(/,/^void Rtc::threadedIrq/p' "$rtc_source")
    if ! rg -q 'class Rtc : public Timer, private SplitIrqHandler' \
            "$rtc_header" ||
        ! rg -q -U \
            '(?s)Rtc::hardIrq\(.*?acknowledgeInterruptFromHardIrq\(\).*?recordFromInterrupt\(\).*?work = RtcPeriodicWork.*?HardIrqDisposition::Deferred' \
            "$rtc_source" ||
        ! rg -q -U \
            '(?s)Rtc::acknowledgeInterruptFromHardIrq\(\).*?assert\(!Processor::getInterrupts\(\)\).*?LockGuard<Spinlock> guard\(m_CmosLock\).*?return readLocked\(0x0C\)' \
            "$rtc_source" ||
        rg -q \
            'sendEvent|m_AlarmQueue|m_HandlerRegistry\.dispatch|while[[:space:]]*\(|compareAndSwap' \
            <<<"$rtc_hard" ||
        ! rg -q -U \
            '(?s)Rtc::threadedIrq\(.*?m_PendingTicks\.takeAll\(\).*?processPeriodicTick\(delta\)' \
            "$rtc_source" ||
        ! rg -q -U \
            '(?s)Rtc::quiesceIrqSources\(\).*?setPeriodicInterruptEnabled\(false\)' \
            "$rtc_source"; then
        echo "RTC work escaped its acknowledge/count top half and threaded bottom half."
        failed=1
    fi

    local ps2_source=src/system/kernel/machine/mach_pc/Ps2Controller.cc
    local ps2_header=src/system/kernel/machine/mach_pc/Ps2Controller.h
    local ps2_regressions=src/modules/system/hosted-smoke/ps2-controller-regressions.cc
    local ps2_mouse_source=src/modules/drivers/x86/ps2mouse/Ps2Mouse.cc
    local ps2_hard ps2_config ps2_gate_tries
    ps2_hard=$(sed -n \
        '/Ps2Controller::hardIrq(/,/^bool Ps2Controller::captureOneLocked/p' \
        "$ps2_source")
    ps2_config=$(sed -n \
        '/bool Ps2Controller::configureIrqEnable(/,/^uint8_t Ps2Controller::readByte/p' \
        "$ps2_source")
    ps2_gate_tries=$(printf '%s\n' "$ps2_hard" | awk '
        index($0, "m_IoGate.tryAcquire()") { ++count }
        END { print count + 0 }
    ')
    if ! rg -q \
            'class Ps2Controller : public Controller, private SplitIrqHandler' \
            "$ps2_header" ||
        ! rg -q \
            '__atomic_always_lock_free\(sizeof\(size_t\), nullptr\)' \
            src/system/include/pedigree/kernel/machine/Ps2CaptureState.h ||
        [[ "$ps2_gate_tries" != 1 ]] ||
        ! rg -q -U \
            '(?s)Ps2Controller::hardIrq\(.*?if \(!m_IoGate\.tryAcquire\(\)\).*?work = RecoveryWork;.*?return HardIrqDisposition::Deferred;.*?m_pBase->read8\(4\).*?canPushFromInterrupt\(\).*?work = RecoveryWork;.*?return HardIrqDisposition::Deferred;.*?m_pBase->read8\(0\).*?status & SecondPortData.*?pushFromInterrupt\(.*?work = CapturedWork;.*?return HardIrqDisposition::Deferred;' \
            <<<"$ps2_hard"; then
        echo "The PS/2 hard stage lost one-shot admission, capacity preflight, or status-based routing."
        failed=1
    fi

    matches=$(rg -n \
        'm_(First|Second)PortBuffer|Scheduler::|yield\(|FATAL|ERROR|WARNING|NOTICE|TRACE|waitFor|while[[:space:]]*\(|for[[:space:]]*\(|new[[:space:]]|delete[[:space:]]|LockGuard|acquireIoForThread|\.read\(|\.write\(' \
        <<<"$ps2_hard" || true)
    if [[ -n "$matches" ]]; then
        echo "The PS/2 hard stage contains a blocking or unbounded operation:"
        echo "$matches"
        failed=1
    fi

    if rg -q 'sendCommandWithResponseLocked|0x20' <<<"$ps2_config" ||
        ! rg -q -U \
            '(?s)m_ConfigByte \|= flagAdd;.*?m_ConfigByte &= flagRemove;.*?sendCommandLocked\(0x60, m_ConfigByte\)' \
            <<<"$ps2_config" ||
        ! rg -q -U \
            '(?s)sendCommandWithResponse\(0xAA\).*?sendCommand\(0x60, m_ConfigByte\).*?sendCommand\(0xAE\).*?sendCommand\(0xA8\)' \
            "$ps2_source"; then
        echo "PS/2 IRQ configuration stopped using its cached 8042 config byte."
        failed=1
    fi

    if ! rg -q -U \
            '(?s)Ps2Controller::readByte\(\).*?m_DebugState\.value\(\).*?return readByteNonBlock\(\);.*?acquireIoForThread\(\)' \
            "$ps2_source" ||
        ! rg -q -U \
            '(?s)Ps2Controller::readFirstPort\(.*?m_DebugState\.value\(\).*?readByteNonBlock\(\).*?return byte != 0;' \
            "$ps2_source" ||
        ! rg -q -U \
            '(?s)Ps2Controller::readSecondPort\(.*?m_DebugState\.value\(\).*?readByteNonBlock\(\).*?return byte != 0;' \
            "$ps2_source"; then
        echo "PS/2 debugger input can block or invent a successful zero byte."
        failed=1
    fi

    if ! rg -q -U \
            '(?s)Ps2Controller::uninitialise\(\).*?shutdownSplitIrq\(\).*?m_ReadMode = StoppingReadMode;.*?m_FirstPortBuffer\.disableWrites\(\).*?m_SecondPortBuffer\.disableWrites\(\)' \
            "$ps2_source" ||
        ! rg -q -U \
            '(?s)Ps2Controller::readFirstPort\(.*?m_DebugState\.value\(\).*?readMode == StoppingReadMode.*?readMode == PollingReadMode.*?m_FirstPortBuffer\.read' \
            "$ps2_source" ||
        ! rg -q -U \
            '(?s)Ps2Mouse::initialise\(.*?setIrqEnable\(true, true\).*?writeSecondPort\(SetDefaults\).*?readSecondPort\(result\).*?writeSecondPort\(MouseStream\).*?readSecondPort\(result\).*?m_ReaderThread\.adopt' \
            "$ps2_mouse_source" ||
        ! rg -q -U \
            '(?s)Ps2Mouse::readerThread\(\).*?!m_pController->readSecondPort\(byte\).*?m_pController->readsStopping\(\).*?getUnwindState\(\) != Thread::Continue.*?return;' \
            "$ps2_mouse_source" ||
        rg -q 'readerThread(Trampoline)?\([^;]*\)[[:space:]]*NORETURN' \
            src/modules/drivers/x86/ps2mouse/Ps2Mouse.h ||
        ! rg -q -U \
            '(?s)Pc::initialise3\(\).*?Rtc::instance\(\)\.initialise3\(\).*?m_Ps2Controller->initialise3\(\).*?m_Keyboard->startReaderThread\(\)' \
            "$pc_source" ||
        ! rg -q -U \
            '(?s)Pc::deinitialise\(\).*?Rtc::instance\(\)\.uninitialise\(\).*?m_Ps2Controller->uninitialise\(\).*?m_Keyboard->stopReaderThread\(\).*?Pit::instance\(\)\.uninitialise\(\).*?Pic::instance\(\)\.shutdownThreaded\(\)' \
            "$pc_source"; then
        echo "PS/2 buffered-reader startup or teardown ordering regressed."
        failed=1
    fi

    if ! rg -q 'ps2-one-shot-hard-admission' "$ps2_regressions" ||
        ! rg -q 'ps2-capture-queue-fidelity' "$ps2_regressions" ||
        ! rg -q 'canPushFromInterrupt\(\)' "$ps2_regressions"; then
        echo "Hosted PS/2 admission and capture-queue coverage is incomplete."
        failed=1
    fi

    if ! rg -q 'class IrqEventCounter' "$irq_event_counter" ||
        ! rg -q 'recordFromInterrupt\(size_t occurrences = 1\)' \
            "$irq_event_counter" ||
        ! rg -q 'occurrences > \(Maximum - count\)' \
            "$irq_event_counter" ||
        ! rg -q 'm_Count \+= occurrences' "$irq_event_counter" ||
        ! rg -q 'takeAll\(\)' "$irq_event_counter" ||
        ! rg -q 'irq-event-counter-bounded-arithmetic' \
            "$split_irq_regressions" ||
        ! rg -q 'recordFromInterrupt\(m_NextEvents\.value\(\)\)' \
            "$split_irq_regressions" ||
        ! rg -q 'observedEvents == 15' "$split_irq_regressions" ||
        ! rg -q 'observedEventTime == 14648437' "$split_irq_regressions"; then
        echo "Counted split IRQ occurrences can again collapse into a work bit."
        failed=1
    fi

    if ! rg -q -U \
            '(?s)Pc::initialise3\(\).*?Rtc::instance\(\)\.initialise3\(\).*?startReaderThread\(\)' \
            "$pc_source" ||
        ! rg -q -U \
            '(?s)Pc::deinitialise\(\).*?Rtc::instance\(\)\.uninitialise\(\).*?Pic::instance\(\)\.shutdownThreaded\(\)' \
            "$pc_source" ||
        ! rg -q 'statusB & ~RtcUpdateInhibit' "$rtc_source" ||
        ! rg -q 'statusB \| RtcUpdateInhibit' "$rtc_source" ||
        ! rg -q 'status & ~RtcInterruptEnableMask' "$rtc_source"; then
        echo "RTC worker lifecycle or CMOS control-bit preservation regressed."
        failed=1
    fi

    matches=$(rg -n \
        'Scheduler::instance\(\)\.yield\(\)' \
        "$timer_registry_source" || true)
    if [[ -n "$matches" ]]; then
        echo "The timer callback drain reverted to scheduler yielding:"
        echo "$matches"
        failed=1
    fi

    if ! rg -q 'class EXPORTED_PUBLIC TimerHandlerRegistry' \
            "$timer_registry_header" ||
        ! rg -q 'WaitQueue m_DispatchWaiters' "$timer_registry_header" ||
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
