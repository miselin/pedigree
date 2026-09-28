# Spinlock contracts

Choose the lock from the contexts that access its protected state:

| Type | Policy | Typical use |
| --- | --- | --- |
| `Spinlock` | Atomic exclusion and spinning only | Boot barriers with scheduling and local reentry already excluded |
| `NoPreemptSpinlock` | Suppress preemption and migration; preserve the caller's IRQ state | Short, nonblocking thread-only bookkeeping |
| `NoIrqSpinlock` | Mask local IRQs while owned; restore entry state on release | IRQ/fault-reachable state and scheduler handoffs |

Use `LockGuard<T>` with the selected type. An ordinary `Spinlock` does not prevent
an IRQ or another local thread from interrupting its owner and spinning forever.
A section that must block needs a waitable primitive such as `Mutex`.

## Atomic primitive

`SpinlockWord::tryAcquire()` attempts once and returns whether it acquired the
lock. A successful compare-and-exchange has acquire ordering; failure has relaxed
ordering. `acquired()` is a relaxed snapshot, not an ownership or publication
barrier. The machine word contains zero while locked and one while unlocked.

`release()` publishes protected writes with a release store. `releaseChecked()`
uses a release compare-and-exchange and returns false if the word was already
unlocked. This detects duplicate release, not whether the caller owns the lock.
An initially locked word can be released without a preceding acquisition.

The primitive neither waits nor manages interrupts, recursion, ownership, or
lock tracking. A caller must supply those policies. In particular, it cannot
replace a scheduler thread lock without preserving the scheduler handoff.

## Preemption and recursive ownership

`NoPreemptSpinlock` pins execution to the current CPU before attempting the atomic
acquisition. Its per-CPU preemption depth nests across locks and explicit
`Preemption::disable()` / `enable()` pairs. Only counter updates briefly mask
IRQs; contention and the critical section retain the caller's IRQ state.
IRQ and debugger handlers must not acquire this lock, including recursively.

Recursive acquisition is opt-in through `NoPreemptSpinlock::allow_recursion` or
`RecursingLockGuard<NoPreemptSpinlock>`. Each release retires one depth, and the
outermost release publishes unlock before allowing a context switch. Pending
timer and worker scheduling requests remain pending while preemption is disabled.
The outer IRQ-enabled release services them at an ordinary thread boundary;
if IRQs remain masked, a deferred one-shot quantum remains armed.

Blocking, yielding, migration, stack abandonment, and returning to userspace with
a live depth are rejected. Hosted signal frames retain their existing scheduling
boundaries; `enable()` does not schedule from a live hosted signal frame.

## Interrupt and recursive ownership

`NoIrqSpinlock::acquire()` saves the current interrupt state locally, disables local
interrupts, and acquires the word. Only a successful new owner writes the saved
state into the lock. This prevents a contender from replacing another owner's
restoration state. An ordinary thread that entered with IRQs enabled may enable
them while waiting on another CPU, then masks them again before acquisition.
Callers that entered with IRQs disabled keep them disabled throughout contention.

Recursive acquisition is opt-in through `NoIrqSpinlock::allow_recursion` or
`RecursingLockGuard<NoIrqSpinlock>`. Reentry requires both the owning CPU and thread
to match; CPU identity still matters when early boot threads are null. Nested
acquisitions retain the outermost saved interrupt state.

`release()` unlocks only at the outermost recursive level and restores that
level's saved interrupt state. It captures the state before publishing unlock,
because the next owner may immediately overwrite it. `exit()` follows the same
recursive release rules but leaves interrupts disabled. Both require interrupts
to remain disabled while the lock is owned. Ordinary scopes can use
`LockGuard<NoIrqSpinlock>` to pair acquisition and release.

## Diagnostics

The following diagnostics and lock-order tracker cover `NoIrqSpinlock`.
`NoPreemptSpinlock` always checks local ownership and balanced preemption depth.

`PEDIGREE_SPINLOCK_DIAGNOSTICS` defaults to `OFF`. Enabling it retains acquisition
call sites and corruption sentinels, checks interrupt state on release, and uses
`releaseChecked()` to detect an already unlocked word. Without diagnostics,
ordinary release uses the release store. Same-CPU deadlock detection and recursive
ownership remain active in both configurations.

`PEDIGREE_TRACK_LOCKS` independently enables lock tracking and also forces the
diagnostic fields and checks through `SPINLOCK_DIAGNOSTICS`. Enabling the debugger
alone does not enable Spinlock diagnostics.

## Scheduler handoff

`NoIrqSpinlock` supplies the scheduler's private `deferredReleaseWord()` clears ownership, recursion, and
diagnostic metadata while the word remains locked, then returns its address for
context-switch assembly to publish unlock after changing stacks.
`unlockForScheduler()` clears the same metadata and publishes unlock immediately.
Neither restores interrupts or performs ordinary lock-tracker release; the
scheduler owns those operations across the handoff.

All old-owner metadata changes must finish before publishing unlock. No old-owner
cleanup may overwrite a new owner's fields afterward. Scheduler paths must retain
the saved interrupt state until the matching context restores it.

## Verification scope

The policy split passed the 4 KiB native suite (1,316 tests), including the
atomic-word contention/publication and IRQ lock-tracker tests. Native shims model
atomic exclusion; they do not establish IRQ or scheduler behavior. The synthetic
1 KiB lane already had 25 filesystem/storage failures before this change.

The Darwin hosted suite passed with lock tracking off and on. Its spinlock tests
cover IRQ-state restoration, nested and recursive ownership, plain-lock state
preservation, and a real timer callback during nested no-preempt scopes. A ready
peer must remain deferred through the inner release and run during the outer
release, outside any hosted signal frame.

Fresh one- and four-CPU QEMU/TCG boots passed kernel-thread procfs, scheduling
policy, placement, wakeup, and affinity-race checks. Existing lifecycle and
permissions contract failures involving process/task ID assumptions remain
separate. These runs do not establish physical-hardware timing or a throughput
improvement from the policy split.

## One-CPU measurement, 2026-09-19

These historical results used the IRQ-masking implementation then named `Spinlock`.

The RAM-only `which.o` link was compared against `61267bd70`, using the same
`-Os` kernel/module policy, GCC/musl binaries, input files, QEMU configuration,
and normal accounting/interrupt handling. Two fresh boots per version each ran
one warm-up and five measured links. Input identities matched and QEMU block
counters remained unchanged throughout both workloads.

| Metric | Before | Refactor |
| --- | ---: | ---: |
| Host wall time, median | 1.725938 s | 1.704423 s |
| Guest wall time, median | 1.695596 s | 1.579469 s |
| Guest system time, median | 1.375262 s | 1.281622 s |
| Spinlock self instructions, separate trace | 92,560,599 | 44,157,062 |
| Total kernel instructions | 672,648,364 | 630,133,262 |
| `sizeof(Spinlock)`, normal build | 96 bytes | 48 bytes |

The 1.2% host-wall difference is not a demonstrated material linker speedup.
Guest clocks and system accounting are separate observations, not substitutes
for host elapsed time. The byte-validated traces had identical syscall histograms
(64,055 calls) and userspace instruction counts (244,811,919). They establish
52.3% less Spinlock instruction work and 6.3% less kernel instruction work,
not equivalent cycle or time reductions.

The common uncontended acquire/release pair uses 100 -> 47 instructions when
IRQs start enabled, including helper bodies. Locked operations and flags reads
both fall from two to one; release has no helper call or stack frame. Diagnostic
builds deliberately retain the checks and checked unlock.
