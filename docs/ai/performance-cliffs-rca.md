# Performance cliffs: cross-workload root-cause analysis

Initial investigation: 2026-09-27; background-work, scheduler and IRQ-latency follow-ups: 2026-09-28. Target: amd64 Pedigree under QEMU/TCG on the same macOS ARM64 host as the Linux controls.

## Finding

A previously unexplained storage cliff is now traced to **false quarantine of AHCI's MSI completion interrupt**. The controller initially works, but legitimate empty threaded callbacks reach the PCI interrupt layer's unhandled threshold. MSI is then disabled. Disk reads continue through AHCI's 10 ms fallback wait, so the system remains functional while disk-dependent work becomes dramatically slower and the CPU spends much of the interval idle.

The live guest had vector 0x40 disabled, `unhandled = 8`, and only 55 AHCI interrupt completions while thousands of reads continued. Two fresh one-vCPU runs took 104.05 and 105.28 seconds from guest start through boot and RAM-root preparation to first-phase readiness, compared with about 15.3 seconds on four vCPUs. A final baseline with all preparation profiling disabled repeated the interval at 104.57 seconds. Both topologies performed the same 8,828 reads. The extra roughly 89 seconds is consistent with approximately one 10 ms fallback interval per read.

The four-line correction reduced that same single-vCPU interval to **12.81 and 12.50 seconds**, approximately **8.3 times faster**, while retaining the same read-request count. Four-vCPU readiness remained about 15.4 seconds. A second live snapshot confirmed MSI remained enabled with zero unhandled callbacks counted and 8,628 interrupt completions. The initial diagnostic patch changed only the AHCI declaration; the later scheduler remediation is recorded separately below.

This is a concrete cause of a large cliff, not evidence that every earlier symptom had the same cause. Fresh disk-free GCC measurements do not reproduce the large wall-minus-user/system discrepancy. Their remaining Linux gap is mainly reported execution time. Retained network controls separately demonstrate packet-loss timeouts and costly API/worker handoffs, including substantial degradation when the same lwIP sources run inside Linux. No single continuously active global time sink was established across those workloads.

## Evidence boundaries

The current-source image was rebuilt from `09d63df5ae762b02de6a9a6ed973d8e5b3fc37a2` plus the existing pending changes. Those changes include the network queue, IOAPIC, console and shutdown work. This is not a clean-commit benchmark. The exact patch, new source files, CMake cache, kernel, uncompressed initrd and hashes are retained in the evidence bundle.

Fresh work in this investigation comprises one- and four-vCPU GCC controls, host profiles, PCI-topology inspection, an accounting ablation, live AHCI state inspection, and a targeted correction with repeated guest validation. Network conclusions below are reanalysis of retained experiments and current code, not newly executed network or physical-NIC measurements. Earlier T420 observations are separate user-reported hardware evidence.

All timed GCC phases used the same GCC 15.3/musl executables and source, copied into RAM by the same fixture. QMP block counters show zero disk reads and writes during these phases. QEMU is 11.1.1, q35, TCG with multiple host threads, 4 GiB, `SandyBridge,-rdrand,-rdseed`. Linux uses the retained 6.18.36 control with mitigations and transparent huge pages disabled. Those settings make this a controlled performance comparison, not a deployment recommendation.

Binary comparison between baseline and corrected images found identical contents in every allocated, file-backed kernel ELF section; `ahci.o` is the only changed initrd member. The root filesystem and original CMake configuration are unchanged. Full-file kernel hashes differ in nonloaded metadata; the executable kernel payload is identical.

The Pedigree baseline uses `-O3`, precise accounting, disabled lock tracking, disabled spinlock diagnostics, and disabled PCAP. Assertions/debugger and additional checks remain enabled. Every arm uses a fresh writable overlay; frozen backing images are never rebuilt while in use. Builds and measured guests run sequentially.

## AHCI root cause

### Reproduction and mechanism

The existing compile fixture copies its tools and headers from the AHCI disk into RAM before the first timed phase. Watching only the compiler's reported time misses that preparation interval. QMP records exactly 114,104,320 bytes in 8,828 read operations before the first phase in each baseline run. These counters include boot as well as fixture copying. The interval begins immediately before QMP `cont`, not before QEMU process creation. The slow single-vCPU runs accumulate only 0.59–0.62 seconds of QEMU block read-service time; the backend itself does not account for the 104–105 seconds of elapsed time.

A host sample during the slow copy found about 80% of vCPU samples in QEMU's guest-idle wait. A stopped-guest stack then showed `ProcessorBase::haltUntilInterrupt()` beneath the scheduler's idle path. Together with the interrupt state and fallback code, these observations support delayed notification of completed device work. The idle stack alone does not establish hardware completion at that exact instant.

The disabled MSI line and threshold are directly observed. The source explains how legitimate polling and late callbacks can produce that state; the precise sequence of the first eight empty callbacks was not traced:

1. `AhciController::initialiseController()` registers a threaded PCI message interrupt. Its startup probe requires a real interrupt completion; initial delivery succeeds.
2. `AhciPort::reapCommand()` also polls completion state. A poll can consume and acknowledge the hardware status before an already-posted MSI callback runs. The callback then has no pending work to claim. The port keeps one polling credit, which need not account for multiple late messages.
3. `PciMessageInterrupts::dispatchThreaded()` treats an empty callback from a handler without the spurious-safe declaration as unhandled. At eight it calls `quarantine()`, which disables MSI. Successful work whose cookie has already been superseded does not reset this counter, so the threshold does not imply eight successive failed disk requests.
4. AHCI continues to believe interrupts are available. Subsequent commands commonly finish in hardware after the initial poll, but their completion semaphore receives no interrupt notification. `reapCommand()` wakes at its 10,000 microsecond fallback interval and discovers completion then.

The debugger directly confirmed `mode = Threaded`, `spuriousSafe = false`, `enabled = false`, `removing = false`, `msix = false`, `unhandled = 8`, and cookie 65. A C++ downcast of the registered handler identified AHCI, with IRQ 0x40 and the same PCI device pointer. Port 0 remained online with one outstanding command and `m_InterruptCompletions = 55`. These are captured in `io-debug-1c/irq-state.txt` and `ahci-state.txt`.

The unmodified single-vCPU baseline also showed thousands of IRQ10/GSI16 assertions, absent in the four-vCPU runs. AHCI clears PCI `INTxDisable` after message registration, so disabling MSI exposes legacy assertions without registering a new INTx handler. This supports the observed transition; changing that bit alone would not restore completion delivery.

### Correction and causal validation

`AhciController::acceptsSpuriousInterrupts()` now returns true. This uses the existing handler contract already used by NVMe. AHCI reads its own pending status and does not complete requests or acknowledge nonexistent port work during an empty callback. The declaration prevents legitimate empty message callbacks from being mistaken for a broken interrupt source. Registry admission failures, teardown checks, and the actual-interrupt startup probe retain their existing behavior. INTx policy is unchanged.

| Run | Boot + preparation | Disk reads | QEMU read-service sum | Benchmark |
| --- | ---: | ---: | ---: | --- |
| Baseline, 1 vCPU, A | 104.050 s | 8,828 | 0.618 s | PASS |
| Baseline, 1 vCPU, B | 105.284 s | 8,828 | 0.594 s | PASS |
| Baseline, 1 vCPU, no profiling | 104.570 s | 8,828 | 0.605 s | PASS |
| Corrected, 1 vCPU, A | 12.812 s | 8,828 | 0.125 s | PASS |
| Corrected, 1 vCPU, B | 12.500 s | 8,828 | 0.124 s | PASS |
| Baseline, 4 vCPUs, A | 15.291 s | 8,828 | 0.136 s | PASS |
| Baseline, 4 vCPUs, B | 15.339 s | 8,828 | 0.138 s | PASS |
| Corrected, 4 vCPUs | 15.401 s | 8,828 | 0.137 s | PASS |

The corrected initrd adds 64 KiB to cumulative boot reads (114,169,856 bytes versus 114,104,320); it does not reduce the read workload. The fully unprofiled baseline took 104.570 seconds versus a corrected mean of 12.656 seconds, a reduction of 91.914 seconds. The earlier baseline repetitions agree closely. Timing runs had no debugger attached. In a separate diagnostic after preparation, MSI vector 0x40 had `enabled = true`, `spuriousSafe = true`, `unhandled = 0`, cookie 5,965, and 8,628 interrupt completions. The disk remained online. The corrected runs no longer showed the thousands of IRQ10/GSI16 assertions.

Both corrected single-vCPU runs and the four-vCPU run completed the cold/warm compile, resulting-program execution, checksum, idle, and existing anonymous-memory contract phases successfully. Warm compilation itself remained 15.915/15.358 seconds on one vCPU and 16.398 seconds on four: removing the storage cliff does not materially accelerate a phase already running entirely from RAM.

The 10 ms fallback is retained as a recovery mechanism. Shortening it would reduce the symptom while leaving the lost-interrupt defect intact. The patch does not change the shared interrupt-generation or lifetime protocol.

Relevant source: `src/modules/drivers/common/ahci/AhciController.h`, `AhciController.cc`, `AhciPort.cc` (`pollCompletions`, `interrupt`, `reapCommand`), and `src/system/kernel/machine/mach_pc/PciMessageInterrupts.cc` (`dispatchThreaded`, `quarantine`).

### Impact and detection gap

The demonstrated impact is on uncached AHCI reads: executable loading, header loading, filesystem traversal and other disk-dependent work can all inherit the delay. The evidence does not extend that impact to cached/RAM-only execution, memory-only network transfers, other storage controllers or physical hardware.

Three properties hide the defect from ordinary functional checks. The startup interrupt probe succeeds before later quarantine; the fallback preserves correct I/O; and `quarantine()` only logs if masking fails, so successfully disabling the source is silent. The existing AHCI smoke's cumulative nonzero-completion check can also pass if enough interrupts arrive early. A passing read or successful boot therefore does not establish sustained interrupt delivery.

For this regression, the existing compile runner's first-phase `ready_host_s`, QMP block totals and interrupt-state inspection provided a high-signal check without new production hooks or a new permanent harness. Preserve this boot/preparation measurement when comparing future storage changes. A production diagnostic for successful quarantine would make future incidents easier to recognize; that broader observability change is deferred.

## Baseline GCC controls: RAM-only timed phases

| Guest / run | Warm host elapsed | Warm guest elapsed | User | System | Guest remainder |
| --- | ---: | ---: | ---: | ---: | ---: |
| Pedigree, 1 vCPU | 15.331 s | 15.316 s | 11.987 s | 3.114 s | 0.215 s |
| Linux, 1 vCPU | 12.781 s | 12.779 s | 11.050 s | 1.467 s | 0.262 s |
| Pedigree, 4 vCPUs, A | 15.967 s | 15.954 s | 12.238 s | 3.572 s | 0.143 s |
| Pedigree, 4 vCPUs, B | 16.067 s | 16.054 s | 12.336 s | 3.580 s | 0.138 s |
| Linux, 4 vCPUs | 14.101 s | 14.099 s | 12.211 s | 1.604 s | 0.283 s |

Cold compiler phases also passed. The one-vCPU warm comparison is 15.331 versus 12.781 seconds host elapsed; the four-vCPU comparison is about 16.0 versus 14.101 seconds. Pedigree's warm accounting remainder is 0.14–0.21 seconds, smaller than the Linux controls' 0.26–0.28 seconds. These measurements therefore do not show the suspected large unaccounted interval during RAM-only compilation. CPU-only guest elapsed was approximately 0.197–0.202 seconds, with matching checksums, and five-second idle controls tracked host time closely.

CPU-only and five-second idle controls test ordinary execution speed and clock scale. Timed phases in these controls do not run the QEMU instruction plugin or host sampler. The diagnostic profiles below are separate runs and are not used to claim small speedups.

The remainder is guest elapsed minus the compiler child tree's user and system time. It includes scheduling, unrelated/background work, parent-side launch/reap work and measurement boundaries. It is not a measurement of wasted CPU. Kernel execution charged to the compiler is already included in system time.

Pedigree currently fills CPU time but not fault, block-I/O or context-switch fields in `getrusage`; their printed zero values cannot establish absence of faults, I/O or scheduling. Disk exclusion here comes from QMP counters, not those zero fields. See `src/modules/subsys/posix/system-syscalls.cc:964`.

## Host execution profiles

Five-second macOS `sample` captures began at the warm-compile ACK. Call-tree counts were converted to exclusive residual weights, checked against each vCPU's total, then grouped. These categories are disjoint; helper time nested beneath translation-block lookup appears only in that group.

| Sampled active vCPU | Pedigree, 1 vCPU | Linux, 1 vCPU | Pedigree, 4 vCPUs |
| --- | ---: | ---: | ---: |
| Samples | 3,854 | 3,834 | 3,849 |
| Translated guest code, unresolved host PC | 57.81% | 55.89% | 56.92% |
| Translation-block lookup, including nested helpers | 33.01% | 35.05% | 34.03% |
| Software MMU outside lookup | 2.36% | 1.72% | 3.22% |
| JIT write protection | 2.88% | 0.81% | 2.55% |
| Host mutex/BQL acquisition | 1.53% | 4.62% | 0.34% |
| Code generation outside those groups | 0.08% | 0.13% | 0.16% |
| Other execution | 2.34% | 1.77% | 2.78% |
| Idle waits | 0% | 0% | 0% |

Pedigree's other three vCPUs were 99.30%, 98.93% and 99.43% idle in the four-vCPU capture. This does not show an idle-spin storm. Actual blocked mutex waits were 1.38% in Pedigree versus 4.62% in Linux's single-vCPU capture. Translation generation was negligible: no recompilation storm is visible in these windows.

There is a modest JIT-protection difference, but these short snapshots do not establish its whole-workload cost or cause. They do establish that the large translation-block lookup cost is common to both guests. It would be incorrect to attribute all of it to Pedigree.

Instruction counts cannot substitute for this measurement: QEMU uses lookup and chaining to dispatch translated blocks, must revisit interrupt handling after state changes that unmask interrupts, and calls device models for MMIO. These operations have different host costs. See the [QEMU translator documentation](https://www.qemu.org/docs/master/devel/tcg.html).

The unresolved host PCs cover translated guest user and kernel code; macOS `sample` does not split them. These profiles therefore reject a large observed host-wait anomaly but do not fully identify remaining guest hotspots. The first Linux sampling attempt hit the `uv` launcher rather than QEMU; it was excluded and repeated with a direct exec wrapper. The valid capture identifies QEMU PID 5802.

## Precise-accounting ablation

Four-vCPU warm host times with precise accounting enabled were **15.967 and 16.067 seconds**. With `PEDIGREE_TIME_ACCOUNTING=FALSE`, they were **16.451 and 15.726 seconds**. The run order was enabled, disabled, disabled, enabled. The means are 16.017 and 16.088 seconds respectively; there is no consistent improvement from disabling accounting. Cold results likewise overlap (enabled 17.257/17.170; disabled 17.911/17.132 seconds).

This tests the aggregate cost of per-boundary clock samples, publication and shared global counters. It does not test a hypothetical removal of all synchronization, nor does it justify disabling accounting in production. The original build configuration was restored after freezing the variant.

## Follow-up: background workers and runnable latency (2026-09-28)

The follow-up question is distinct from storage completion loss: can unrelated kernel work occupy a CPU, or delay preemption, while an application remains ready? Yes, the architecture permits both. The measurements below quantify their effect in this fixture rather than inferring their absence from successful I/O or a small compiler accounting remainder.

### CPU ownership across idle and compilation

Read-only stopped-guest snapshots enumerated every process and thread and read their existing CPU-time counters before each benchmark phase. AP kernel processes were included, not just PID 0. Idle threads were identified from their scheduler's idle-thread pointer and excluded from worker totals. No counters were reset and no guest functions were called. The final runs stop QEMU before starting LLDB, keeping debugger startup outside the measured phase. Counter intervals include the small inter-phase gate/launch boundaries and any open accounting interval; these are approximate attribution, not a cycle-exact decomposition of application time.

| Configuration / phase | Guest elapsed | Kernel worker CPU, excluding idle | Application elapsed minus user+system |
| --- | ---: | ---: | ---: |
| 1 CPU, idle | 5.004 s | 209 ms | 5.002 s |
| 1 CPU, cold compile | 17.265 s | 246 ms | 262 ms |
| 1 CPU, warm compile | 16.095 s | 241 ms | 246 ms |
| 4 CPUs, idle | 5.007 s | 415 ms | 5.003 s |
| 4 CPUs, cold compile | 18.238 s | 512 ms | 212 ms |
| 4 CPUs, warm compile | 16.599 s | 430 ms | 135 ms |

On one CPU, background workers account for approximately 1.5% of warm-compile elapsed time and explain most of its accounting remainder. An earlier independent snapshot run found 221 ms of workers and a 225 ms remainder. On four CPUs, worker times are summed across CPUs and can overlap application execution; they must not be added to, or equated with, the application's wall-time gap.

The final one-CPU warm interval attributed 108.6 ms to CacheManager trim, 58.5 ms to the generic `runConcurrentlyAttached` worker (attributed to lwIP from the enabled source paths, not a captured stack), 49.7 ms to the clock deadline worker, 9.5 ms to eight TextIO flip threads, and about 14.5 ms to remaining workers. This is observable periodic work, not an always-running kernel thread. The source inventory found blocking waits between normal work batches. Cache maintenance runs every 500 ms; ordinary SCSI/ext2 caches use explicit dirty tracking, so these numbers do not imply repeated checksums of every clean cached page. lwIP maintains timers even without a physical NIC, including a 100 ms AUTOIP timer. Text consoles wake on their blink cadence.

A separate three-second host sample during idle placed 2,098 of 2,263 vCPU samples in the guest-halt condition wait (92.7%). This guards against a large hard-IRQ workload hidden by idle accounting, though it is a short diagnostic sample, not a precise utilization estimate.

### Direct latency observations

Each running loop lasts approximately ten seconds and uses direct timestamp reads to detect gaps while the application stays runnable. Each wake test makes 2,000 absolute monotonic-clock waits for deadlines 1 ms ahead. Wake figures are **lateness beyond the deadline**, not total sleep duration. No debugger or host sampler runs during these probes.

| Guest | Longest running-loop gap | Wake lateness p50 | Wake lateness p99 | Maximum wake lateness |
| --- | ---: | ---: | ---: | ---: |
| Pedigree, 1 CPU | 4.201 ms | 1.060 ms | 3.283 ms | 13.924 ms |
| Linux, 1 CPU | 1.498 ms | 0.534 ms | 1.602 ms | 7.491 ms |
| Pedigree, 4 CPUs | 1.371 ms | 1.614 ms | 3.588 ms | 6.285 ms |

None of the three running loops observed a gap of 5 ms or longer. There were 21, 2 and 21 gaps of at least 1 ms in the table's respective runs. The Pedigree one-CPU timestamp loop reported 9.848 seconds user and 0.037 seconds system in 10.000 seconds elapsed, confirming that it predominantly executes in userspace. TSC calibration before and across each loop agreed within 0.1%.

These are observations of this QEMU/TCG configuration, not hard latency bounds. The timestamp loop includes guest interrupts, host scheduling and emulator costs; it does not assign individual gaps to a particular worker. The wake test additionally includes timer expiry and wake/scheduling machinery. The probe runs alone, not concurrently with a compiler or a saturated network/storage workload.

A first probe repeatedly called `clock_gettime`; it spent most of its CPU time in Pedigree system calls while Linux used its fast clock path. That probe was therefore repeated with direct x86 timestamp reads and only occasional monotonic-clock checks. The reported running-app results use this latter variant, with measured clock calibration and a 128-bit conversion to avoid overflow on a long gap. Both probes are temporary variants of the existing benchmark's control phases, outside tracked source; no production testing hook was added.

### Constraints identified before scheduler remediation

- Normal kernel workers and applications use the same default priority. IRQ workers are pinned, and ordinary thread placement has no automatic work stealing. Spare CPUs therefore do not guarantee that an application can escape a busy worker on its assigned CPU.
- Spinlock acquisition disables local interrupts before contention is resolved. Time waiting for a contended spinlock, as well as time holding it, can defer the timer and preemption. Per-thread totals cannot distinguish many short critical sections from one long section.
- Request cleanup, firmware work, network traffic and device-specific polling can change the background workload. UHCI polls root ports every 1 ms and virtio-GPU refreshes every 50 ms; neither device exists in this fixture. These are concrete candidates for other configurations, not causes established here.

There are also diagnostic blind spots. `/proc/stat`'s runnable count and the loadavg runnable fraction omit kernel workers, while IRQ/softirq/iowait/steal fields are literal zeros (`ProcFs-status.cc`). Hard-IRQ execution interrupting the idle thread is classified as idle by `Scheduler::recordCpuTime`; execution interrupting an app can appear in its system time. Neither procfs nor application accounting alone can rule out the proposed mechanism. The scheduler's ready-queue selection timing measures selection execution, not time an application spent waiting ready; no cumulative runnable-wait counter currently exists.

The supported conclusion is narrower than a system-wide clean bill of health: **normal background activity measurably takes CPU time, and the direct probes observe millisecond latency without attributing individual gaps to particular workers. This fixture does not reproduce a persistent large CPU-stealing worker or repeated long application stalls.** The remaining useful target is event-specific latency under the device/workload that exhibits a stall, with thread identity and IRQ-disabled state captured during that event. The measurements alone do not establish that a scheduling-policy change will improve GCC throughput. The subsequent requested remediation addresses the concrete observability, contention and placement limitations separately.

Evidence is in `/private/tmp/pedigree-background-rca`: `workers-1c/` and `workers-4/` hold final thread snapshots and deltas; `workers-1b/` holds the initial independent capture and idle host sample; `tsc/latency-results.json` holds the matched direct-timestamp/wakeup controls. All use the previously validated AHCI-fixed image, with current source/build identity verified. Large images remain temporary; the compact evidence archive excludes them. No additional kernel changes were made during that measurement pass.


## Scheduler and observability remediation

The subsequent requested changes add three capabilities:

- `cat /proc/kernel/threads` reports internal process/task IDs, logical CPU, state, priority, user/system nanoseconds, idle identity and worker name. Process and thread leases protect enumeration. `/proc/stat` runnable counts and `/proc/loadavg` task counts now include non-idle kernel workers. This is a live view rather than an atomic per-open snapshot. Kernel records use a separate path because internal task IDs and POSIX PIDs currently occupy different namespaces; stock `ps`/`top` integration is not added.
- Contended spinlocks restore interrupts while read-spinning only for safe, IRQ-enabled, waitable-thread entry. IRQs are masked again before the ownership CAS and owner-state publication. IRQ-disabled, atomic, nested and raw-IRQ contexts retain their constraints, including scheduler off-stack release. This permits interrupts and normal preemption during eligible waits; it neither sleeps nor eliminates contention CPU cost. `hlt` is unsuitable here because unlock has no guaranteed interrupt wakeup. Lock tracking defers first-lock attempt registration until ownership so an unowned waiter leaves no per-CPU tracking behind while preemptible.
- Priority selection now considers the runnable current thread as well as queued peers. Previously a lower-priority queued worker could displace the current application regardless of their relative priorities. Equal-priority rotation remains FIFO; priorities skipped eight selections receive aged service so continuous application work cannot starve maintenance. Cache trimming and TextIO flipping start at maintenance priority 2, below ordinary priority 1. IRQ, timer and dependency workers retain their priorities. The bound is scheduling decisions, not a real-time latency guarantee.

Idle CPUs advertise a reservable destination. At a clean root user-return boundary, an eligible Linux-ABI application with queued work on its current CPU can reserve an allowed idle CPU. The existing affinity worker transfers the saved continuation off-stack and publishes its wake on the destination. This leaves affinity masks unchanged and retains pinned kernel placement. It deliberately cannot move arbitrary saved syscall or native callback continuations that still hold CPU-local references. NUMA policy is unchanged.

Validation also found an existing explicit-affinity defect: the source worker acknowledged a new mask and cleared the return-gate flag before physical movement. A task could report a singleton CPU-0 mask while executing on CPU 2. The worker now retains the gate flag until the actual owner satisfies the acknowledged mask. The existing affinity API test reproduced this failure before that correction and passes afterward.

The final fresh x64 image passed kernel-thread procfs inspection plus scheduling policy, placement, wakeup and affinity-race families on one and four CPUs. The new placement regression starts three busy peers on one CPU, broadens two masks, and requires them to spread across available CPUs while the singleton peer stays pinned; live register, TLS and errno checks remain enabled. The race family exercises concurrent setters and target exit. It was separated from the existing exec/lifecycle family so unrelated exec identity failures do not hide those checks. The Darwin hosted lifecycle passed, including priority/current-thread selection, bounded maintenance service and spinlock interrupt/context restoration. A lock-tracking-enabled hosted run also passed; hosted execution alone does not exercise actual SMP contention.

Two broader existing contracts remain failing: exec expects `gettid() == getpid()`, and the permission suite uses a process PID as a scheduling task ID. Both failures were reproduced with the same test payload on the original AHCI-fixed kernel, as well as the new one- and four-CPU guests. The namespace mismatch is not repaired by this scheduling patch, and a full scheduling-suite pass is therefore not claimed.

The stock compile runner was repeated sequentially against frozen pre-scheduler and final images, with the same RAM-root payload, QEMU/TCG topology, precise accounting and fresh writable overlays. All compile, execution and anonymous-memory markers passed. Warm compile times were:

| CPUs / repetition | AHCI-fixed baseline, seconds | Scheduler changes, seconds |
| --- | ---: | ---: |
| 1 / first | 15.716 | 16.517 |
| 1 / repeat | 15.583 | 16.484 |
| 4 | 16.913 | 16.702 |

**The single-CPU result is a repeatable 5–6% regression in these controls, with its cause unresolved.** Most of the increase is charged to user execution (12.23–12.28 seconds before, 12.90–12.94 afterward); the unaccounted remainder changes by only tens of milliseconds. A temporary ablation restoring the old ready-queue selector while retaining the other changes produced 16.777 seconds warm, so restoring that selector did not recover the loss. This ablation also rebuilds the kernel and cannot isolate binary-layout effects. No interrupt-rate storm or renewed storage fallback was found. The four-CPU comparison is close to baseline and has only one pair; it does not establish a throughput improvement. These results validate function while leaving a concrete performance concern for follow-up. Contended-spinlock latency improvement has not been quantified, and the functional redistribution test is not an end-to-end storage/network throughput benchmark.

Evidence is retained under `/private/tmp/pedigree-scheduler-fixes`: `contracts-final-1/`, `contracts-final-4/`, `contracts-baseline-1/`, hosted logs, exact build configuration and the existing compile-runner output. Test images use disposable QEMU snapshots; compile controls use fresh overlays. Physical hardware has not been exercised for these scheduler changes.


## IRQ masking and event-deferral measurements

The next pass adds opt-in x64 accounting at `/proc/kernel/latency`, enabled with `PEDIGREE_LATENCY_ACCOUNTING=ON` and precise thread accounting. The normal configuration remains OFF. The existing compile benchmark captures before/after rows when its `latency-stats` marker is present. The file is root-readable because it includes kernel origin addresses.

The IRQ gauge counts completed outermost masked intervals, their nanoseconds, a lifetime maximum and origin, and counts at least 1 ms / 10 ms long. It brackets both explicit masking and implicit hardware/syscall entry and return; nested disables do not multiply time. The idle STI/HLT interval is excluded. Early boot before clock calibration and small assembly transition tails are outside collection. RCU read guards already mask interrupts, so their non-preemptible time is included here.

Pedigree's `Uninterruptible` scopes defer event delivery but remain preemptible and may sleep. Their separate counters report CPU time, completed thread elapsed time, scope count and lifetime maximum. Termination-only worker-lifetime guards are excluded. IRQ-off and deferred CPU time overlap: adding them would double-count work. Sleeping or descheduled time contributes only to the thread-duration gauge, which can exceed total CPU capacity when many scopes overlap.

For each valid CPU row, the IRQ rate is `delta(irq_off_ns) / delta(sample_ns)`. Sum those rates for CPU-seconds per second, then divide by the CPU count to express a fraction of machine capacity. Open intervals create window-edge error; the file exposes their start and origin rather than pretending a remote snapshot is globally atomic. Absolute IRQ timestamps use each writer CPU's local clock and cannot be compared directly with the reader's global sample timestamp. A new lifetime maximum is identified by its value increasing between edges, not by comparing clock origins.

These measurements use the same RAM-root GCC fixture, fresh overlays and sequential QEMU/TCG runs as above. The table contains the final instrumented image's system totals:

| CPUs | Phase | IRQ-off CPU-s/s | Deferred CPU-s/s | IRQ intervals >=1 ms |
| --- | --- | ---: | ---: | ---: |
| 1 | Idle | 0.024894 | 0.000103 | 0 |
| 1 | Cold GCC | 0.132884 | 0.170378 | 0 |
| 1 | Warm GCC | 0.137439 | 0.177869 | 0 |
| 4 | Idle | 0.122491 | 0.000084 | 124 |
| 4 | Cold GCC | 0.172357 | 0.177968 | 0 |
| 4 | Warm GCC | 0.181467 | 0.187761 | 0 |

No completed interval in these final idle/cold/warm windows reached 10 ms. Both final warm windows had no open interval at either edge. Their 8,024,821 / 8,207,557 completed intervals averaged 318 / 431 ns. The four-CPU warm IRQ rate is 4.54% of machine capacity, not 18.1%.

SMP idle had 124 intervals at least 1 ms long. Two CPUs established new maxima of 4.632 and 4.069 ms, both starting at `PerProcessorScheduler::checkEventState`'s mask boundary. These are concrete latency tails to follow, but the start address does not identify how much time was execution, contention or host descheduling. The earlier repeat had 44 such idle intervals, so the tail count is variable.

Collection materially changes throughput:

| CPUs | Warm GCC, disabled | Warm GCC, enabled | Observed increase |
| --- | ---: | ---: | ---: |
| 1 | 15.868 s | 18.592 s | 17.2% |
| 4 | 17.119 s | 19.473 s | 13.7% |

These are one matched disabled/enabled pair per topology, not a precise decomposition of probe cost from run variation and code-layout effects. The earlier enabled warm measurements were 19.228 / 19.072 seconds. Rates describe the instrumented kernel and must not be presented as baseline IRQ duty. The observed slowdown is why collection remains opt-in; subtracting its wall-time cost from the IRQ counter would not recover an uninstrumented duty estimate.

The earlier corrected run also observed two IRQ-off intervals at least 1 ms long during the four-CPU cold compile. A new lifetime maximum of 4.196 ms began at `Spinlock::acquire`'s interrupt-disable boundary, symbolized against that run's frozen kernel. This identifies the start of a masked stretch; it does not prove the duration was spent waiting for the lock. Host vCPU descheduling can extend the same elapsed interval. Warm compilation did not reproduce a long blackout in that run. Boot-time maxima of 20.7 / 74.7 ms were already present before the measured workloads and must not be labeled compile stalls.

The earlier four-CPU warm run also established a new 125.9 ms completed event-deferral maximum. It coexisted with zero IRQ-off stretches at least 1 ms long in that phase. This is evidence of delayed event eligibility, not evidence that other runnable threads lost 125.9 ms of CPU service. Further attribution would need to distinguish the scope's active work from its sleeping/descheduled lifetime.

The initial counter output exposed an independent existing bug in the shared kernel formatter: its x86 inline division used a 32-bit divisor/instruction on x64. Nanosecond timestamps wrapped in printed output at 2^32, making five seconds look like about 0.7 seconds. Full-width unsigned division fixes both duration printing and kernel address output; the regression covers 5,000,000,000, UINT64_MAX, LONG_MIN and full-width pointers. This was a measurement defect, not the cause of the original performance cliff. Pre-fix counter dumps are excluded from the results.

The new counters keep NMI execution out of the IRQ-state writer and accept preceding CPU slices published at NMI entry through an independent atomic counter. Existing precise thread-accounting boundaries themselves are not fully NMI-reentrant; injected/recurring-NMI CPU accounting is not qualified by these runs. No NMI injection or physical-hardware test was performed.

Validation includes fresh enabled and disabled UEFI images, one-/four-CPU compile/execution/anonymous-memory contracts, 91 formatter/String tests, and seven existing compile-runner tests. The enabled kernel also passed procfs thread inspection plus scheduling policy, placement, wakeups and affinity-race families on one and four CPUs. The already documented lifecycle and permission failures remain separate baseline failures. Switching the option in both directions reassembles the architecture hooks; the disabled binary has no IRQ-accounting symbol or calls.

These gauges establish frequent short masking in this fixture; they do not establish a new common cause for the unexplained cross-workload slowdown. They include host vCPU descheduling and probe overhead, and neither quantify native-hardware masking nor rule out workload-specific long sections in networking or storage. The existing benchmark documentation describes capture, units, coverage and boundary error. Evidence is retained under `/private/tmp/pedigree-latency-accounting`, with frozen enabled/disabled payloads under `/private/tmp/pedigree-global-rca/latency-accounting-*`. Source/configuration snapshots, raw counters, derived rates, commands and logs accompany the measurements.

## Spinlock policy split comparison, 2026-09-28

The comparison isolates `9b89234f6c` immediately before the split from
`b4b1fb9656` after it. The earlier scheduler, storage and network fixes are present
in both. The split passes the tested runtime contracts but introduces a substantial
one-CPU performance regression. It does not reduce aggregate IRQ masking in this
GCC fixture.

Both versions use the same cross-compiler, guest feature settings and workload
files. Paired generated `config.h` files and GCC image bytes outside the ESP are
identical. The baseline worktree uses file-prefix mapping to normalize source
paths. Runs are sequential QEMU 11.1.1 TCG on the same arm64 macOS host, with fresh
writable overlays/snapshots, no concurrent builds and no guest profiling. Precise
accounting remains enabled; sampled accounting, built-in packet capture and lock
tracking are disabled. Latency probes are OFF for throughput and ON only for the
separate IRQ measurements. HDD writes remain crippled; this is not a persistence
test or a physical-hardware qualification.

GCC uses the existing RAM-root quick fixture, q35/SandyBridge and 4 GiB RAM. Two
boots per version and CPU count run in before/after/after/before order. Values
below are means of the two runs; with two observations these also equal medians.

| CPUs | Phase | Before guest wall | After guest wall | Change | Before / after host wall |
| --- | --- | ---: | ---: | ---: | ---: |
| 1 | Cold GCC | 17.205 s | 18.179 s | +5.7% | 17.220 / 18.193 s |
| 1 | Warm GCC | 16.058 s | 17.038 s | +6.1% | 16.072 / 17.054 s |
| 4 | Cold GCC | 18.873 s | 18.471 s | -2.1% | 18.889 / 18.488 s |
| 4 | Warm GCC | 17.120 s | 16.994 s | -0.7% | 17.136 / 17.007 s |

One-CPU warm ranges are 15.992–16.124 s before and 16.955–17.122 s after.
Warm system time rises 3.258 → 3.633 s (+11.5%); user time rises
12.529 → 13.108 s. The unaccounted remainder changes only 0.271 → 0.297 s.
This is predominantly additional accounted execution, not a new large wall/CPU
gap. Four-CPU warm ranges overlap (16.811–17.428 / 16.848–17.141 s), so that
difference is not a demonstrated speedup. Five-second idle controls remain close
to five seconds; the short CPU controls vary, further limiting interpretation of
small SMP timing differences. All timed phases have zero block reads and writes.

Separate probe-enabled boots also use two repetitions per version/topology, with
the second pair reversing order. Warm GCC measurements are:

| CPUs | Before / after IRQ-off CPU-s/s | Before / after machine capacity | Before / after intervals per compile | Before / after mean interval |
| --- | ---: | ---: | ---: | ---: |
| 1 | 0.136778 / 0.139569 | 13.678% / 13.957% | 8.028M / 9.083M | 322.7 / 304.7 ns |
| 4 | 0.174543 / 0.180045 | 4.364% / 4.501% | 8.207M / 9.510M | 421.9 / 371.4 ns |

Rates and counts are means across boots; interval durations are weighted by
completed count. These remain instrumented-kernel measurements: warm probe-enabled
times are roughly 15–18% longer than the corresponding OFF means. One baseline
SMP warm snapshot has an open interval at an edge, retaining the boundary error
described above. None of the measured compiles has a completed IRQ-off interval
at least 10 ms long. More, shorter intervals do not establish improved aggregate
duty or a better worst-case latency distribution.

The network comparison uses the existing q35/512 MiB native virtio-net fixture,
SLIRP and the same localhost HTTP server. Each fresh boot performs two 16 MiB
warmups, ten small requests, three 64 MiB downloads and three 64 MiB uploads.
There are two boots per version/topology, reversing version order on the repeat.
The table averages the two per-boot medians; host packet capture is enabled
identically in both versions.

| CPUs | Direction | Before | After | Change |
| --- | --- | ---: | ---: | ---: |
| 1 | Download | 114.651 Mbit/s | 54.670 Mbit/s | -52.3% |
| 1 | Upload | 139.501 Mbit/s | 86.237 Mbit/s | -38.2% |
| 4 | Download | 203.708 Mbit/s | 209.955 Mbit/s | +3.1% |
| 4 | Upload | 190.166 Mbit/s | 200.491 Mbit/s | +5.4% |

All 48 measured transfers completed without repeated TCP segments or zero-window
advertisements. One-CPU download ACK turnaround, taking the median of each
transfer's median, increases 0.632 → 4.411 ms; four-CPU turnaround stays near
0.349 → 0.357 ms. Upload host ACKs remain fast. The evidence supports slower guest
service on one CPU, not the earlier packet-loss/retransmission-timeout cliff.
It does not yet locate the exact scheduling or execution cost responsible.

Source inspection identifies additional IRQ transitions in the new bookkeeping.
An uncontended IRQ-enabled `NoPreemptSpinlock` pair masks separately in
`Preemption::disable()`, release's `Preemption::disabled()` ownership check, and
`Preemption::enable()`. The old lock used one continuous masked interval.
`Processor::executionContext()` and scheduler/affinity queries now also call the
masking depth query. Eligible outermost releases call `servicePendingScheduling()`
even when no request is pending; that path performs two pending-bit CAS operations.
The counter coalesces nested disables, so these added boundaries are real pulses,
not double-counted nested masking. The mechanisms explain how counts can rise and
provide concrete optimization targets; they do not quantify their share of GCC
cost or prove the cause of the larger network regression. A controlled ablation
is still needed for that attribution.

All 16 GCC boots passed compilation, execution and anonymous-memory contracts;
all eight network boots completed. The normal OFF configuration was restored and
the CMake cache matches its initial contents. Raw logs, commands, payload hashes,
frozen kernels/images, packet captures and summaries are retained in
`/private/tmp/pedigree-spinlock-ab`; `summary.json`, `comparison.json` and
`network/summary.json` contain the derived results. These temporary artifacts
preserve the evidence, while the two Git revisions identify the source comparison.

## Kernel metrics baseline, 2026-09-28

The spinlock split is retained. `/proc/metrics` now exposes per-CPU cumulative
scheduler, preemption and lock-policy counters, plus x64 interrupt, exception and
syscall entries. Each open freezes one readable snapshot. The existing
[compile benchmark guide](../../scripts/benchmarks/compile-latency.md#cheap-kernel-counters)
describes capture, definitions and rates. These counters measure frequency;
IRQ-off duty and lock-wait duration still require separate timing instrumentation.

Counter overhead was checked with the same source and RAM-root workload, changing
only `PEDIGREE_METRICS`. QEMU/TCG used q35, SandyBridge, 4 GiB, no NIC, fresh
writable overlays, precise accounting, and latency probes disabled. Each topology
ran two boots per arm in opposing orders (OFF/ON/ON/OFF for one CPU,
ON/OFF/OFF/ON for four). Snapshot collection was disabled in both timing arms.

| CPUs | GCC phase | Counters OFF, median | Counters ON, median | Change |
| --- | --- | ---: | ---: | ---: |
| 1 | Cold | 17.465 s | 17.501 s | +0.2% |
| 1 | Warm | 16.313 s | 16.302 s | -0.1% |
| 4 | Cold | 18.970 s | 19.060 s | +0.5% |
| 4 | Warm | 17.621 s | 17.363 s | -1.5% |

Host elapsed measurements agree with the direction and approximate magnitude.
All timed phases had zero disk read/write bytes. The four-CPU warm ranges overlap:
16.835–18.407 s OFF and 16.803–17.923 s ON. These runs show no clear GCC regression
from the counters; they do not establish zero overhead, a speedup, network cost,
or physical-hardware behavior.

Separate enabled boots captured both edges of every phase. The warm compile
windows were 16.824 s on one CPU and 18.346 s on four CPUs. Counts below sum CPU
labels and include snapshot collection and background activity; they are not
process-owned counts.

| Counter delta | 1 CPU | 4 CPUs |
| --- | ---: | ---: |
| Syscall entries | 124,057 | 124,057 |
| Exception entries | 114,473 | 114,472 |
| Scheduler yields | 633 | 21,711 |
| Actual context switches | 3,012 | 2,255 |
| Same-thread selections | 209 | 21,831 |
| Scheduling-service attempts | 220,456 | 270,749 |
| Preemption-state queries | 1,032,032 | 1,413,040 |
| No-preempt lock acquisitions | 95,494 | 95,491 |
| IRQ-masking lock acquisitions | 3,675,733 | 4,288,136 |
| IRQ-masking acquisitions that spun | 0 | 2,651 |
| Interrupt entries / scheduler timer callbacks | 935 / 935 | 30,094 / 1,137 |
| Automatic balancing migrations | 0 | 85 |

Plain-lock acquisitions and no-preempt acquisitions that spun were zero in both
warm windows. The one-CPU run therefore provides a concrete target outside lock
contention: about 218,482 IRQ-masking acquisitions, 61,343 preemption-state queries,
and 13,104 scheduling-service attempts per second, compared with 179 actual
context switches per second. Source review already identifies IRQ pulses in the
preemption helpers, but these counts do not attribute elapsed time to them.
The four-CPU IRQ-lock contention frequency was 0.062%; rare waits could still be
long, so frequency alone cannot exclude a contention cost.

The four-CPU idle control also recorded about 1,037 yields and 1,113 interrupt
entries per second across CPUs, versus 104 actual context switches per second.
Future ablations should retain the idle control and distinguish scheduling
attempts and same-thread selections from actual handoffs. None of these counters
by itself demonstrates a continuously running background thread or a root cause
for the earlier network regression.

Validation: enabled one- and four-CPU guests passed the full system-status
contract (including fragmented reads, seek/dup stability and advancing fresh
snapshots), procfs kernel-thread inspection and scheduler API checks. The disabled
one-CPU guest passed the same checks. All eight timing boots and both capture
boots passed GCC, executable and anonymous-memory checks; all 13 focused runner
tests passed. The normal build ends with counters ON and latency probes OFF.
The timing payloads precede a formatting-only correction for 64-bit values on
32-bit targets; their hot counter code is identical. Final snapshot text was
rechecked in fresh one- and four-CPU guests. Frozen payload hashes, commands, raw
snapshots, reports and `analysis.json` are in `/private/tmp/pedigree-metrics`;
these local artifacts are temporary.

## Demonstrated causes of earlier cliffs

### GCC and SMP

The earlier four-vCPU diagnostic measured 236,866 active mapping invalidations and 746,755 TLB IPIs. Restricting private-address-space invalidations to resident CPUs reduced IPIs to 27,670, about 96.3%, without reducing the mapping workload. AP startup also needed the BSP's PAT and cleared CR0 cache-disable bits. Combined RAM-root cold/warm times moved from about 25.43/24.03 seconds to 17.95/16.57 seconds, with warm system time falling from about 9.85 to 3.77 seconds.

This is a demonstrated coordination cost, not an inference from instruction counts. More cores and additional cross-CPU synchronization can add waiting rather than throughput. Current code retains residency targeting in `src/system/kernel/core/processor/x64/VirtualAddressSpace.cc:1314`.

The previously reported T420 progression, 87.460 seconds elapsed to 5.500 seconds warm, supports the combined fixes on hardware. The final report was 4.230 seconds user plus 1.240 seconds system, leaving 30 ms. It does not isolate the individual hardware effects and was not remeasured here.

### TCP retransmission cliff

The original 16 MiB downloads took 44.23 and 51.37 seconds. The retained mailbox experiment reduced them to 2.80 and 2.96 seconds and removed retransmitted data segments. `sys_mbox_trypost` had treated transient mailbox mutex contention as a full queue, dropping packets that could otherwise have been accepted.

The causal chain was contention → false admission failure → packet loss → TCP retransmission timeout → long wall time with relatively little instruction work. The present adapter waits for the mailbox mutex but never for free queue space (`src/modules/system/lwip/sys_arch.cc:313`). A genuinely full queue must still reject input.

A separate Intel receive-stall repair corrected IOAPIC verification that included volatile redirection bits. Neither mechanism is a general CPU-speed limit.

### Packet capture overhead

The old built-in capture module processed packet bytes and called the serial path even when the intended UART was absent. Removing just that capture work improved download/upload from 107.246/162.683 to 168.639/229.617 Mbit/s. The current build has `PEDIGREE_PCAP=OFF`. This is a demonstrated per-packet cost, separate from the retransmission cliff.

## Why Linux's network lead is not proof of a common kernel problem

The most discriminating retained control ran the exact Pedigree lwIP protocol sources inside Linux, using a pthread adapter, netconn API and per-frame AF_PACKET I/O. The application used common 16 KiB chunks; each direction was the median of three 64 MiB memory-only transfers after warmup, with matching SLIRP topology and disabled NIC offloads.

| Stack | 1 vCPU down / up, Mbit/s | 4 vCPUs down / up, Mbit/s |
| --- | ---: | ---: |
| Native Linux TCP | 736 / 552 | 1,503 / 836 |
| lwIP on Linux, matched settings | 122 / 251 | 125 / 305 |
| Pedigree | 102 / 227 | 137 / 240 |

Replacing Pedigree's scheduler, clock and kernel with Linux did not remove most of the network gap. This implicates protocol/integration choices and handoffs; it does not establish an intrinsic lwIP ceiling, because the Linux adapter itself adds syscalls, copies and synchronization.

A separate direct-Ethernet control, with comparable larger buffers and direct input, improved downloads from 306.611 to 609.442 Mbit/s when replacing netconn with raw callbacks. Across three downloads, voluntary context switches fell from 58,316 to 133 and process CPU from 8.08 to 2.59 seconds. Native Linux in that topology reached 1,210.907 Mbit/s. No observed retransmissions, zero windows or adapter drops accompanied those two buffered lwIP variants.

In a separate lwIP 2.2.1 experiment (the preceding comparisons use 2.0.2), increasing windows while retaining the fixed 64-slot queued-input mailbox produced another real cliff inside Linux: about 31.9 Mbit/s and 4,038 retransmitted segments. Larger buffers alone cannot repair a service pipeline that overflows its admission queue. SLIRP did not negotiate window scaling in the older matched controls; those runs cannot establish large-window WAN behavior.

Post-fix small-window captures generally have millisecond-scale gaps and no retransmissions, rather than the original multi-second stalls. Window replenishment, batching and API ownership therefore remain concrete network performance targets.

## What the accounting gap does and does not mean

The GCC driver includes waited-for compiler descendants. That still is not all machine CPU time. Device IRQ workers and the lwIP processing thread can do substantial work outside a client process's accounting ownership. A network client's low user/system total can coexist with a busy kernel network pipeline. It must not be interpreted as proof that the entire machine slept.

Precise accounting is based on guest time at thread/mode boundaries. It is not a direct measurement of host scheduler CPU allocation. That is why this investigation combines guest accounting, host elapsed time, idle/CPU controls and host thread profiles instead of treating any one counter as decisive.

## Exclusions and remaining uncertainty

- The inspected GCC PCI topology contains no UHCI controller. Its 1 ms root-port polling cannot explain these runs. That driver remains relevant to different device configurations, including physical machines, but no hardware claim follows from this exclusion.
- Current x64 deadline mode disables RTC IRQ8. The old continuous 512 Hz RTC hypothesis does not describe this configuration.
- Timed disk I/O, a large current GCC accounting remainder, a continuously spinning spare-vCPU pool, and a dominant host-blocking/retranslation storm are not supported by these controls.
- This investigation does not identify every contributor to the remaining GCC execution-time gap. The host profile leaves most translated guest PCs unresolved; reducing a dispatch count alone would not close that gap.
- No fresh T420 run, physical 82579LM test, or broad arbitrary-workload qualification was performed. Results are specific to the stated source, configuration and workload.

After the AHCI correction, the next justified GCC investigation is time-weighted attribution of the residual guest execution with exact guest-PC mapping and matched workload phases, keeping kernel/user boundary costs and memory translation separate. The next network investigation should measure queue admission, ACK/window replenishment and handoffs together. Neither should start by adding delays, tuning timer rates blindly, or weakening synchronization.

## Reproduction and evidence

Evidence directory: `/private/tmp/pedigree-global-rca`. `manifest.json`, `pending-source.patch`, `pending-new-source/`, `source-status.txt`, and the frozen payloads identify the baseline. `timing-controls.json` holds the baseline/accounting/Linux timing comparison and `fix-controls.json` the corrected repetitions; each run retains `command.json`, `report.json`, `serial.log`, PCI topology and QMP counters. `host-profile-summary.jsonl` and the original sample files preserve diagnostic attribution.

The disposable `run.py` delegates to `scripts/benchmarks/run-compile-latency.py` and adds PID/PCI metadata. The second single-vCPU baseline also samples QMP state once per second during preparation, stopping before timed phases. Only separately named debugger runs open a loopback GDB port; their elapsed times are not used as performance controls. `qemu-linux` preserves the earlier Linux command construction while directly executing QEMU. `timing-controls.py` records the arm order and exact commands. The profiler and instruction plugin are absent from timed phases in the timing controls. The first baseline single-vCPU preparation had a short host sample, and its repetition had QMP preparation sampling. The final `baseline-clean-1` run has neither and reproduces the same cliff. The corrected timing runs also have neither.

Historical evidence was re-read from `/private/tmp/pedigree-vm-singlecore` and `/private/tmp/pedigree-network-check`; compact network result files are copied into `historical-network/`. These are earlier experiments, not additional fresh repetitions.

Validation: fresh `cmake --build build --target uefi-image --parallel 4` passed. The baseline, accounting ablation, Linux controls and all three corrected guest runs passed their benchmark markers. The final CMake cache exactly matches the original; `PEDIGREE_TIME_ACCOUNTING=TRUE` and precise accounting are restored. Existing unrelated edits and untracked source files were preserved. `git diff --check` passed. No commit or push was made.

This validation concerns read latency and completion delivery. `PEDIGREE_CRIPPLE_HDD=TRUE` remained unchanged; these runs make no new writeback or persistence claim. The separate AHCI persistence/error-injection suite was not rerun. Debugger-attached runs are diagnostic evidence only.

To repeat the timing check with the retained fixture, use the existing runner with a fresh output directory (substitute `current.img` for the pre-correction arm):

```sh
UV_CACHE_DIR=/tmp/pedigree-uv-cache uv run --no-project python \
  scripts/benchmarks/run-compile-latency.py \
  --image /private/tmp/pedigree-global-rca/ahci-fix/ram.img \
  --output /private/tmp/pedigree-global-rca/repeat-1 \
  --firmware-code /private/tmp/pedigree-gcc-deep/ram-repeat/firmware-code.fd \
  --firmware-vars /private/tmp/pedigree-gcc-deep/ram-repeat/firmware-vars.fd \
  --cpus 1 --quick --skip-sync --timeout 300
```

Compare `phases[0].ready_host_s` and its `blocks_before` in `report.json`, as well as the timed phases; repeat with `--cpus 4`. The local evidence directory is temporary, so retain the frozen image/manifests if exact future reproduction is required. `evidence.tar.gz` contains the report, measurements, profiles, diagnostic scripts, patches and manifests; the large disk images and binaries are excluded from that compact archive.
