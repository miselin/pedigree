# Go syscall audit

This audit compares the Go 1.26.5 `linux/amd64` runtime selected by the local
`pedigree/amd64` port with Pedigree's `linuxCompat` syscall path. It is a
source-level compatibility check, not a claim that concurrent behavior has
been exercised.

Status meanings:

- **Ready**: the Linux syscall is translated and its inspected behavior
  matches what this part of Go needs.
- **Fallback**: the syscall is absent or incomplete, but Go has an explicit
  recovery path.
- **Partial**: the entry point exists, but its semantics are narrower than
  Linux or still need runtime coverage.
- **Blocked**: the syscall is absent or its current semantics violate a Go
  runtime requirement.

## Runtime and standard library

| Area | Go's Linux requirement | Pedigree source result | Status | Consequence |
| --- | --- | --- | --- | --- |
| Syscall return ABI | Success in `RAX`; failures as `-errno` | The `linuxCompat` dispatcher converts the thread error to `-errno` in `src/system/kernel/core/processor/x64/SyscallManager.cc` | Ready | Go's assembly error checks have the expected shape. |
| Static ELF startup | Enter the executable entry point directly when no `PT_INTERP` is present, with Linux-shaped `argc`, `argv`, environment and auxv | The local loader patch avoids loading an `ET_EXEC` twice and enters its own entry point; Pedigree builds an amd64 auxv in `PosixSubsystem.cc` | Ready | A static Go ELF can enter `_rt0_amd64_pedigree`. |
| `AT_RANDOM` | Sixteen unpredictable bytes in the auxv | On x64, the local loader fills the entry from `RDRAND` with an `RDSEED` fallback. If neither instruction produces all sixteen bytes, it reserves the same stack space but publishes `AT_IGNORE` instead | Partial | Deterministic zero bytes are no longer advertised. Hardware availability and output still need native coverage, and there is no software entropy pool. |
| `arch_prctl` (158) | `ARCH_SET_FS` and `ARCH_GET_FS` for Go TLS | `posix_arch_prctl` stores and retrieves the current thread's TLS base | Ready | Initial-thread TLS has the required primitive. |
| `mmap` (9), `mprotect` (10), `munmap` (11) | Anonymous private arenas, inaccessible reservations and guard pages, permission changes, fixed replacement, unmapping | All three are translated. The local patch fixes the zero-valued `PROT_NONE` test, so reservations and guard pages are actually inaccessible. Anonymous shared mappings are rejected, and non-`MAP_FIXED` address hints can still replace existing mappings | Partial | The ordinary Go heap primitives are present, but Linux address-selection and replacement semantics still need runtime coverage. |
| `brk` (12) | Querying the current break; expansion if used | `posix_brk` queries and expands but refuses to shrink | Partial | Sufficient for Go's startup query; not a full Linux implementation. |
| `madvise` (28) | Reclaim unused heap pages and memory hints | No translation. Go retries `MADV_FREE`, then `MADV_DONTNEED`, then a fixed anonymous `mmap` | Fallback | Not a startup blocker if fixed anonymous replacement works; memory accounting and reclaim quality suffer. |
| `mincore` (27) | Auxv fallback probing | No translation. Pedigree supplies auxv at exec | Fallback | Not needed on the inspected static-exec path. |
| `gettid` (186) | Stable per-thread identity | The local handler always returns the Pedigree thread ID, so the main thread's value cannot change when `clone` adds another thread | Partial | This meets Go's within-process stability and uniqueness requirement. Pedigree thread IDs are process-local rather than globally Linux-unique, and the path still needs native thread-creation coverage. |
| `clone` (56) | Shared-address-space threads, TLS, and a child stack | `CLONE_VM` creates a Pedigree thread in the same process, installs the supplied stack and TLS, inherits the caller's signal mask, and returns a thread ID. A new thread starts with its alternate signal stack disabled. Several Linux flags are otherwise only advisory or ignored | Partial | Structurally sufficient to attempt Go `M` creation, but lifecycle details still need native coverage. |
| `set_tid_address` (218) | Kernel clear/wake registration on thread exit | The translator returns success without registering the address | Partial | Go does not pass `CLONE_CHILD_CLEARTID` in its normal amd64 runtime clone path, but general Linux binaries cannot rely on this result. |
| `futex` (202) | Atomic compare-and-sleep, wake counts, and optional relative timeouts | The local implementation serializes value checking, enrollment and wakeup; the scheduler releases the futex lock only after marking the waiter sleeping. Event enqueue/wakeup and the scheduler's final event check/transition to sleep now share the thread lock, closing the timeout-event lost-wakeup window. It queues every waiter, scopes private keys by address space plus virtual address, removes empty queues and uses a Pedigree alarm for relative timeouts. Shared futexes, realtime waits and operations beyond private `WAIT`/`WAKE` return `ENOSYS` | Partial | The operations Go normally uses now line up by source inspection and compile successfully. Native SMP/timer stress is still required; a non-timer interruption during a timed wait can currently be reported as `ETIMEDOUT` rather than `EINTR`. |
| `sched_getaffinity` (204) | Discover available CPUs | No translation; Go returns one CPU when the call fails | Fallback | The runtime deliberately falls back to `GOMAXPROCS=1`. |
| `sched_yield` (24) | Yield the current OS thread | Translated to Pedigree's scheduler yield | Ready | The primitive exists. |
| `clock_gettime` (228) | A monotonic clock for `nanotime` and realtime for wall time | The local syscall and vDSO implementations distinguish `CLOCK_REALTIME` from `CLOCK_MONOTONIC`; realtime uses the epoch clock and monotonic uses Pedigree's nanosecond tick count. Both normalize seconds/nanoseconds and reject unsupported IDs | Ready | The source-level clock contract is now correct for Go; actual progression and suspend behavior remain runtime checks. |
| `nanosleep` (35) | Sleep until the interval elapses or return an accurate remainder | Pedigree delays for the interval, is not properly interruptible, and copies the requested duration into the remainder even after success | Partial | Basic sleeps can work; interruption and remaining-time semantics are wrong. |
| `timer_create` (222), `timer_settime` (223), `timer_delete` (226) | Per-thread CPU profiling timers | No translations | Partial | Ordinary timers do not use these calls, but CPU profiling is unavailable. |
| `rt_sigaction` (13) | Linux amd64's 32-byte kernel structure and Linux delivery frame | The local adapter stores the handler, flags, restorer, and full 64-bit mask for signals 1 through 31. The synchronous x64 fault path consumes the stored `SA_RESTORER`, mask, and relevant flags. Signals 32 through 64 are still reported as default because Pedigree has only 32 native slots | Partial | Startup probing and synchronous custom dispositions line up. Asynchronous delivery, realtime signals, `SA_RESETHAND`, and `SA_RESTART` remain incomplete. |
| `rt_sigprocmask` (14) | Per-thread signal masks, including around `clone` | The local handler implements an eight-byte kernel mask per thread, validates reads and writes, inherits it across thread creation, queues blocked standard signals, and forces `SIGKILL` and `SIGSTOP` unblocked. Synchronous delivery snapshots and restores the mask through `ucontext` | Partial | The source contract needed around `clone` and synchronous nesting is present, but asynchronous delivery and wakeup behavior still need native concurrency tests. |
| `sigaltstack` (131) | A separate alternate stack for every OS thread | The local handler implements per-thread query, set, and disable operations and reports `SS_ONSTACK`. Synchronous x64 fault delivery builds and bounds the Linux frame on the alternate stack, including nested delivery | Partial | The Linux-shaped synchronous path is implemented and cross-builds, but has not recovered from a deliberate fault inside Pedigree. Asynchronous event delivery is still native. |
| Synchronous signal delivery and `rt_sigreturn` (15) | Handler arguments `(sig, *siginfo, *ucontext)` plus a Linux restorer frame | For x64 Linux-ABI processes, custom synchronous fault dispositions now receive a 440-byte Linux frame plus aligned legacy FXSAVE state. `rt_sigreturn` validates user bounds, selectors, flags, alternate-stack state, mask, and FPU image before returning through `iretq` | Partial | The frame and return path are source- and cross-build-verified, not executed inside Pedigree. AVX upper state is not preserved, and bad frames terminate rather than generating a fully Linux-compatible secondary signal. |
| Asynchronous signal delivery | Linux frames for process-, thread-, and timer-directed signals | Pedigree's generic event path still calls its native trampoline with a serialized Pedigree buffer rather than the Linux amd64 frame | Blocked | Go cannot safely use signal-based asynchronous preemption or profiling. The local Go patch keeps async preemption disabled. |
| `tgkill` (234) | Target a particular runtime thread with a signal | No translation | Blocked | Async preemption and several self-signal paths are unavailable. It must not be enabled before Linux-compatible signal delivery exists. |
| `exit` (60), `exit_group` (231) | Terminate one thread or the whole process | Both are translated and dispatched separately | Ready | The required exit paths exist. |
| `epoll_create1` (291), `epoll_ctl` (233), `epoll_pwait` (281), `eventfd2` (290) | Go's Linux network poller and its wakeup descriptor | No translations. The local Go target excludes `netpoll_epoll.go` and selects a Pedigree-specific backend instead | Fallback | Direct Linux epoll users remain unsupported, but Go timers and networking no longer select these missing calls. |
| `poll` (7), `select` (23), pipe (22) | Descriptor readiness plus a way to wake a blocked runtime poll | The local Go backend uses level-triggered `poll` and a nonblocking `pipe2` wakeup. Pedigree polling now preserves absolute timeout deadlines across spurious wakes, reports socket errors even without requested events, returns `POLLNVAL` per bad descriptor, and observes socket EOF and readiness | Partial | The Go poller and complete standard library build successfully. Timer wakeups, descriptor races, and packet I/O remain native runtime gates. |
| `pipe2` (293) | Atomic close-on-exec/nonblocking pipe creation | The local translator and POSIX handler validate `O_CLOEXEC`/`O_NONBLOCK`, initialize both descriptors before publishing their numbers, and retain plain `pipe` as flags zero | Partial | The Linux entry point is source- and compile-verified, but blocking and descriptor inheritance behavior have not been exercised. |
| Socket calls | `socket`, `connect`, `bind`, `listen`, `accept`, send/receive calls and socket options | The Linux numbers are translated and dispatched to Pedigree's POSIX socket layer. The local patch accepts creation flags, propagates later `fcntl(F_SETFL)` nonblocking state, maps the basic Linux socket options and `TCP_NODELAY`, consumes `SO_ERROR`, preserves explicit local binds, transfers data received before accepted-descriptor registration, and reports normal FIN as EOF | Partial | The source path now covers Go's basic nonblocking TCP contract and exact lwIP error mapping. IPv6, address/message parity, descriptor races, and real packet I/O still need focused runtime coverage. |
| AF_UNIX stream sockets | Pathname listeners, nonblocking connect/accept, readiness, peer credentials, bidirectional bytes, EOF, and broken-pipe reporting | The local implementation has shared connection-owned streams, atomically queued accepts, direction-specific poll waiters, strong pathname lookup references, identity-safe close/unlink/rebind handling, `SO_REUSEADDR`, `SO_TYPE`, `SO_ERROR`, `SO_PEERCRED`, and `/run` mapping. A hosted buildutility harness covers the Go-shaped flow plus pending data at close, listener teardown, rebind, and 200 connect-versus-close races under AddressSanitizer | Partial | The Pedigree implementation passes native host-memory and lifetime coverage, but abstract addresses, half-close, rename, normalized address fidelity, datagram parity, and Pedigree-scheduler interleavings remain incomplete. |
| `accept4` (288) | Accept with nonblocking and close-on-exec set atomically, truncating the peer address to the caller's capacity while reporting its full size | The local translator and handler validate the flags, stage the peer address in kernel memory, copy only the caller-provided capacity, and initialize the accepted descriptor before publishing it; plain `accept` delegates with flags zero | Partial | The normal Go entry point now exists, but end-to-end nonblocking accept remains a runtime gate. |
| `openat` and `*at` file calls | Modern Linux filesystem access | `openat`, `newfstatat`, `mkdirat`, ownership, unlink, rename, link, symlink, readlink, chmod and access variants are translated | Partial | The ordinary file base is broad, but Linux flag and metadata fidelity need tests. |
| `/proc/self/exe` | Go's Linux implementation of `os.Executable` | No `self/exe` entry was found in Pedigree procfs | Partial | `os.Executable` returns an error; this is not a runtime-startup blocker. |
| `getrandom` (318) | Kernel-provided cryptographic entropy | The local translation accepts the Linux flags Go uses and returns x64 `RDRAND` output with an `RDSEED` fallback. `/dev/random` and `/dev/urandom` use the same hardware path. Unsupported hardware or exhausted retries return `EAGAIN`; the deterministic kernel LCG is not used | Partial | The known deterministic source is removed from security-sensitive reads, but there is no software entropy pool and no native availability, health, or TLS validation yet. Do not connect this node to a real control plane. |

## What the audit proves

The port is feasible, but the current boundary is narrower than "Go works":

- The compiler target, Linux syscall register ABI, static ELF entry, TLS,
  heap primitives, files, and a substantial socket layer line up.
- A minimal single-process executable is close enough to justify continuing
  the port.
- Private futex `WAIT`/`WAKE`, monotonic time, and synchronous amd64 fault
  delivery now have source-level repairs. A correct general Go runtime is still
  blocked by asynchronous signal semantics and needs scheduler race coverage.
- Timers and network I/O now select the local `poll` backend and the supporting
  socket semantics are materially closer to Go's contract. AF_UNIX has
  AddressSanitizer-backed host coverage, but no packet or timer has been
  exercised inside Pedigree.
- Security-sensitive reads now avoid the deterministic LCG, but hardware-only
  entropy without native availability and health coverage is not yet a
  production security claim.

The source audit cannot prove scheduler interleavings, wakeup races,
synchronous fault recovery, asynchronous signal delivery, timer progression,
driver behavior, or packet I/O. Those eventually need runtime tests. QEMU was
intentionally skipped here; source inspection and cross-builds are sufficient
to expose the remaining ABI boundary, but not to claim runtime compatibility.

## Fix order

1. Prove deliberate synchronous fault recovery through the new Linux amd64
   frame and `rt_sigreturn`, including alternate-stack, nested, and FPU cases.
2. Implement Linux frames for asynchronous delivery and `tgkill`; only enable
   asynchronous Go preemption after targeted stress.
3. Stress private futex waits, monotonic clocks, timer deadlines, and the
   `poll` wakeup path on native Pedigree.
4. Exercise DNS, TCP, EOF/error transitions, and the remaining IPv6 and
   address/message semantics.
5. Validate hardware entropy availability and health, then gate TLS and mTLS;
   add a defensible software pool or failure policy for unsupported hardware.
6. Fill in quality and optional features such as CPU affinity, `madvise`, and
   per-thread profiling timers.

## Kubelet-specific gap

Kubelet should be built with `GOOS=pedigree`. The local Go port aliases Linux
build constraints and `_linux.go` filenames, and a full current kubelet compile
confirmed that its Linux and `x/sys/unix` implementations are selected. Using
the real target retains the Pedigree entry point and disables unsafe
signal-based asynchronous preemption. A local kubelet identity shim must still
advertise the Kubernetes node OS as Linux at the few exact `runtime.GOOS`
reporting and Pod-admission sites.

Pedigree currently has no Linux translations for `unshare`, `setns`, or
related namespace operations; no cgroup controller model was found; and its
`mount` handler supports only procfs and tmpfs rather than Linux bind/mount
namespace semantics. The tree also has no Linux netlink/veth/firewall control
plane for a conventional CNI implementation.

That makes the useful first Kubernetes milestone a tainted, unschedulable
heartbeat-only node that bypasses normal CRI initialization, followed by a
small Pedigree-native CRI service for one trusted preloaded process.
Containerd/runc and ordinary CNI are later architectural projects, not the
first Go-port test.
