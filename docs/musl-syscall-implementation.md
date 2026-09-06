# musl syscall implementation

The amd64 expansion backlog starts at `0867c8927`: 108 untranslated syscall
numbers referenced by the bundled musl source plus five mapped operations with
no successful valid implementation. The [inventory](musl-syscall-implementation.csv)
tracks all 113. A mapping alone does not close an item; its scope and public musl
contract evidence must be recorded.

Current checkpoint: 35 of 113 backlog entries implemented; 78 remain.

Work proceeds by families, starting with IPC, then timers and signal integration,
VM and descriptor-backed objects, file operations, and process/resource features.
Simple queries and aliases follow the larger ownership and blocking contracts.
Linux administration and isolation remain explicit work items; an error-only
placeholder or unconditional success is not implementation evidence.

Each completed pass receives a local commit. Queue, timer, and process-interface
state belongs in POSIX. Shared kernel/VFS changes should express a reusable
lifetime or behavior contract that cannot be implemented correctly inside the
subsystem. Public-wrapper guest tests use fresh headless images, disposable
disks, per-suite exit statuses, and one/four-CPU runs for concurrent behavior.

## Remapping, residency and page discard

The VM pass adds `mremap`, `mincore`, and `madvise(MADV_DONTNEED)`.
Remapping supports shrinking, in-place growth, MAYMOVE relocation, and FIXED
replacement for anonymous memory and ordinary shared/private file mappings.
Source subranges preserve their surrounding mappings, backing offsets, protection
limits, and copy-on-write behavior. Whole contiguous System V attachments can
move or shrink without changing attachment counts.

Preparation allocates replacement metadata, address reservations, page tables,
and a retirement journal before mutation. Commit checks the reservation epoch
and transfers the current PTEs under the address-space mutation lock; detached
references are retired afterwards. Process reservation access now uses serialized
operations instead of exposing its allocators. Fallible container operations and
checked nullable allocation support let preparation report exhaustion without
partially changing mappings.

`mincore` reports residency without faulting pages in, includes cached file pages
and present PROT_NONE mappings, and masks file information for callers who neither
own nor may write the file. Copyout uses bounded kernel snapshots. DONTNEED drops
private changes while retaining the mapping and permissions; shared file and SHM
contents remain in their backing storage.

The initial contract has explicit limits:

- Remapping operates within one managed source object. Zero-size shared mapping
  duplication, DONTUNMAP, direct physical mappings, and unmanaged initial
  address-space mappings are unsupported. SHM growth and fragmented or partial
  source attachments are unsupported.
- Preparation admits at most 65,536 aggregate pages across the new extent and
  complete affected source/victim objects, 4,096 existing objects, and 32
  reservation retries. Capacity exhaustion returns ENOMEM.
- DONTNEED is the only accepted advice. Other advice values return EINVAL.
- Production PTE transfer is implemented for amd64. Hosted builds provide compile
  coverage; other architectures do not gain a remap implementation from this pass.
- Ordinary allocator free-list growth retains the pre-existing fatal allocation
  failure limitation. The remap transaction prepares its release metadata before
  commit. Existing fork/TLS snapshot coherence and the separately parked VM fault
  are outside this pass.

`vm-contract-test` contains bounded remap, residency, and discard families.
The default-off `PEDIGREE_VM_REMAP_TESTS` fixture exercises preparation allocation
failure, stale reservations, a CoW change after preparation, protection transfer,
and exact displaced-page retirement. A standalone native fixture injects actual
allocation failure into the shared metadata containers.

Native verification passed 119 affected utility cases and 38 routing/ABI/fallible
allocation checks. Six affected hosted translation units compiled; the scheduler
regression unit passed C++ syntax checking only because its existing ELF assembly
is not accepted by the Darwin assembler. The remap kernel fixture passed with one
and four CPUs; it is disabled in the ordinary image configuration.

Initial failures are retained under `/private/tmp/pedigree-vm-expansion-20260905/`.
The new CoW fixture undercounted its retained physical-page references and was
corrected. The residency fixture now establishes file mode explicitly because
RamFs creation currently ignores the supplied mode, a follow-up for the file pass.
An existing clock fixture could consume its deadline while creating workers; it
now gates startup and checks the observation window, with failure diagnostics.
The original clock failure lacked timestamps sufficient to prove its exact cause.
Final fresh headless one- and four-CPU ordinary images passed all three VM
families and all 16 integration suites, including per-suite zero exit statuses
and the final marker. `verification.json` in that artifact directory records the
image identity, logs, and checks. Disk writes remained disabled.

## Signal and timer descriptors

This pass adds `signalfd`, `signalfd4`, `timerfd_create`, `timerfd_settime`, and
`timerfd_gettime`. The guest application is `event-descriptor-contract-test`,
with optional `signalfd`, `timerfd`, and `clock` family arguments.

Signal descriptors consume the caller's pending signals without changing its
signal mask. Standard signals coalesce; realtime signals retain ordered values.
They share masks across dup, fork, and SCM_RIGHTS. Reads report complete 128-byte
records; failed scalar or vectored copies leave the failing record pending.
SIGCHLD exit records include trusted child identity and status, and timer signals
carry timer identity, payload, and overrun. Legacy child stop/continue delivery
still lacks typed status metadata; fault-specific siginfo fields are not populated.

Poll observes its calling thread. Epoll retains registration with the process's
signal context, samples the waiting thread's private queue, and survives the
registering thread's exit. An inherited epoll registration does not observe the
child's new signal context; the child must create its own registration.

Timer descriptors support CLOCK_REALTIME and CLOCK_MONOTONIC, relative/absolute
deadlines, periodic expiration counts, and realtime CANCEL_ON_SET. Realtime
changes wake absolute timers immediately; relative deadlines remain monotonic.
Expiration counts and settings belong to the shared open description and survive
fork, descriptor passing, and exec unless the descriptor has CLOEXEC. A failed
readv copy preserves the counter. Disarming retains the configured interval.
There are at most 256 live timer descriptions (ENFILE on exhaustion); alarm and
CPU clock variants are unsupported.

Both descriptor types support ordinary fd flags, FIONBIO, anonymous-inode fstat
metadata, noop lseek, and `/proc/self/fd` readlink names. Closing the final
descriptor wakes a blocked reader with EBADF. This explicit local close contract
differs from Linux's ability to retain an in-flight read after the numeric fd is
closed.

Verification: 37 native routing/ABI checks passed, and the sleep/clock and
thread/signal hosted regression units compiled against the final headers. Fresh
headless one-CPU and four-CPU guests passed all three new families plus 14 existing
suites, with individual exit statuses and final markers. Artifacts are under
`/private/tmp/pedigree-eventfd-expansion-20260905/`; `verification.json` records the
ISO identity and final logs. Initial guest failures exposed missing EBADF and
partial-read errno clearing; those failures and their repairs are retained.
The descriptor-passing fixtures use Unix streams because datagram socketpair
remains an existing networking limitation.

## IPC pass

Implemented and guest-verified: System V shared memory, semaphore sets, message
queues, and POSIX message queues (18 numbers; 95 backlog items remain). Coverage includes creation/lookup permissions,
fork/exit lifetime, removal while referenced or blocked, bounded resources,
interruptible waits, ABI marshalling, and message selection/priority semantics.
POSIX queue notification includes musl's SIGEV_THREAD cookie transport.

The guest application is `ipc-contract-test`; optional arguments select
`messages`, `semaphores`, `mqueues`, or `shared-memory`. Each family runs in a
bounded child and reports its own result. The integration run also exercises the
existing signal/exec, user-copy, futex, descriptor-passing, readiness, and resource
contracts.

The implementation uses finite resources and reports exhaustion. Defaults and
limits are exposed through the corresponding IPC control queries where the ABI
provides them:

- SysV messages: 128 queues; 8192-byte messages; 16384-byte default queue storage,
  configurable up to 65536 bytes with privilege for increases.
- SysV semaphores: 128 sets, 256 semaphores per set, 128 operations per atomic
  vector, and 4096 undo records. Waiters use broadcast/recheck; strict FIFO
  handoff is not promised.
- SysV shared memory: 128 segments, 16 MiB per segment, 64 MiB total, and 128
  explicit attachments per process. HugeTLB is unsupported. SHM_LOCK pins pages;
  per-user RLIMIT_MEMLOCK accounting remains part of the resource/VM expansion.
- POSIX queues: 64 live queues, up to 128 messages of 8192 bytes per queue. The
  AF_NETLINK endpoint implements the notification cookie transport required by
  musl; general routing netlink remains outside this IPC interface.

The IPC checkpoint originally limited signals to 1–34. The signal/timer pass
below extends delivery through 64, including POSIX queue notifications.

Verification: the native routing/ABI checks passed 42 tests. Fresh, display-free
QEMU guests with one and four CPUs passed all four IPC families plus 12 existing
regression suites, including per-suite zero exit statuses and the final marker.
Logs and the disposable-image harness are under
`/private/tmp/pedigree-musl-expansion-20260905/` (`ipc-runtime-*`). This is guest
behavioral evidence; no persistence claim is made with disk writes disabled.

Concurrent fork/exit exposed an unrelated procfs shadow-tree race. Removing the
unused PID index lets the existing VFS directory synchronization own publication;
inode allocation is now atomic. The original failure and the subsequent passing
run are retained alongside the IPC notification cleanup regression evidence.

The existing intermittent VM split/unmap investigation remains recorded
separately. Reusing the established shared-file mapping path does not establish
that the earlier fault is resolved.

## Signals, timers and realtime clock

This pass adds `rt_sigpending`, `rt_sigtimedwait`, `rt_sigqueueinfo`, the five
POSIX timer calls, and `clock_settime` (nine numbers). Signal queues retain
standard-signal coalescing and realtime FIFO order, support synchronous waiters
in sibling threads, and preserve a delivery after failed result copyout.
Blocked ignored signals remain available to synchronous consumers. Signal
disposition changes rebind every pending instance without losing its metadata.

Timers support realtime and monotonic clocks, relative and absolute deadlines,
periodic overruns, SIGEV_NONE, SIGEV_SIGNAL and thread-directed delivery,
including musl's SIGEV_THREAD helper. Rearm, delete, target exit and successful
exec invalidate the appropriate queued notification generation. Timers are not
inherited across fork; IDs reject stale and foreign-process references.

Setting realtime updates kernel and vDSO time and wakes absolute realtime sleeps
and POSIX queue waits. Relative waits and monotonic time remain unaffected.
The timer service reevaluates absolute realtime deadlines on its next tick.
The final signed-nanosecond second is converted without premature saturation.

Current bounds and deliberate restrictions:

- Pending queued signals and timer reservations share a fixed limit of 16 per
  real UID. Mutable RLIMIT_SIGPENDING remains in the resource pass. Realtime
  `kill`/`tkill`/`tgkill` report EAGAIN on exhaustion; Linux can instead retain a
  signal without allocating its queued metadata.
- User `rt_sigqueueinfo` accepts SI_QUEUE; kernel timer and queue-notification
  metadata is constructed internally. Existing process-directed rejection of
  musl-private signals 32–34 remains; private thread-directed protocols work.
- Timers have 256 global slots and 64 per-process slots, further constrained by
  signal reservations. CPU-time and alarm/boottime clock variants are unsupported.
- Only privileged realtime clock setting is supported; hardware RTC persistence
  is not implied.

`signal-timer-contract-test` runs bounded `signals`, `timers` and `clock`
families. Native timer arithmetic has six passing cases; 37 routing/source ABI
checks pass. Two affected hosted regression translation units compile. Fresh
one- and four-CPU guests passed all 14 integration suites after the once-only
subsystem cleanup repair, including each suite's zero status and final marker.
Logs are `signal-timer-exit-fixed-*` in the artifact directory below; image
identity and results are recorded in `verification.json`. The backlog now has
27 implemented numbers and 86 remaining. Disk writes remained disabled.

The first one-CPU integration run passed all new families, then failed during
IPC mqueue process exit with a retained spinlock. Review found that final
shutdown repeated blocking subsystem cleanup under the process spinlock;
thread cleanup now has one admission per lifetime. A diagnostic retry did not
reproduce the fault, so its exact retained-lock identity was not captured.
The original failure and diagnostic run remain under
`/private/tmp/pedigree-signal-timer-expansion-20260905/`; the lock-checker message
now includes the retained lock and acquisition caller for future attribution.
