# Scheduler wait and asynchronous lifetime refactor

Date: 2026-08-02

Status: historical working record. This captures the August 2 refactor
checkpoint; later IRQ, scheduler, POSIX, and verifier follow-up is not folded
back into its test counts or remaining-risk list. Use `RESTORATION.md` and a
fresh `./verify.sh` run for current support and verification claims.

## Outcome

Pedigree now has one kernel contract for blocking:

> A waiter is published under the same queue guard which protects the
> predicate, before any lock is released and before the scheduler can commit
> the thread to `Sleeping`.

Code outside the wait implementation can no longer directly put a thread to
sleep or make it ready. The scheduler refuses to commit `Sleeping` without an
active `WaitQueue` record, and a wake which wins before that commit is retained
in the record. The condition check, waiter publication, external-lock release,
scheduler transition, wake reason, cancellation, and retirement are therefore
one protocol rather than conventions spread across callers.

The refactor also treats asynchronous lifetime as part of waiting correctness.
Unregistering a callback, stopping a worker, cancelling an I/O request, joining
a thread, or unloading a module must close admission and drain work which
already owns the relevant pointer, stack frame, buffer, or code.

The same contract now extends across the hardware-interrupt boundary. Ordinary
device handlers do not execute driver logic in hard IRQ context. The hard stage
is limited to controller acknowledgement/masking, bounded device quiescence,
and lock-free publication; the registered `IrqHandler` runs on a manager-owned
thread with interrupts enabled. A driver which genuinely needs a top half must
opt into the separate `HardIrqHandler` type or use `SplitIrqHandler` and is
covered by an exact registration allowlist.

Trigger handling is explicit rather than inferred. Edge, level, and synthetic
sources declare controller-acknowledgement order and whether a masked line is
released after the hard stage or only after threaded completion. PCI INTx uses
the level-threaded policy, so the PIC masks before EOI and rearms only after the
worker has quiesced the device. This is the softirq-style architecture the old
`tryWriteFromInterrupt` and ad-hoc interrupt-context checks were missing.

This is a broad correctness refactor, not a claim that every kernel concurrency
bug is gone. The implementation and tests cover the logical races described
below. Architecture memory ordering, real interrupt controllers, and real DMA
still require SMP QEMU or hardware.

## Faults made deterministic

| Fault | Previous failure window | Contract now used |
| --- | --- | --- |
| Wake before sleep | A producer woke after the condition check but before the scheduler committed `Sleeping`. | `WaitQueue::Guard` publishes a persistent waiter first; the scheduler aborts a stale sleep when its reason has already changed. |
| Condition-variable lost wake | The caller mutex was released before the thread became discoverable by `signal()`. | `waitAndUnlock()` publishes under the queue guard, releases the mutex, blocks, and reacquires the mutex before returning. |
| Wake and event collision | An ordinary wake won the queue reason, then a signal or timeout marked the thread before it resumed; the marker was ignored and the caller re-blocked. | `Semaphore` and `ConditionVariable` treat the per-wait interruption marker as authoritative independently of the queue wake reason. |
| Terminal abandonment | Thread termination consumed a stack while a timer, callback, or completion owner still held it. | Waits have explicit abandonment callbacks or completion semantics; `TerminationDeferral` holds teardown outside ownership scopes. |
| Non-returning state abandonment | Syscall handler leases, termination-deferral scopes, and page-fault dispatch records relied on C++ destructors which kill, event-return, register-restore, userspace-jump, or reboot paths could bypass. | Terminal syscall actions are staged until ordinary handler cleanup has run; per-thread, per-state-level deferred records are retired newest-first before a stack or state is abandoned. |
| Timeout callback lifetime | A timeout event was removed while already queued or running, or its storage was freed after another wake won. | Timer removal is an admission barrier, queued events are culled, and completion/cancellation drains before storage retirement. |
| Callback unload race | A registry removed a pointer while an invocation had already copied it outside the registry lock. | Registrations close admission and synchronously drain pinned invocations. Registries which support self-removal defer it; a syscall registration token instead rejects self-reset while its handler lease is live. |
| Detached worker lifetime | A detached thread retained an object or module pointer after teardown. | Object-owned workers use `OwnedThread` or an explicit stop/wake/join-for-completion sequence. |
| Request queue teardown | Startup, suspend, destroy, and active-request transitions raced with an unowned worker and raw request pointers. | `RequestQueue` has worker-ready rendezvous, explicit states, request references, cancellation, close/drain, and an owned worker. |
| Join and reaping race | A target could be reaped or its process could disappear while a joiner or observer still held a raw pointer. | Thread/process lifetime leases pin objects; join publication, detach, reaping, and process-exit election use guarded state. |
| Synchronous USB early return | A timeout or signal returned while a host controller could still write the caller's buffer or issue its callback. | Submission has an acceptance result and timeout uses cancel-and-drain with an exactly-once completion handoff. |
| Network receive ABA | A queued receive used a raw `Network *` key which could match a later object at the same address. | Device registrations carry a generation and queued requests validate it under the registry lock. |
| Concurrent log corruption | `Log::flushEntry` released the lock and continued reading a shared mutable staging buffer. | Each entry is snapshotted before dispatch; concurrent writers no longer share the flushed entry state. |
| POSIX teardown busy-spin | Process teardown repeatedly tried the memory-map operation gate without blocking, monopolising a core while its owner needed to run. | The teardown path now uses the gate's blocking operation-barrier acquisition before becoming unscheduleable. A hosted regression holds the real `PosixSubsystem` lifecycle gate during destruction, verifies that its reaper sleeps rather than spins, and then observes completion after release. |
| IRQ registry lock inversion | PIC dispatch tried to acquire the same registry writer lock needed by ordinary registration or teardown while `RFLAGS.IF=0`, producing the observed boot hang. | Hard dispatch scans a fixed atomic publication table and pins callbacks with preallocated hazard records; registry mutation and synchronous drain remain ordinary-thread work. |
| PIC controller ownership stall | A hard entry could spin on controller state held by a preempted thread, while an ordinary higher-priority waiter could spin on the same CPU and prevent that owner from running. | Hard entries use a lock-free gate and publish counted terminal actions instead of waiting. Ordinary threads first take a sleeping owner mutex; callback-time atomic mutation makes one direct gate attempt and returns failure on genuine contention. |
| PIC completion handoff stall | Deferred acknowledgement temporarily masked IRQ0, then transferred restoration to a BSP worker whose doorbell did not itself send an IPI. The masked timer could no longer provide the scheduling boundary expected to run that worker. | Temporary masks unrelated to a handler-lifetime transition are restored by the current owner before release. A racing vector either acquires the clean gate or publishes pending work and defeats the release CAS. Only replacement-lifetime publication is handed to the BSP. |
| Stale ExtINT after handler replacement | Masking a PIC line did not retract a vector already accepted by the BSP. Publishing a new handler lifetime from an AP could therefore qualify the old vector against the replacement handler set. | Every line transition retains a distinguished transition lifetime until the BSP disables interrupts, drains pending controller work, publishes the new lifetime, applies the physical mask, and releases ownership as one terminal sequence. |
| Duplicate virtual-wire ownership | Firmware LINT0 state could leave more than one local APIC accepting the legacy PIC, making controller ownership and EOI assumptions false. | The architectural BSP bit selects exactly one ExtINT LINT0 route; every AP programs LINT0 masked. Readback is checked and failure to establish or at least mask the route is fatal. |
| Hard-IRQ driver execution | Device callbacks logged, allocated, took sleepable locks, woke queues, or called other subsystems while interrupts were disabled. | Normal handlers are manager-threaded by default; explicit split handlers publish fixed records from the hard stage and perform recovery, callbacks, batching, and logging in their worker. |
| Level-triggered interrupt storm | EOI or unmask could occur before a threaded PCI handler had cleared the device source, immediately retriggering and starving ordinary work. | `IrqPolicy` separates electrical trigger, controller acknowledgement, and line-release timing; level-threaded lines remain masked until the worker reports safe completion. |
| Bottom-half publication race | A one-bit wake token could be cleared while another interrupt published work, stranding pending work or a teardown waiter. | Counted generations, lock-free MPSC publication, predicate-backed workers, and re-publication during token release retain racing work. |
| Unbounded controller wait | A missing device transition could spin forever during init, command completion, reset, or teardown. | Reviewed controller polling now has a visible time deadline or a finite terminal poll budget, pauses between probes where appropriate, and fails the device offline. A deliberately narrow whole-tree scanner rejects the hardware-read and loop shapes it recognizes. |
| Scheduler-timer selection | PIT remained armed at 100 Hz even when LAPIC drove scheduling, while a failed LAPIC could still be returned as the scheduler timer. | PC timer selection defaults to PIT and switches to LAPIC only after successful BSP discovery/initialization; the unused PIT is never registered/programmed. Early LAPIC failure keeps a one-CPU PIT fallback, but failure after LAPIC selection or during AP startup still panics rather than performing runtime failover. |
| IRQ-time accounting callback | Every x86-64 interrupt entered generic `TimeTracker`; `Process::trackTime()` synchronously invoked POSIX interval-timer and signal code before vector dispatch, reaching spinlocks, scheduler/process locks, allocation, logging, events, and WaitQueues with interrupts disabled. | `InterruptTimeAccounting` performs monotonic per-thread baseline updates, atomic process totals, and one coalesced report-edge publication only. A predicate-backed per-CPU worker leases processes and is the sole deferred caller of `reportTimesUpdated()`. CPU-timer `getitimer`/`setitimer` paths may still settle totals and signal synchronously in ordinary syscall context. Teardown closes and drains report admission. |
| Shared process CPU baseline | Two threads in one process used the same user/kernel entry timestamps; SMP execution could exchange each other's baselines, and a kernel-mode interrupt was incorrectly charged as user time. | Entry baselines live on the exact `Thread`, backward/uninitialized monotonic samples are rejected, handler time is always kernel time, and process totals are lock-free aggregates. |
| Coalesced interval-timer drift | Delayed accounting discarded periodic-timer overshoot, and an exact expiry boundary could fail to signal. | Pure interval state consumes the entire elapsed batch, retains the phase after multiple periods, disarms one-shot timers exactly, and coalesces standard signals to one pending expiry. |
| Cross-compiler aggregate return | The GCC 8 module compiler passed a hidden result pointer for `IrqHandlerRegistry::dispatchThreaded()`, while the native kernel compiler expected `this` in the first argument; the kernel treated the three-byte result object as a registry and ASan reported a stack overflow read. | Exported APIs return scalar admission and fill result state through explicit out parameters. The same scan found and converted `RangeList::getRange()`, and its new invalid-index regression also exposed a capacity-versus-count bounds bug. A public-header gate rejects nested aggregate returns on exported classes. |
| PC TSC clock regression | RTC calibration truncated the TSC rate to integer ticks per nanosecond, every CPU used the BSP TSC origin, and unordered `rdtsc` samples could move backward across CPUs. | The PC clock uses ordered `lfence; rdtsc`, exact rational cycles/nanoseconds conversion with 128-bit intermediate arithmetic and saturation, per-CPU anchors installed before AP scheduling, migration-safe sampling, the coarse RTC floor, and one bounded global compare-exchange to clamp observed regressions. This is exact conversion of the measured ratio, not a claim of exact hardware time. |
| Hosted sanitizer fiber metadata | Saved hosted scheduler contexts retained zero `ucontext_t.uc_stack` bounds, even though `HostedSchedulerState` had carried unused explicit stack fields since 2020. ASan therefore believed resumed fibers had a zero stack top and reduced the real failure to a misleading one-frame `clock_gettime` overflow. | Every hosted state publishes its kernel or auxiliary stack bounds, the sanitizer switch hooks consume those explicit bounds, and the initial fiber handoff records the original host-stack bounds. A hosted checkpoint verifies the saved waiter state before exercising the switch. |
| Hard-boundary diagnostic recursion | A legacy hard-IRQ regression released a `Semaphore`. The runtime guard tried to report that forbidden operation through `Log`; callback snapshotting touched another `WaitQueue`, re-entered the same guard, and recursed until stack exhaustion. | IRQ-originated semaphore wakeup now runs through the manager-threaded handler. A production guard violation latches the operation and takes a terminal path which does not enter Pedigree logging, allocation, waiting, or debugger machinery. |
| IRQ global-constructor heap entry | The static `Pic` and `Rtc` objects constructed their threaded-dispatcher names before `SlamAllocator` and `Log`. Copying the dynamic `String` entered the unconstructed allocator lock; its warning entered the unconstructed log lock and recursively exhausted the boot stack before `Processor init`. | `ThreadedIrqDispatcher` retains its constructor-time name in bounded inline storage and creates the dynamic worker name only from `initialise()`, after allocator and scheduler startup. The x86-64 constructor audit and boot smoke cover the linker-order-sensitive path. |
| AP transition-GDT regression | The AP trampoline received its STARTUP IPI and set `CR0.PE`, but the BSP had copied the 64-bit GDT into its real-mode transition area. Its first far jump therefore selected a long-mode code descriptor before EFER.LME and paging were enabled, stranding SMP boot at processor #1. | The low-memory transition area again receives the 32-bit GDTR/GDT. The 32-bit trampoline loads the separate 64-bit GDT only after establishing the prerequisites for long mode; a source contract and genuine four-processor QEMU boot cover the handoff. |
| Standalone POSIX harness linkage | Network syscall interruption tracking reached Pedigree `Processor` and `Thread` state even when the same source was linked into the standalone `unixsockets` utility, producing unresolved kernel symbols. | External-source builds retain the syscall behavior but omit only Pedigree thread-interruption bookkeeping. The standalone target is part of the canonical hosted utility build and executes its datagram and streaming cases. |

Where an interleaving must be forced, a hosted-only hook is inserted at the
exact old failure window. The competing operation is forced there and the test
asserts the blocked state, ownership state, result, and teardown. Protocol,
contract, liveness, state-cleanup, and static service-boundary tests use their
own direct oracles instead. The race regressions do not rely on running a
probabilistic stress workload until it happens to fail.

## APIs introduced or hardened

### `WaitQueue`

- `Guard::wait()` atomically publishes and blocks an interruptible waiter.
- `Guard::waitForCompletion()` keeps ownership state alive across terminal
  requests.
- `Guard::waitAndUnlock()` publishes before releasing a caller `Mutex`, then
  reacquires it for every returning outcome.
- `Guard::waitAndUnlockForCompletion()` combines the mutex handoff with a
  non-abandonable lifetime barrier.
- Channels distinguish independent predicates carried by one queue.
- Persistent, per-state-level waiter records retain wake reasons across the
  wake-before-block window.
- Wake and cancellation update the waiter under the queue and thread locks;
  scheduler notification occurs after those locks are released.

`PerProcessorScheduler::blockCurrent()` is private to the protocol in
practice, and rejects a sleep without a published waiter. Raw
`schedule(Thread::Sleeping)` and direct `Ready`/`Sleeping` transitions are
restricted to the scheduler, `WaitQueue`, and thread lifecycle internals.

### Blocking primitives

- `ConditionVariable` uses the atomic mutex-release/enrol protocol and reports
  timeout, signal interruption, deferred termination, and mutex ownership
  explicitly.
- `Semaphore` uses `WaitQueue`, owns timeout cleanup, preserves exact
  interruption reasons, and has a separate completion acquisition path.
- `Mutex` now records and validates its owner, rejects recursion and
  non-owner release, and no longer has the ambiguous boolean constructor.
- `Completion` represents one-way operation completion and result ownership.
- `UnlikelyLock`, `RingBuffer`, `ProducerConsumer`, `Buffer`, and
  `MemoryPool` use explicit admission, close, interruption, and drain states.

### Lifetime and teardown primitives

- `OperationBarrier` supplies close-admission plus drain for callbacks and
  in-flight operations. Exported acquisition uses a scalar result plus an out
  lease to avoid a non-trivial C++ return ABI across hosted module compilers.
- `OwnedThread` binds a worker to its owner's stop/wake/join lifecycle.
- `DeferredScopeRecord` is an intrusive thread-owned record carrying state
  level and sequence. It supports arm, disarm, move, and checkpoint
  retirement. Cleanup records are detached newest-first under the thread's
  deferred-scope lock, then invoked without that lock held.
- Normal state pop and cleanup reject armed deferred records. State
  abandonment, whole-thread kill, event return, and `TimeoutGuard` explicitly
  retire the applicable records before nonlocal control flow.
- `TerminationDeferral` and the compatibility `Uninterruptible` scope use the
  common deferred record rather than independent counters whose destructors
  could be skipped.
- `RelayEvent` transfers the latest event disposition without retaining an
  unsafe sender-owned event object.
- `IrqHandlerRegistry` closes IRQ admission and drains handlers before module
  or object teardown.
- Scheduler `ProcessLease` and process `ThreadLease` acquisitions use scalar
  results with out parameters and pin lifetime until the lease is released.
- `SyscallManager::Registration` is a move-only, generation-bearing ownership
  token. Public registration requires an out token; reset closes admission and
  drains existing handler leases, and POSIX's two service registrations roll
  back transactionally.
- A syscall `HandlerLease` arms abandoned-stack cleanup before admitting a
  callback. Normal return disarms that record before dropping the manager pin;
  abandonment detaches the record first and then invokes its cleanup.
- Thread/process exit, event return/pop, processor-state restoration,
  userspace jump, reset, and reboot are staged until the handler lease and
  termination deferral have retired.

`Uninterruptible` was retained only as a compatibility name. It is now
nestable event and termination deferral, not a global boolean which one nested
scope can accidentally re-enable.

### Interrupt execution and controller policy

- `IrqHandler` is the ordinary device interface. `Pic` and hosted
  `IrqManager` own the worker, publication cookie, unregister barrier, and
  callback lifetime; the driver callback runs with interrupts enabled.
- `HardIrqHandler` is a deliberately separate interface for the small set of
  architecture or device top halves that cannot defer acknowledgement.
- `SplitIrqHandler` pairs a bounded `hardIrq()` publication with a predicate-
  backed owned worker. Shutdown closes registration, rejects later hard work,
  drains already-published generations, and joins the worker.
- `IrqPolicy` independently names edge/level/synthetic triggering,
  before/after-hard controller acknowledgement, and after-hard/after-thread
  line release. Registration does not pretend to program platform electrical
  routing such as the PC ELCR.
- `ThreadedIrqDispatcher` uses a lock-free generation/cookie doorbell. Hard
  publication neither allocates nor touches a ready queue; the scheduler timer
  consumes the doorbell at its existing reschedule boundary.
- `DeviceHardIrqContext` marks explicit hard callbacks. Scheduling,
  semaphore/mutex operations, WaitQueue access, heap allocation, and heap free
  reject that context. Scheduler-timer and exception paths are narrow,
  reviewed architecture exceptions rather than device-driver precedent.
- `IrqHandlerRegistry` publishes fixed slots atomically and uses preallocated
  dispatch hazards, so dispatch never waits for its writer spinlock. Ordinary
  unregister closes admission and drains every callback which already pinned
  the handler.
- `PicControllerStateGate` qualifies deferred entries and tails with a line
  lifetime, retains exact EOI obligations, and lets a racing publisher defeat
  idle release with one compare-exchange. Controller-thread callers sleep
  behind a `Mutex`; hard callbacks which intentionally remove or replace a
  peer make one nonblocking gate claim and keep that claim across the
  registry's atomic unregister path.
- PIC line transitions use a distinguished unpublished lifetime. Only the BSP
  may publish the replacement, at an interrupts-off boundary immediately
  before any physical unmask. Stable temporary masks may be restored remotely
  while ownership is retained because a racing vector makes release fail.
- `LocalApicLint0Policy` derives virtual-wire ownership from
  `IA32_APIC_BASE.BSP`: the BSP uses ExtINT, APs remain masked, and runtime
  readback fails closed if the requested role does not latch.
- PIC diagnostics are atomically counted in the hard path and rendered later
  by the debugger. Spurious/unhandled reporting no longer formats log messages
  with interrupts disabled.

### CPU-time and interval-timer deferral

- `InterruptTimeAccounting` is the raw x86-64 entry scope. It samples the
  monotonic timer and performs only fixed lock-free baseline/aggregate updates
  plus accounting-worker publication.
- User and kernel baselines belong to `Thread`, not `Process`. They are kept
  per mode and per processor, and migration seeds a fresh destination baseline
  rather than subtracting timestamps from unrelated CPU clock domains.
- `DeferredTimeAccounting` is one coalesced pending report edge. User and
  kernel time remain absolute atomic process totals; the worker reads those
  totals and POSIX timer-local baselines derive the elapsed virtual/profile
  deltas.
- Every per-CPU scheduler owns a predicate-backed accounting worker. A counted
  generation is published before the shared IRQ doorbell; completion records
  only the generation actually scanned, so a publication racing drain remains
  scheduler-visible.
- The worker scans scheduler-published processes under `ProcessLease`, drops
  registry locks before callbacks, and enters a per-process
  `OperationBarrier` before invoking `reportTimesUpdated()`.
- Process termination closes report admission and drains a callback already in
  progress without holding the process lock. Aggregate CPU totals remain
  recorded; only the pending report edge is discarded so a dying process does
  not generate a new timer signal.
- POSIX virtual/profile timers consume a complete elapsed batch. A batch which
  crosses several periods preserves the next period's phase and emits one
  standard signal, matching non-queued signal semantics. Absolute timer-local
  baselines also reject a stale pre-rearm worker snapshot, so newly armed
  timers are not debited for time accumulated before arming.

### Hosted compiler ABI boundary

- Kernel exports use scalar returns plus explicit out parameters for detached
  results and lifetime leases. This avoids compiler-specific small-aggregate
  and non-trivial return conventions at the native-kernel/Pedigree-module
  boundary.
- `IrqHandlerRegistry::dispatchThreaded()` returns admission as `bool` and
  writes handled/rearm state through `ThreadedDispatchResult &`.
- `RangeList::getRange()` returns success as `bool`, clears and fills `Range &`,
  and validates against logical `count()` rather than Vector capacity.
- The exported-aggregate scanner covers public nested-struct returns declared
  on `EXPORTED_PUBLIC` classes. It complements, but does not replace, the
  explicit-template `Result` scanner or a general ABI compatibility analysis.

### PC monotonic clock

- RTC calibration retains the measured cycles/nanoseconds ratio instead of
  truncating it to integer ticks per nanosecond.
- Ordered TSC reads and an overflow-safe 128-bit scale preserve fractional
  conversion and saturate rather than wrap.
- Each processor receives an immutable `{local TSC, monotonic nanoseconds}`
  anchor before it can schedule migratable work. The sampling window briefly
  masks interrupts so a TSC cannot be paired with another CPU's anchor.
- A coarse RTC floor and a single bounded global compare-exchange clamp
  cross-CPU regressions without putting a retry loop in interrupt-time clock
  reads.

## Migrated subsystem inventory

### Scheduler, events, and process lifetime

- Scheduler and per-processor scheduler sleep/wake transitions
- Thread event nesting, interruption, join, detach, and reaping
- state-stack checkpoints and deferred cleanup for event return, state
  abandonment, and terminal stack destruction
- Process publication, parent/child ownership, exit rendezvous, reparenting,
  suspend/resume, and single-winner exit election
- IPC waits and event delivery
- timer alarms, delay, timeout guards, and shutdown quiescence
- page-fault and IRQ handler registration lifetime, including abandonment-safe
  page-fault dispatch cleanup
- debugger thread snapshots and wait-queue diagnostics

### Queues, storage, and filesystems

- `RequestQueue`, including startup, active-vs-backlog accounting,
  suspend/resume, terminal ownership, cancellation, and destroy
- cache callback and queued-object retirement
- `MemoryPool`, `Buffer`, `RingBuffer`, `ProducerConsumer`, and `RadixTree`
- VFS `File`, `Pipe`, `MemoryMappedFile`, and locked-file paths
- ext2, FAT, ISO9660, ramfs, rawfs, hosted disk-image, and loop-disk request
  and buffer ownership paths

### Drivers and module-owned asynchronous work

- ATA and SCSI controller/disk requests
- ATA interrupt completion is threaded; command, DMA, packet, reset, and ready
  waits have bounded failure paths rather than permanent boot/teardown stalls
- UHCI, OHCI, and EHCI threaded IRQ delivery, request cancellation,
  exactly-once completion ownership, callback serialization, and completion
  drain
- USB synchronous wrapper acceptance, timeout, and completion handoff
- USB PnP callback registration drain
- EHCI asynchronous-advance worker ownership and callback delivery ordering
- 3Com 90x, NE2000, and the built HCD/ATA paths move ordinary device work out
  of hard IRQ context; receive batches are detached under device locks and
  delivered after those locks are released. The dormant RTL8139 migration has
  source/static evidence only. Full CDI remains unsupported, but its IRQ
  adapter has a dedicated x86-64/hosted compile-check target in addition to
  the static boundary scan.
- DM9601 send/receive admission, USB-transfer ownership, worker shutdown, and
  teardown drain
- PS/2 controller split capture/drain, fixed lock-free input records, mouse
  worker ownership, watchdog, partition registration, and related request/IRQ
  teardown paths
- RTC hard-stage acknowledgement/count publication, threaded alarms/callbacks,
  single CMOS ownership, and bounded UIP/calibration waits
- Local APIC processor-control IPI submission, reversible debugger quiesce,
  terminal stop, bounded acknowledgements, and fixed atomic per-CPU scheduler-
  timer handler slots
- PC scheduler-timer selection, including PIT fallback and suppression of the
  unused PIT when LAPIC initialization succeeds
- VMware SVGA indexed-I/O/FIFO serialization, bounded FIFO drain, provider
  withdrawal, and framebuffer callback close/drain
- network-stack receive registration generation and cancellation
- InputManager callback snapshots, external removal, self-removal, shutdown,
  and queue cleanup
- Log callback snapshots/removal and per-entry snapshot isolation
- network filter callback unregister/drain for `pcap`
- syscall handler token ownership, abandonment-safe handler pins,
  unregister/drain, and staged post-handler terminal actions for unloadable
  subsystem modules

### POSIX and services

- file descriptor and process lifetime pins
- poll, pthread, signal, file, network, console, and process-system call waits
- base-state event-return rejection, complete x86-64 signal-state restoration,
  and Pedigree-C self-unload rejection
- virtual terminal and Unix filesystem wait paths
- status server and console/TextIO worker teardown
- memory-pressure manager/killer callback and worker lifetimes

## Debugger support

The `threads` debugger command reports the wait record for each sleeping
thread:

- queue address and channel owner/value;
- whether publication is complete or still in progress;
- current wake reason and nested event-state level;
- mutex owner for mutex-backed semaphore waits; and
- `INVALID: sleeping without waitq` for a scheduler contract violation.

This turns a stalled snapshot into wait-for evidence: the predicate, queue,
channel, owner, producer, and instruction address are visible together.
Repeated snapshots can distinguish an unchanged deadlock edge from slow but
real progress.

The `irqs` command complements that wait graph with per-line controller state:
delivery mode, trigger/ack/release policy, requested and effective mask state,
dispatch generation, pending threaded work, registered handlers, and total,
spurious, unhandled, and publication-failure counters. These fields are
snapshotted without making the debugger take the registry writer lock. In the
specific QEMU failure signature, repeated snapshots can therefore show whether
the PIC line is masked, whether a bottom half is making progress, and whether
unregister is draining a pinned callback; hard dispatch itself no longer spins
on `m_HandlerLock`.

The operational guide is in
[`docs/wait-queues.md`](wait-queues.md).

## Build-time gates

`verify.sh` rejects:

- sleeping outside `WaitQueue`;
- an escaped `blockCurrent()` call;
- direct `Ready` or `Sleeping` status changes outside the implementation
  boundary;
- ambiguous boolean `Mutex` construction;
- exported lifetime leases returned by value;
- `OperationBarrier` admission which bypasses the scalar/out ABI;
- compiler-dependent `Result` aggregates in explicitly instantiated exported
  templates;
- nested aggregate returns declared on exported public classes, which are not
  stable across the native-kernel/Pedigree-module compiler boundary;
- syscall handlers registered without a move-only ownership token, null-token
  registration, a legacy two-argument call, or a raw unregister API;
- direct non-returning thread, event, processor, userspace, reset, or reboot
  transitions from subsystem syscall callbacks;
- Linux `rt_sigreturn` falling through to a second event-return action;
- event return/pop admission at base state;
- Pedigree-C event-return fallthrough or a wrapper which discards the error;
- Pedigree-C requesting its own module unload from its live syscall handler;
- x86-64 state restoration discarding a Pedigree event state which it does not
  own;
- volatile hand-written lock flags; and
- obvious lock-shaped empty busy loops, including operation-gate acquisition
  hidden behind a nested call expression;
- ordinary driver registrations that bypass named threaded IRQ policy;
- production modules which call the raw processor `InterruptManager`;
- hard/split IRQ registration sites outside the exact reviewed architecture
  allowlist;
- process `TimeTracker` execution directly in a processor interrupt manager;
- time-accounting calls outside their reviewed receiver/mode scopes, a second
  absolute-time callback, missing accounting-worker lifecycle/build wiring, or
  an unbounded callee reachable from raw x86-64 accounting; and
- unbounded hardware-status loops, including wrapper-based register reads and
  nested loops whose counter does not actually force `break`, `return`, or a
  terminal failure.

The bounded-poll, hard-IRQ, time-accounting, exported-aggregate,
explicit-template ABI, hosted-marker, and syscall-registration scanners all
have fixture self-tests. The applicable scanners then check the whole source
tree or public-header surface. These gates prevent known bad API shapes from
being reintroduced, but they complement rather than replace deterministic
runtime tests. In particular, the poll scanner deliberately recognizes a
finite family of hardware-read and loop forms rather than claiming a general
termination proof.

For the dynamic rung, the hosted harness derives its expected
`HOSTED-WAIT-TEST` PASS-marker set from the regression sources, including
adjacent C string literals. Marker names must be unique and the runtime log
must contain the exact same set exactly once. The separate static
`HOSTED-SYSCALL-TEST`, static `HOSTED-NETWORK-TEST`, and userspace
`HOSTED-SMOKE` markers are asserted independently, and every corresponding
failure prefix is rejected.

## Verification

The canonical command is:

```sh
./verify.sh
```

It runs:

1. static wait/API boundary checks;
2. native unit tests without ASan;
3. the same native suite with ASan;
4. a hosted static-kernel lifecycle smoke;
5. the real hosted scheduler/module regressions with ASan and both the system
   allocator and `SlamAllocator`;
6. root-mount and userland lifecycle smoke rungs, including a POSIX/lwIP
   loopback roundtrip with bounded scheduling windows before connection and
   data delivery.

The dynamic syscall regressions cover deferred state-cleanup order, handler
lifetime, staged post-handler actions, the event-action boundary, and a real
abandoned stack:

- `state-cleanup-order`;
- `syscall-handler-lifetime`;
- `syscall-post-actions`;
- `syscall-event-action-boundary`; and
- `syscall-abandoned-stack`.

A separate static-kernel real-service test invokes the actual POSIX and
Pedigree-C services. Its
`HOSTED-SYSCALL-TEST: PASS real-event-boundaries` checkpoint requires
base-state SIGRET, unwind, and event-return misuse to return an error without
corrupting the state stack; it also requires the live Pedigree-C handler to
reject unloading its own module.

A static-kernel real-`NetworkStack` regression holds a queued receive before
dispatch, unregisters and destroys the original device, reconstructs a device
at the identical address with a new generation, and then releases and drains
the request. `HOSTED-NETWORK-TEST: PASS receive-generation-aba` requires one
stale discard, no delivery or cancellation, and exactly one terminal owner of
the buffer. This test is static-only: keeping lwIP and `network-stack` linked
avoids claiming that dynamically unloading them is safe while lwIP still owns
a detached worker.

The loopback rung is an end-to-end liveness and data/EOF check. Its
rendezvous exercises real userspace `socket`, `bind`, `getsockname`, `listen`,
`accept`, `connect`, `send`, `recv`, and `shutdown`, plus pthread creation and
join, one-byte integrity, and EOF. Bounded yield windows make it likely that
`accept` and `recv` block, but they do not force those exact POSIX/lwIP
predicate windows. The generic kernel-hook regressions prove the underlying
wait-publication protocol, not the exact syscall wrappers.

An x86-64 cross-build separately compiles selected architecture and real driver
targets which the hosted machine cannot instantiate.

### Final-tree validation status

The stable canonical command was attempted as:

```sh
PEDIGREE_VERIFY_JOBS=2 \
PEDIGREE_VERIFY_RUN_ID=wait-refactor-final-20260730 \
./verify.sh
```

It did not reach a compilation or test stage. The sandbox denied Git permission
to refresh `.git/modules/external/googletest/config`, so the `submodules`
preflight failed after one second. The exact result is in
[`build-verify/logs/wait-refactor-final-20260730/summary.txt`](../build-verify/logs/wait-refactor-final-20260730/summary.txt)
and its adjacent `submodules.log`. A requested unsandboxed retry was rejected
by the execution service's current usage limit. No manual sequence was used to
bypass that approval boundary.

Consequently, the following are **not verified on the final tree**:

- the canonical native non-ASan and ASan counts;
- the Docker-native utility suite;
- either hosted allocator mode, its ASan instrumentation check, or the six
  hosted lifecycle rungs;
- runtime appearance of the 83 expected dynamic `HOSTED-WAIT-TEST`
  checkpoints (these are checkpoints, not 83 independent tests);
- the separate static syscall, static network, and userspace loopback
  checkpoints; and
- the selected x86-64 target matrix.

Supplemental current-tree checks which did complete are:

- a fresh native non-ASan CMake build in
  `/tmp/pedigree-wait-native-check`, with CTest passing 1/1;
- shell syntax for `verify.sh`, `scripts/test-hosted-kernel.sh`, and
  `easy_build_hosted.sh`;
- self-tests for the explicit-template ABI, hosted-marker, and
  syscall-registration scanners, plus the whole-tree syscall-registration
  scan;
- focused hosted/x86-64 syntax, exported-wrapper/consumer ABI, and static-link
  boundary checks; and
- `git diff --check`.

Earlier focused hosted logs are useful implementation evidence but predate the
last syscall-boundary changes, so they are deliberately not reported as final
canonical validation. No QEMU, SMP, or hardware runtime was performed.

## What hosted proves

The hosted module lane runs Pedigree's real scheduler, thread/event machinery,
locks, timers, wait queues, process lifecycle, module loader, and teardown. It
also preserves the intentional native-hosted-kernel versus Pedigree-module
compiler boundary. That makes it the primary integration-test layer for these
logical interleavings.

Native unit tests remain the right layer for pure state machines, reference
counts, container boundaries, and overflow. Hosted tests are the right layer
for scheduler publication and lifetime handoffs. Neither requires emulated PC
hardware.

## Boundaries and remaining risk

- The hosted scheduler run is not proof of true SMP memory ordering. It does
  not execute x86-64 syscall/interrupt entry assembly,
  `InterruptTimeAccounting`, RTC/TSC code, PIC/LAPIC hardware, or real CPU-timer
  signal delivery.
- The C++ accounting scopes exclude architecture entry and return stubs, so
  that overhead remains attributed to the surrounding mode. There is no forced
  SMP accounting-worker race, in-flight callback/close race, real
  virtual/profile signal test, or process-accounting wrap test.
- Per-CPU TSC anchoring removes offsets but assumes a common, invariant TSC
  rate. Frequency drift is not detected and there is no alternate monotonic
  source. The single compare-exchange clamps an observed regression with
  bounded IRQ-path work; it is not a strictly completion-ordered linearizable
  clock.
- Hosted post-action regressions intercept the seven terminal actions only
  after RAII retirement. They do not execute a valid x86-64 signal restore,
  syscall entry/return assembly, userspace jump, reset, or reboot. The
  abandoned-stack regression does execute real thread termination.
- The static real-service syscall test covers invalid service boundaries. It
  does not prove successful dynamic POSIX or Pedigree-C unload.
- Deferred cleanup protects resources which explicitly arm a record.
  `TimeoutGuard` can retire those records before nonlocal control flow, but it
  cannot run arbitrary unregistered C++ destructors from an abandoned stack.
- Real interrupt acknowledgement/masking, DMA visibility, controller halt and
  suspend timing, MMU/page-table behavior, and privilege transitions still
  need focused x86-64 QEMU or hardware tests. A bounded boot smoke, even with
  multiple virtual CPUs, cannot prove all of these paths.
- `LocalApic::removeHandler()` atomically clears its per-CPU slot but does not
  pin and synchronously drain a handler already loaded by an interrupt. Local
  APIC timer callbacks also still receive `delta == 0`.
- A production device-hard-IRQ boundary violation is fail-stop because it
  cannot safely enter ordinary diagnostics. Hosted latches the operation,
  writes a fixed message, and exits; a native kernel latches it and halts only
  the offending processor. A system-wide emergency stop/report path would need
  a separate raw NMI, reset, or constructor-independent serial design.
- PIT fallback covers LAPIC discovery/initialization failure before LAPIC timer
  selection. There is no runtime failover after selection or during AP startup.
- The UHCI/OHCI/EHCI common cancellation contract is tested with hosted fakes;
  the real MMIO/DMA implementations receive architecture builds and static
  review, not runtime hardware proof.
- USB PnP drains factory/probe callbacks. Successfully bound, module-defined
  driver instances still need an explicit detach/delete lifecycle before
  arbitrary driver-module unload can be declared safe.
- Public raw network lookup APIs remain unpinned; migrated queued receives
  validate a registration generation, but future asynchronous users must use
  a lease or repeat that guarded validation.
- The static network ABA regression does not prove dynamic lwIP or
  `network-stack` unload; lwIP's detached worker is why those modules remain
  linked in that lane.
- The POSIX loopback does not force a live signal/timeout collision,
  close/teardown contention, or forced disconnect. The wrapper state machine
  separately verifies that every successful `result >= 0`, including zero,
  wins over a racing signal; the exact live syscall interleaving remains
  unforced.
- There is not yet a kernel-wide module-execution lease. The registries
  migrated here drain their own callbacks, but a module which unloads itself
  from inside unrelated module code can still return through unmapped text.
- lwIP's `sys_thread_new`, the generic `Time::runAfter` /
  `runConcurrently` helpers, the per-processor scheduler add worker, and the
  x86 keyboard reader still contain detached-thread lifetime contracts outside
  the migrated owned-worker set. The hosted build utility `TunWrapper` has the
  same object-lifetime shape.
- The bounded-poll scanner recognizes reviewed `while` loops and hardware-read
  shapes. Custom accessors, other loop forms, and indirect state machines can
  still contain an unbounded hardware wait.
- VMware FIFO arithmetic, timeout state, and provider lifetime have pure,
  static, and build coverage only. No real SVGA FIFO, hot-unplug,
  retained-provider client, or unload race was exercised. Provider withdrawal
  still depends on the graphics service remaining available and is fatal if it
  cannot be completed.
- The exported-aggregate checker covers nested aggregate returns declared on
  `EXPORTED_PUBLIC` classes. It is not a general ABI verifier for top-level
  aggregates, aliases, packing, calling-convention attributes, or other
  ABI-sensitive types.
- The `Log` no-lock entrypoints now use stable per-call entry storage, while
  their shared ring and duplicate-suppression bookkeeping remain intentionally
  unsynchronised across CPUs.
- Full CDI remains outside the supported module/runtime matrix and has missing
  external headers. Its IRQ adapter has targeted compile/static evidence only.
  RTL8139 is dormant and has source/static evidence only.
- Hosted ASan is run with `detect_leaks=0`; passing it is not leak-sensitive
  evidence.
- Clang thread-safety annotations are not yet integrated. Current static gates
  catch forbidden shapes, while predicate/lock relationships remain runtime
  invariants.

The important remaining distinction is therefore not “hosted versus QEMU.”
It is:

1. pure invariant tests;
2. deterministic real-scheduler hosted tests;
3. architecture compilation and static checks; and
4. SMP/emulated-hardware or physical-hardware validation.

Most lost wakeups, teardown races, and module callback lifetimes belong in the
second layer and should be found there before booting a PC platform.
