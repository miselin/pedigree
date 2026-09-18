# Syscall framework performance

The current investigation isolates the cost of one million raw `getuid`
syscalls. The handler still reads the process's real UID; the benchmark is not
a constant-return implementation. Compilation performance is outside this pass.

## Dispatch contract

`SyscallHandler::canRunWithInterruptsDisabled()` defaults to false. The POSIX
handler opts in only `getuid`, `getpid`, and `gettid`, for the Linux and Pedigree
ABIs. `getuid` additionally requires a `PosixProcess`, whose published real UID
can be read atomically. Instrumented or verbose syscall configurations retain
ordinary dispatch.

An eligible callback must finish in bounded time without enabling interrupts,
allocating, taking locks, touching user memory, blocking, dispatching events,
changing TLS or the return frame, reporting interruption, or requesting a
post-syscall action. Its eligibility check has the same restrictions and must
have no side effects. Neither may read or copy uncaptured selector/base
metadata. Extending this allowlist requires checking the entire callback path.

The x64 manager keeps interrupts masked for those callbacks and retains error
conversion, time accounting, and pending user-return work. This avoids creating
the termination-deferral and post-action machinery required by blocking
callbacks. Other calls retain the complete existing dispatch path.

The manager also defers capturing user selectors and FS/GS bases. A query
without pending return or affinity work leaves them untouched. Before return
work can enable interrupts, switch threads, or expose the frame to signals or
tracing, the manager captures the metadata and uses the ordinary restore path.
See [x64 syscall entry](x64-syscall-entry.md) for the assembly invariants.

## Timing and accounting

Measurements below were taken on **2026-09-17**. All use the same static benchmark
ELF, one vCPU, QEMU 11.1.1 TCG, `q35`, 4 GiB RAM, and
`SandyBridge,-rdrand,-rdseed`. Values are the benchmark's own monotonic elapsed
time for one million calls, without tracing enabled.

| Revision / experiment | Elapsed seconds |
| --- | ---: |
| Starting implementation (`65558e329`) | 2.071356 |
| Bounded callback route with corrected accounting clock | 1.189355 |
| Direct accounting transitions | 1.015061 |
| Deferred selector/base metadata | 0.807890, 0.840663, 0.827553 |
| Retained implementation, including exact x64 clock conversion | 0.718402, 0.686924, 0.721727 |
| Linux 3.2.78 reference, same benchmark ELF | 0.124384, 0.125294, 0.124515 |

The dispatch pass's median is 0.718402 s: about 2.9 times faster than
the starting point and still about 5.8 times slower than this Linux reference.
The baseline and intermediate rows before deferred metadata are single trials.
Linux ran the benchmark
in initramfs; Pedigree used its existing guest fixture. The CPU settings and
benchmark bytes match, but the kernels and surrounding environments differ.

Earlier zero user/system times were an accounting defect. The fast clock read
the RTC cursor, which advances in its worker after the measured thread switches
out. A running thread's syscall transitions therefore repeatedly saw a clock
that had not advanced. Accounting now samples ordered TSC against the local CPU
anchor. The x64 conversion uses guarded 128/64 hardware division, preserving
the exact quotient and saturation behavior without the general division helper.
The enclosing benchmark process reported 0.362–0.381 s user and 0.357–0.368 s
system. These include work outside the inner loop. The split follows the
existing C++ accounting boundaries: entry and exit overhead can still be charged
to user time, so it is not an instruction-accurate user/kernel attribution.

The dispatch-pass trace contains eight uninterrupted calls of 710 instructions each,
down from 1,076 at the starting commit. All return UID 0 and all observed bytes
match the frozen kernel and modules. Each call has two SWAPGS, two RDTSC, four
LOCK-prefixed additions and 40 CALLs. No RDMSR, WRMSR or REP initialization executes
on this clean query path. Pending work still uses full metadata capture/restore.

Accounting occupies 380 of those 710 dispatches (53.5%), including clock reads,
mode transitions and thread/process counter publication. The accounting pass
below reduces this cost further. Replacing thread counter
atomic additions with load/store is unsafe: NMI accounting can nest despite
masked IRQs and its update would be lost. Any batching or deferred publication
design must preserve resource-usage reads, CPU timers, migration and exit totals.

Instruction traces explain executed paths, not cycles or elapsed cost. A
serializing MSR instruction and an ordinary register operation each count as a
dispatch, while REP/string-operation dispatch counts depend on translation and
instrumentation. Do not use their shares as a time profile or infer the cost of
a whole copy from its REP count. Compare clean complete syscall captures
separately from captures containing interrupts or scheduling.

## Accounting pass

The follow-up starts at `d64dc8792`, with the same benchmark ELF, QEMU settings,
and accounting semantics. The retained changes are:

- Sample the timestamp and logical CPU identity together, sharing the local
  processor lookup and anchor. The generic timer implementation preserves the
  existing sequence for other machines.
- Inline the small baseline and clock-anchor helpers on the accounting path.
- Update each x64 thread counter with one unlocked memory `ADD`. IRQ masking and
  scheduler ownership exclude writers on another CPU; one instruction also
  prevents a nested NMI from losing an increment between a load and store.
  Aligned counter reads remain atomic. Process counters retain locked additions
  because several threads can update the same process concurrently.

This does not change accounting frequency, defer process totals, or alter
rounding. It also does not repair the preexisting possibility of an NMI nesting
inside a baseline update; the single-instruction counter update is not a claim
that the entire accounting path is NMI-safe.

| Accounting experiment | Million-call elapsed seconds | Median |
| --- | --- | ---: |
| Unchanged starting kernel | 0.729702, 0.709125, 0.713291 | 0.713291 |
| Retained changes | 0.677917, 0.656262, 0.651691, 0.645112, 0.637557 | 0.651691 |
| Rejected exact reciprocal conversion | 0.736010, 0.769728, 0.757491 | 0.757491 |

The retained median is **8.6% lower**, still about **5.2 times** the earlier Linux
reference. The last two candidate/control pairs were interleaved after the
reciprocal experiment to check that its regression was not simply host drift.
The enclosing process's median wall/user/system times change from
0.770625/0.372816/0.365677 s to 0.711602/0.357224/0.319797 s. These include setup
and teardown; the existing attribution-boundary limitation still applies.
The reciprocal cached a Q64 multiplier and corrected its quotient exactly;
native tests passed, but the extra arithmetic was slower in this QEMU workload.
Its source changes were removed, and guarded hardware division remains.

Eight uninterrupted final captures each execute **630 instructions**, including
**300 in accounting** (47.6%). The accounting ranges in capture 1 are 127–276 and
443–592. Each call now executes 28 CALLs and two locked additions, down from 40
and four. The two ordered TSC samples and two divisions remain. These are
instruction counts, not accounting's fraction of elapsed time.

The final retained implementation passes 32 native clock/accounting/timer tests
and all 14 guest suites on one CPU. On four CPUs, query correctness, resource
usage, the new concurrent-accounting contract, clock conversion, and CPU interval
timers pass before the broader suite stops in `timers`. The unchanged starting
kernel with the same fixture also stops, later in `wait-restart`. Its sampled
stacks show the debugger polling for input while the other CPUs are paused;
the candidate's final registers show a consistent stop signature. This proves
that the debugger-stop failure class predates this patch, not that both runs
have an identical initiating fault. Neither full four-CPU run passed. Forced
migration and NMI injection remain untested.

The new contract checks live process totals against snapshots from four active
threads, retained totals after join, sleep exclusion, repeated CPU timer delivery
in a query-only loop, and cross-thread timer arming. Process publication remains
eager: batching only until the next caller-side `getrusage` would miss remote
readers and change the baseline when another thread arms a CPU timer.

The next pass below tests process-owned per-CPU counters and aggregation at
every existing reader, including the cost of looking up the owning CPU.

Artifacts are under `/private/tmp/pedigree-accounting-phase-20260917`:
`candidate/` contains frozen payloads, timings, the trace and Callgrind output,
and both guest runs; `baseline1` and `control-recheck-*` hold baseline timings;
`candidate-recheck-*` hold the interleaved candidate timings. `reciprocal/`
preserves the rejected diff, binaries, tests and runs. `control/contracts-4cpu`
preserves the failed starting-kernel control and paused stack samples.
The final rebuild matches the frozen kernel, debug kernel and initrd hashes;
kernel/initrd payloads read back from both guest images match as well.

## Per-CPU process accounting

Starting at `6d11f09de`, each process created after CPU initialization gets an
immutable array of user/kernel counters indexed by the current dense CPU index.
Each slot occupies a separately aligned 64-byte region. Publication keeps IRQs
masked and uses a single unlocked x64 memory `ADD`; other architectures and
native tests use atomic additions. The dense index is distinct from firmware
processor IDs, which need not be contiguous.

The existing atomic totals remain the fallback for unavailable storage or an
out-of-range CPU index. Bootstrap processes permanently use that fallback: an
AP NMI before initialization completes can still see shared BSP identity/state,
so treating that early index as exclusive would be unsafe. Storage never grows,
moves, or transfers totals. Reads sum all persistent slots plus the fallback,
and timer-report notification remains after each update. Fork creates zeroed
counters; exec retains them; exited threads need no retirement transfer.

This moves work to the less frequent usage reads, which now cost O(allocated
CPUs). Shard storage requests `64 * CPUs + 63` bytes per process, in addition to
the helper's bookkeeping and allocator overhead. It adds no allocation or lock
to the syscall accounting path.

Three uninstrumented runs per arm use the same fixture ELF and QEMU settings.
The parallel fixture has four pthreads each issuing one million raw `getuid`
calls in one process; its timed interval covers the start barrier through joins.

| Workload | Starting median | Per-CPU median | Result |
| --- | ---: | ---: | --- |
| One CPU, one million calls | 0.642347 s | 0.638260 s | Essentially unchanged |
| Four CPUs, four concurrent million-call loops | 1.123212 s | 0.977760 s | 13.0% less wall time |

One-CPU starting trials are 0.642347, 0.643512 and 0.633459 s; candidate trials
are 0.622335, 0.638260 and 0.650722 s. Four-CPU starting trials are 1.172412,
1.104888 and 1.123212 s; candidates are 0.977760, 0.973071 and 1.083064 s.
The one-CPU distributions overlap, so this is a scaling improvement rather than
evidence of a meaningful single-core speedup. The enclosing parallel process's
median user/system times fall from 2.210680/2.171158 s to 1.933187/1.916566 s.

Eight clean traces contain **675 instructions each and zero LOCK-prefixed
instructions**, versus 630 and two before. Accounting occupies 345 instructions
(capture-1 ranges 127–299 and 466–637). The extra dense-CPU lookup raises the
instruction count, illustrating why dispatch counts cannot predict elapsed
cost. Ordered TSC sampling and conversion remain unchanged.

All **36 native accounting/clock/timer tests** and all **14 guest suites on both
one and four CPUs** pass. Coverage includes live cross-thread aggregation,
sleep exclusion, CPU timers, fresh fork totals, preservation across exec,
wait/reap snapshots, and signal interruption/restart behavior. Native shard
tests cover wide values, every allocated slot, invalid indices, and concurrent
writers/readers. The x64 unlocked-add path is exercised by the guest tests.
Forced migration and NMI injection remain untested; passing these runs does not
resolve the earlier intermittent SMP debugger stops.

Artifacts are under `/private/tmp/pedigree-percpu-accounting-20260917`, with
per-run commands, logs and measurements under `control/` and `candidate/`.
`final/summary` contains the final trace and Callgrind output;
`final/contracts-{1,4}cpu` contain final contract results. A formatting-only
rebuild was rechecked with both guest suites and the trace. Installed payload
hashes match the frozen final build, and the checked-in parallel fixture builds
byte-for-byte identically to the ELF used for the measurements. The first
contract run is retained: it failed because the new self-exec fixture searched
PATH for a bare filename present only in its working directory. The fixture now
tries that path directly before PATH lookup. The retained parallel fixture is
[`parallel-getuid.c`](../scripts/benchmarks/parallel-getuid.c).

## Reusing accounting setup: rejected experiments

Starting from `3365263e5`, this pass tested reusing setup across the existing
IRQ-masked getuid/getpid/gettid window. Both accounting boundaries retained
fresh ordered TSC reads, the exact nanosecond conversion, immediate thread and
process totals, and live CPU-timer publication checks. The cached state ended
before pending work, IRQ enabling, or affinity handling.

All timings below are one CPU, one million calls through the unchanged getuid
ELF, using uninstrumented QEMU runs and disposable overlays. Traces were separate.

| Variant | Runs | Median inner elapsed | Clean instructions per call |
| --- | ---: | ---: | ---: |
| Original, including rebuilt control | 8 | 0.621266 s | 675 |
| Cache timer, CPU identities and clock anchor | 2 | 0.813936 s | 633 |
| Same cache, redundant compiler fill suppressed | 3 | 0.724096 s | 629 |
| Cache only dense CPU counter index | 5 | 0.627635 s | 644 |

The five initial controls had a 0.614998 s median. Three later controls rebuilt
from restored source had a 0.657250 s median; their kernel, debug ELF and initrd
are byte-for-byte identical to the initial control. Original timings span
0.610192–0.817704 s, and index-only timings span 0.624789–0.818736 s. Preserve
those outliers: the index-only change has no reliable elapsed-time benefit.
Neither cache implementation is retained.

The first implementation returned a fully initialized 40-byte aggregate.
Despite that initialization, GCC's hardened build inserted a `REP STOSB`
prefill before the factory call. One trace dispatch therefore concealed a
repeated memory operation. A local `uninitialized` variable attribute removed
only that redundant compiler fill while leaving the factory initializer intact;
the second trace proves the REP is absent. This variant still measured slower
than the original. The attribute and all experimental APIs were reverted.

The wider cache removed two calls per syscall (30 to 28), but its 629-instruction
path did not improve wall time. Guest instruction counts establish executed
work, not its cost under TCG. The remaining regression was not causally isolated;
it must not be attributed solely to a particular instruction or cache effect.
The index-only trace has seven clean 644-instruction captures and one capture
with interrupt/address-space activity, which is excluded from the clean count.

Enclosing original process accounting has median user/system times of
0.332037/0.318194 s. For the cache without REP these are 0.413601/0.335530 s;
for index-only, 0.345526/0.314263 s. They describe the enclosing benchmark process,
not just the inner loop; moving setup before the entry sample also changes
which mode receives that overhead. Inner wall time is the acceptance metric.

All 36 native accounting/clock/timer tests pass, including after restoration.
No new guest contract or four-CPU correctness claim is made for these rejected
patches. One index-only run reported zero elapsed time because restoring files
with old modification times left mixed Timer vtable layouts in incremental
objects. That invalid run is preserved and excluded; touching every restored
clock source/header and rebuilding kernel plus modules fixed the build mismatch.
Future restores must update modification times or explicitly rebuild consumers.

Artifacts are under `/private/tmp/pedigree-accounting-context-20260917`:
`candidate/`, `prepared/`, and `index/` contain frozen payloads, source diffs,
run logs and traces; `control/` and `restored/` contain original timing runs.
`measurements.json` includes every timing, including the invalid run flagged
separately under `index-stale-objects/`. `identity-verification.txt` records
installed payload readbacks, exact restored-control identity, and unchanged
unrelated SLAM edits.

## POSIX dispatch: conversion versus generic dispatch

The original getuid trace spends 74 self instructions in
`PosixSyscallManager::syscall`. Only eight implement Linux-number to native-POSIX
number translation. Twenty surround eagerly extracting all six arguments,
although getuid needs none; 17 are the large function's prologue/epilogue and
14 are the second switch plus handler call. These are instruction counts,
not elapsed-time shares. ABI-personality bookkeeping adds helper calls as well.

Target musl already issues raw Linux syscall numbers. Linux service 0 and
historical Pedigree service 1 are both externally used, with different register
conventions and some different semantics. There is no userspace/kernel double
renumbering to remove. Genuine adapters for signals, clone/TLS, task IDs,
resource structures, exit behavior and errors must remain.

The retained ABI dispatch pass lets each external ABI select the shared
operation body directly. The existing Linux mapping definitions now generate
switch cases that jump to the matching operation label, eliminating translation
to an intermediate number and redispatch. Native numbers still select the same
bodies. An inline accessor extracts arguments only where they are consumed;
TLS and exit operations retain local snapshots where needed. All 287 mappings
and 319 operation bodies were audited against the original source. Musl,
personality setup, real ABI adapters, accounting and assembly entry are unchanged.

Eight clean getuid captures each execute 638 instructions, down from 675
(5.5%). POSIX dispatch falls from 74 to 37 instructions, with no argument loads
on the getuid path. Observed instruction bytes match the frozen payloads.
The large dispatcher prologue remains, as do 30 CALL/RET pairs across the full
path. The register frame is still saved at entry; this change removes unnecessary
extraction and redispatch after that save.

Uninstrumented one-CPU QEMU 11.1.1 TCG results, using the unchanged benchmark ELF:

| Loop size | Repetitions per build | Original median | New median |
| --- | ---: | ---: | ---: |
| 1 million getuid calls | 5 | 0.614841 s | 0.602661 s |
| 10 million getuid calls | 3 | 6.084874 s | 6.149586 s |

The short batch is 2.0% faster; the longer batch is 1.1% slower. There is no
repeatable elapsed-time improvement. Short-run ranges are 0.605337–0.626150 s
and 0.566641–0.647701 s. Long-run ranges are 6.055387–6.357500 s and
6.054215–8.146384 s; the slow candidate is retained. No timing was excluded.
The longer runs were interleaved, with one guest at a time and no concurrent
builds. Retain the change as a reduction in dispatch work and a simpler route
from external numbers to operations, without claiming a wall-time win.

For one-million-call runs, enclosing median user/system times change from
0.325958/0.313361 s to 0.339986/0.292651 s. For ten-million-call runs they change
from 3.191191/2.849109 s to 3.462707/2.655816 s. These describe the whole process
and should not replace the inner-loop wall measurement. Fewer traced guest
instructions still do not establish lower TCG execution cost.

The new raw-ABI contract passes on both the original and new builds. It checks
the distinct register layouts and errno conventions, unused argument sentinels,
pipe contents, six-argument file mappings with nonzero offsets, unmapped numbers,
fchmodat's unused fourth Linux argument, and global versus native-local thread
IDs. All 15 suites pass on the new build with one and four CPUs: ABI, queries,
resource accounting, concurrent accounting, clock scaling, CPU interval timers,
timers, process lifetime, and seven signal interruption/restart modes.
The separate entry/TLS stress test passes with one CPU; nonzero GS is explicitly
skipped because ARCH_SET_GS is unsupported.

Four-CPU entry/TLS stress remains failing. The candidate reaches the 100-second
deadline with three CPUs in the LAPIC processor-control wait loop. The original
build, running the same freshly compiled fixture and root image, panics with
`Mapping mutation admission timed out` after two workers pass. Both failed runs
and register snapshots are retained as `candidate/entry-4cpu` and
`control/entry-4cpu`. The control confirms this workload already fails without
the ABI changes; it does not prove identical triggers or establish SMP reliability.

The target build and a target translation-unit compile with both verbose syscall
diagnostics enabled pass. The configured hosted compile database is stale and
fails on the original source too; a Clang x86_64 hosted translation-unit check
passes with the host sysroot and GNU feature declarations. This is compile-only
coverage, not a hosted runtime result. Of 43 routing tests, 41 pass. Two stale
source assertions concerning signal-return diagnostics and inotify notification
variables also fail at the starting commit; they are preserved as unrelated
failures. The mapping snapshot was updated for the already-existing clone3 entry.

Artifacts are under `/private/tmp/pedigree-abi-dispatch-20260917`: frozen
`candidate/` payloads, `control/` and `candidate/` timing/contract runs,
`candidate/summary/` trace and Callgrind output, `measurements.json`,
`dispatch-body-comparison.json`, `baseline-routing-evidence.json`, and
`identity-verification.txt`. The latter checks installed payloads, test binaries,
root images, and preservation of unrelated SLAM edits. This pass starts at
`a9a9e3522`; its original payloads are the byte-identical restored build from the
accounting-context experiment above.

## Minimal assembly return: experimental lower bound

A temporary root-only getuid bypass was tested at `0e1687b3c`, before `CLD`,
`SWAPGS`, stack switching or register saves. The matched path disassembles as:

```asm
cmp rax, 102
jne normal_entry
xor eax, eax
sysretq
```

The full-width comparison selects only Linux service-0 getuid. Other calls use
the original entry path. This returns a constant zero and touches no memory,
stack or GS base. RCX and R11 retain the return address and flags established by
SYSCALL; RSP stays at its user value. IA32_FMASK already clears IF/TF/DF, and
SYSRET restores flags from R11. The architectural requirements are described in
the [Intel instruction reference, SYSCALL/SYSRET](https://cdrdv2-public.intel.com/782151/253667-sdm-vol-2b.pdf).

Three uninstrumented one-CPU QEMU 11.1.1 TCG runs per build, same benchmark ELF:

| Path | One million calls, median | Range |
| --- | ---: | ---: |
| Current framework | 0.601623 s | 0.597618–0.608987 s |
| Minimal assembly return | 0.059840 s | 0.058516–0.072194 s |

The bypass is 10.05 times faster, removing 90.1% of elapsed time in this
experiment. All samples are retained. Controls and candidates alternate, and no
builds run concurrently with timing. This is a measured lower bound for this
QEMU workload, not a physical-hardware cycle estimate or a correct getuid result
for arbitrary processes. It confirms substantial cost above bare entry/exit;
it does not apportion that cost among framework components.

A separate plugin run captures eight clean calls, each with exactly five
instruction dispatches: the userspace SYSCALL plus four kernel instructions.
There are no calls, stack accesses, SWAPGS instructions or address-space changes.
All observed instruction bytes match the frozen kernel and benchmark.

The bypass deliberately omits real credentials, accounting boundaries and CPU
timer publication, pending signals/termination/affinity work, and normal dispatch
bookkeeping. Enclosing user/system totals therefore are not comparable. It also
leaves CPL0 running on user RSP: current IDT setup gives only double fault an IST
stack, so NMI/exception safety and canonical SYSRET targets would need proper
handling before considering a general entry path. No NMI injection or production
correctness claim is made for this experiment.

The experimental assembly was restored after capture. Rebuilt kernel, debug ELF,
initrd and configuration are byte-identical to the original control, and unrelated
SLAM edits are unchanged. Artifacts under
`/private/tmp/pedigree-minimal-sysret-20260917` include `candidate/source.patch`,
frozen payloads and images, all six timing runs, `measurements.json`,
`candidate/summary/` (trace and Callgrind output), and
`identity-verification.txt`. Only these findings are retained in the repository.

## Skipping C++ after the assembly frame is saved

A second temporary bypass starts at `2d6949344`. It retains CLD, both SWAPGS
instructions, the kernel-stack switch, all sixteen register/frame pushes, the
32-byte metadata area and saved original syscall number. Immediately before the
C++ call it compares the full RAX with 102. On a match it writes zero to saved
RAX at `[rsp+128]`, sets live EAX to zero (the wrapper's preserve-metadata return),
and rejoins the original CLI, return test, register pops and SYSRET. Other calls
still enter C++. This retains the normal quiet getuid assembly path while
omitting everything reached through its C++ call.

Fresh measurements use three interleaved runs of each build, one CPU, the same
benchmark ELF and QEMU settings, without tracing or concurrent builds:

| Path | One million calls, median | Range |
| --- | ---: | ---: |
| Full framework | 0.589092 s | 0.579423–0.589459 s |
| Bare assembly return | 0.061630 s | 0.058505–0.066326 s |
| Saved frame, skip C++ | 0.078867 s | 0.077884–0.079263 s |

Keeping the full assembly frame adds about 17.2 ms per million calls relative
to the bare return. Skipping C++ still saves about 510.2 ms, or 86.6%, relative
to the full framework (7.47 times faster). These are differences between whole
benchmark medians, including their different branches; they do not isolate an
individual SWAPGS or memory-operation cost. The bulk of this workload's cost is
above the assembly save/restore layer.

Eight separate clean trace captures each contain 52 instructions including the
userspace SYSCALL, versus five for the bare return and 638 for the current full
path. Each framed capture includes two SWAPGS instructions, all saves/restores,
CLI and SYSRET, with no CALL, discontinuity or CR3 change. All 52 observed
PC/byte pairs match the frozen payloads, and every capture returns zero.

This remains a root-only lower bound: it skips real credentials, accounting,
CPU-timer publication and pending return work. Enclosing user/system accounting
is not comparable. No production ABI, SMP or exception-safety claim is made.
The original assembly was restored with a fresh modification time and rebuilt;
kernel, debug ELF, initrd and configuration match the control byte for byte.

Artifacts are under `/private/tmp/pedigree-framed-sysret-20260917`, including
all nine timing runs in `control/`, `bare/` and `candidate/`, `measurements.json`,
`candidate/source.patch`, `candidate/disassembly.txt`, `candidate/summary/`,
`verification.md` and `identity-verification.txt`. Unrelated SLAM edits are
unchanged. Only the findings are retained in the repository.

## Skipping only getuid accounting

A temporary experiment at `5e5dc26c2` skips the paired User-to-Kernel and
Kernel-to-User accounting transitions for quiet Linux getuid calls. Assembly,
handler eligibility, the real UID lookup, errno handling and pending-work checks
remain intact. If return work is pending, the path performs the delayed
User-to-Kernel transition before entering that work, then uses the ordinary
accounting return path. Other syscalls keep their original accounting.

This deliberately charges the skipped kernel interval to user time. Enclosing
user/system totals and CPU-time timer behavior cannot be used as correctness or
performance evidence. The benchmark's inner elapsed time uses CLOCK_MONOTONIC,
independently of process CPU accounting, and remains the comparison metric.

Three fresh interleaved runs per arm, one CPU, unchanged benchmark ELF, no
instrumentation or concurrent builds during timing:

| Path | One million calls, median | Range |
| --- | ---: | ---: |
| Full framework | 0.584242 s | 0.578214–0.593489 s |
| Skip only getuid accounting | 0.363426 s | 0.361896–0.504713 s |
| Assembly frame, skip all C++ | 0.087090 s | 0.081117–0.091405 s |

Skipping accounting saves about 220.8 ms per million calls: 37.8% less elapsed
time, or 1.61 times faster. No samples are excluded, including the 0.504713 s
candidate outlier. In this experiment accounting explains about 44% of the
full-framework excess over the assembly-only baseline. About 276.3 ms remains
between the no-accounting path and that baseline, so accounting is substantial
but does not explain the whole C++ cost. These are differences between medians,
not isolated cycle costs for particular functions.

Enclosing median user/system totals change from 0.327925/0.282795 s to
0.364431/0.030579 s. The apparent system-time reduction includes the intentional
misclassification; it must not be reported as an independent optimization gain.
These whole-process totals also cover work outside the inner measured loop.

Eight clean captures each execute 294 instructions, down from 638. Accounting
functions and both RDTSC instructions disappear; CALL/RET pairs fall from 30 to
18. The real getuid handler and the 23-instruction pending-work predicate remain.
No pending-work service executes in the quiet captures. All 257 observed
PC/byte pairs match the frozen payloads, with no discontinuity or CR3 change.

A temporary version of the query contract issues only getuid in its hot loops.
It passes on one CPU with real UIDs 20001, 20004, 20007 and 20010, errno checks,
17 asynchronous signals, stop/continue and forced termination. This establishes
liveness and real UID behavior for that fixture, not full accounting/timer or
SMP correctness, nor which individual syscall serviced each event.

The original source was restored with a fresh modification time and rebuilt.
Kernel, debug ELF, initrd and configuration match the original control byte for
byte. Artifacts under `/private/tmp/pedigree-getuid-no-accounting-20260917`
include all nine timing runs, `measurements.json`, `candidate/source.patch`,
`candidate/summary/`, `candidate/query-1cpu/`, the query fixture and its source
diff, `verification.md` and `identity-verification.txt`. Unrelated SLAM edits
are unchanged; only these findings are retained in the repository.

## What remains without accounting; constant getuid handler

The eight 294-instruction traces can be partitioned without double counting:

| Work, including its called helpers | Instructions per call |
| --- | ---: |
| Architecture dispatch and direct helpers | 107 |
| POSIX dispatch and ABI personality setup | 50 |
| Userspace SYSCALL plus assembly entry/return | 48 |
| Handler eligibility checks | 39 |
| Pending-return predicate and unwind getter | 25 |
| Actual getuid and its lookup helpers | 25 |
| Total | 294 |

Caller CALL instructions stay with callers; the architecture category includes
the inline affinity check. No pending-work service executes. Actual getuid's
25 instructions comprise its ten-instruction body, CPU information (nine),
current-thread lookup (two), two module jump thunks, and the UID getter (two).
The UID getter is a cached load and return, with no lock or scan.

Four CPU/current-thread lookup pairs occur across eligibility, architecture
dispatch, POSIX personality setup and the handler. Including module jump thunks,
they account for 50 instructions already assigned to the categories above.
The two previously unnamed POSIX locations are jump thunks to CPU information
and current-thread lookup, verified against the frozen ELF's JUMP_SLOT entries.
The path also contains 18 CALL/RET pairs. These are instruction counts, not
elapsed-time shares.

A controlled experiment at `bf5b426e4` replaces only the body of `posix_getuid`
with `return 0`. Both builds use the exact same accounting bypass from the prior
experiment; their kernel, debug ELF and configuration are byte-identical.
Dispatch, eligibility and pending checks remain. Three fresh interleaved
one-CPU runs per build, without tracing or concurrent builds, give:

| Handler, accounting bypassed in both | One million calls, median | Range |
| --- | ---: | ---: |
| Real UID lookup | 0.391921 s | 0.385640–0.409855 s |
| Constant zero | 0.351896 s | 0.347862–0.379523 s |

The constant handler saves about 40.0 ms per million calls (10.2%). All samples
are retained. A separate trace confirms eight clean 271-instruction captures:
the handler is exactly XOR EAX,EAX followed by RET, reducing its subtree from
25 instructions to two. The rest of the 294-instruction partition is unchanged.
CALL/RET pairs fall from 18 to 15, and all 247 observed PC/byte pairs match.
The real UID operation is a small part of the remaining framework cost.

This is a root-only diagnostic, not correct credential handling. User/system
totals remain invalid because both arms bypass accounting. Both experimental
source changes were restored and rebuilt; kernel, debug ELF, initrd and config
match the normal-accounting, real-UID build byte for byte. Artifacts under
`/private/tmp/pedigree-getuid-handler-zero-20260917` include the six timing runs,
`measurements.json`, `breakdown.md`, `breakdown.json`, relocation evidence,
`candidate/source.patch`, `candidate/summary/`, `verification.md` and
`identity-verification.txt`. Unrelated SLAM edits are unchanged.

## Header-inline CPU and process helpers

At `5a800bdbe`, the working-tree experiment moves `ProcessorBase::information`,
`X86CommonProcessorInformation::getCurrentThread`, and `PosixProcess::getType`
into headers. The first build fails because the TSS helper remains defined only
inline in the source file. Completing the relocation moves that helper into the
header, exports the APIC fallback for module callers, and guards the new x86
definitions against hosted inclusion. No accounting or credential ablation is
enabled in this experiment.

Three fresh interleaved one-CPU runs per arm, with no tracing or concurrent
builds, compare the frozen previous payload with the completed header patch:

| Workload | Previous median | Header patch median | Change |
| --- | ---: | ---: | ---: |
| One million getuid calls | 0.625212 s | 0.696693 s | 11.4% slower |
| Ten million getuid calls | 6.340165 s | 7.148185 s | 12.7% slower |

The one-million ranges are 0.603551–0.828386 s and 0.689283–0.704324 s;
the ten-million ranges are 6.108752–6.354499 s and 7.090405–7.199388 s.
All observations are retained. Enclosing ten-million median user/system times
are 3.482552/2.773072 s before and 3.926695/3.150019 s after. These enclosing
measurements include setup outside the inner monotonic benchmark interval.

The clean normal-accounting path falls from 638 to 628 instruction dispatches
and from 30 to 26 CALL/RET pairs. All four current-thread getter calls disappear,
including their three module PLT jumps. `information()` does not inline at
`-Os`: its six calls become three kernel calls at nine self instructions and
three module-local calls at eleven, versus six at nine before. The module
copies add GOT loads. `getType()` remains an indirect virtual call through
`Process*`, despite its visible constant-return body. The count reduction does
not establish the cause of the elapsed-time regression.

Seven captures contain the clean 628-instruction path. The eighth contains
5,483 instructions: its first 628 match the clean path exactly, followed by
an interrupt after SYSRET and 4,855 additional dispatches through vector 0x28
and IRET. Preserve that capture; it does not show interrupt enabling within
the quiet syscall. All 2,471 observed PC/byte pairs validate against the frozen
payload, and all clean kernel dispatches have IF clear.

All fifteen existing contract suites pass on one CPU. On four CPUs, eight
suites pass before `read-eintr` hits `Mapping mutation admission timed out`.
This is a previously observed failure signature, but this pass does not
establish its cause or claim four-CPU correctness. Hosted execution was not
tested. Header formatting and `git diff --check` pass. A final formatting-only
rebuild changes debug data; every non-debug ELF section in the kernel and initrd
modules matches the tested payloads.

Artifacts are under `/private/tmp/pedigree-inline-getuid-20260917`, including
the initial source snapshots, completed `candidate/source.patch`, verified
installed payloads, twelve timing runs, `measurements.json`,
`long-measurements.json`, the full trace and interrupted capture, contract logs,
and `formatting-build-section-comparison.json`. The source experiment remains
in the working tree for follow-up; this is not a retained performance win.

## Kernel GS ownership and inline per-CPU lookup

Starting at `3ccd91eab`, the next pass keeps kernel GS active throughout x64
C++ and scheduling. The BSP installs an anchor before constructors; APs install
their own before entering C++. Syscall entry swaps once to kernel GS and exit
swaps back to user GS. User GS is saved separately across thread switches,
fork, signals, and exec. The [architecture note](x64-kernel-gs.md) describes
the exception windows, IST stacks, and multiarchitecture API boundary.

The control is the completed header-inline experiment above, with its source
and payload frozen before the GS changes. Both arms use normal accounting and
the real UID handler. Three fresh interleaved one-CPU runs per arm, without
profiling or concurrent builds, give:

| Workload | Header-inline control median | Kernel GS median | Change |
| --- | ---: | ---: | ---: |
| One million getuid calls | 0.670840 s | 0.453460 s | 32.4% faster |
| Ten million getuid calls | 7.053152 s | 4.750583 s | 32.6% faster |

One-million ranges are 0.648239–0.715471 s and 0.451653–0.597276 s;
ten-million ranges are 6.838462–7.285850 s and 4.740743–5.967653 s.
All observations are retained. Enclosing ten-million user/system medians are
3.857595/3.124566 s before and 2.840366/1.946764 s after. These process totals
include setup outside the inner monotonic benchmark interval. The results
describe this QEMU TCG configuration, not physical CPU latency.

All eight trace captures contain the same clean 556-instruction path, down
from 628; CALL/RET pairs fall from 26 to 20. All 401 observed PC/byte pairs
match the frozen payload. There is no RDMSR, WRMSR, STR, or information-helper
call on this path. The module's emitted current-thread lookup is:

```asm
movq %gs:0x10, %rax
movq 0x30(%rax), %rax
```

The first load gets the local information pointer; the second gets its current
thread. The second offset is a property of this build, not an assembly ABI.
`PosixProcess::getType()` remains virtual. The count reduction establishes
which work disappeared; it does not assign cycle costs to each instruction.

The entry/TLS contract and all fifteen existing syscall/accounting/signal
suites pass on one CPU. The dedicated GS contract passes with one and four
CPUs, observing a worker on each CPU in the latter. It covers distinct per-thread
GS/TLS/errno, blocking, timer-driven preemption, directed signals, five recovered
user #GP faults, selector changes, fork isolation, and exec resetting GS.
It does not force an individual thread to migrate.

The separate diagnostic build passes nine exact NMI injection boundaries,
including entry before SWAPGS with a user stack, exit after SWAPGS, and the
temporary zero GS base after loading a user selector. Each NMI uses IST2,
observes the same valid CPU anchor, and restores RIP/RSP/CS/GS exactly before
the next injection. The guest's complete canary contract passes afterward.
This is one-CPU, one-NMI-per-boundary evidence; nested NMIs, intervening faults,
and machine checks remain outside its coverage. User #DB is skipped because
the configured kernel debugger owns that vector.

The broader four-CPU entry/mapping stress times out. Captured registers show
three CPUs in `LocalApic::interrupt`'s processor-control pause loop and the
fourth in `ProcessorBase::pause`; all four have distinct valid-looking kernel
GS anchors. Earlier passes also encountered debugger/quiesce stalls, but this
capture alone does not establish the initiating cause. The failed run remains
preserved; the dedicated GS pass does not erase that SMP reliability gap.

The hosted lookup interface compiles with the existing Darwin-hosted flags.
The full hosted kernel build fails in existing Darwin/musl header selection
and context-layout code, so hosted runtime is not claimed. No hosted lookup
implementation was replaced with x64 GS assembly.

Artifacts are under `/private/tmp/pedigree-kernel-gs-20260917`: starting source
snapshots, read-back-verified images, normal and diagnostic payloads, timings,
the symbolized trace and Callgrind file, contract logs, and NMI-window records.
The normal build's diagnostic options are restored to false after validation.
Its code sections and initrd match the timed payload; differences are build
revision/timestamp strings and symbol/debug data. Fresh final-image GS
contracts pass again with one and four CPUs. Existing SLAM and `getType`
working-tree experiments remain unchanged; both timing arms include them.

## Inline accessors and direct accounting clock

Starting at `effa57beb9`, this pass exposes four small accessor bodies to their
callers: x64 `Processor::index()`, and `Thread::getUnwindState()`,
`getStateLevel()`, and `getScheduler()`. The Thread getters retain their acquire
loads; the GS lookup retains its volatile assembly and memory clobber. The
scheduler getter reads the specified thread's assigned scheduler, not an
assumed current-CPU scheduler. Most other simple Thread/Process getters were
already inline. Lazy scheduler creation and virtual POSIX accessors remain
separate from this pass.

The x64 PC implementation of `Time::sampleCpuTime()` now calls the selected RTC
implementation directly. `Pc::getTimer()` always returns that singleton, so
this removes repeated machine lookup and virtual dispatch without changing
the clock. The common Time API remains unchanged and the generic/hosted path
still uses its timer backend. All sample producers now identify the CPU with
`Processor::index()`, matching the permanent x64 GS slot, instead of normalizing
a firmware ID during bootstrap. Clock-anchor readiness remains a separate
check. Calibration, ordered reads, exact conversion, saturation, both boundary
samples, immediate totals, and timer reporting semantics are unchanged.

The emitted process-publication path loses its entire stack frame after index
inlining. The RTC sampler also becomes a leaf without a stack frame: the old
conditional `isInitialised()` call did not execute on CPU zero, but its presence
still forced register preservation on every sample. Removing this possibility
and the firmware-ID normalization saves 16 instructions per sample. Direct
clock dispatch saves another 19 per sample.

Eight clean captures per candidate, with all observed instruction bytes matched
against the frozen kernel/initrd, show:

| Variant | Instructions per getuid | CALL/RET pairs | Accounting instructions |
| --- | ---: | ---: | ---: |
| Kernel GS starting point | 556 | 20 | 317 |
| Inline accessors | 510 | 17 | 281 |
| Accessors + direct clock + permanent index | 440 | 13 | 211 |

The final clock chain is 70 instructions, down from 140. Accounting's remaining
211 instructions comprise 90 in thread transitions, 70 in clock sampling,
14 in thread publication, 23 in process publication, and 14 in report-interest
checks. Both ordered TSC samples and divisions remain. No locked instruction,
RDMSR, WRMSR, PAUSE, or HLT appears in the clean path. Counts describe instruction
dispatches, not elapsed-time shares.

Fresh one-CPU QEMU TCG runs use the unchanged benchmark ELF, normal accounting,
the real UID handler, and independent disk overlays. Kernel and initrd are
rebuilt together, frozen, installed, and read back to verify their hashes.
Timing runs have no tracing or concurrent builds. Three repetitions per cell
give the following inner monotonic elapsed times:

| Workload | Starting median | Accessor-only median | Combined median |
| --- | ---: | ---: | ---: |
| One million getuid calls | 0.500193 s | Not run | 0.414355 s |
| Ten million getuid calls | 4.628801 s | 4.822332 s | 4.342645 s |

The combined medians are 17.2% and 6.2% lower respectively. The million-call
ranges are 0.458606–0.624209 s before and 0.407104–0.428493 s after. Ten-million
ranges overlap: 4.518058–4.851842 s before, 4.569999–4.834726 s for accessors, and
4.299137–5.079062 s combined. Every observation, including the slow combined
repeat, is retained. Inlining alone reduces instructions but does not establish
a wall-time improvement; the integrated result is the retained change. This
does not establish a fixed speedup across workloads or physical CPUs.

For the million-call workload, enclosing wall/user/system medians change from
0.562571/0.287830/0.241932 s to 0.485320/0.275155/0.174426 s. The ten-million
enclosing medians change from 4.702083/2.717281/1.947060 s to
4.406815/2.736300/1.576413 s. These include process setup and teardown outside
the inner benchmark interval; medians of the separate fields need not sum.

All fifteen guest syscall/accounting/signal suites and the dedicated kernel-GS
contract pass on one CPU. The GS contract covers TLS, blocking/preemption,
directed signals, recovered user #GP, selectors, fork, and exec; user #DB remains
skipped because the kernel debugger owns that vector. Forty-one native
clock/accounting/timer tests pass. Actual hosted `Time.cc`, `Timer.cc`, and a
consumer of all four moved accessors compile with the existing Darwin-hosted
configuration; this is compile-only coverage. Four-CPU execution and migration
testing are deferred for this pass.

Artifacts are under `/private/tmp/pedigree-gs-accounting-20260918`, including
starting source snapshots, per-variant source patches, frozen payloads/images,
readback hashes, every timing observation in `measurements.json`, exact trace
summaries/Callgrind files, emitted assembly, and native/hosted/guest logs.
The prior GS final payload is the baseline, not its earlier pre-GS control.
The existing SLAM and POSIX `getType` working-tree edits are unchanged and
included equally in the timing variants.

The direct implementation keeps platform selection out of common public
headers. A future machine-selected accounting-clock implementation can preserve
this same Time API through build-time selection; it need not rediscover a
fixed backend through virtual calls at every boundary. Further accounting work
should measure the remaining transition bookkeeping before changing clock
precision or reporting semantics.

## Process type as immutable data

Starting at `81f796862`, `Process::getType()` becomes a nonvirtual inline read
of an immutable field. Protected constructors default to `Stock`; both
`PosixProcess` constructors explicitly select `Posix`. A bare `Process` created
from a POSIX parent remains `Stock`. The UID interface and credential handling
are unchanged. This replaces the earlier uncommitted POSIX getter-inlining
experiment; unrelated SLAM edits remain untouched.

The eligibility predicate now compares the type field directly and needs no
stack frame. Seven uninterrupted captures each execute **433 instructions and
12 CALL/RET pairs**, down from 440 and 13. No `getType` call executes. Capture
eight includes an interrupt and executes 4,957 instructions; it is retained
separately from the clean-path comparison. All 2,356 observed PC/byte pairs,
including the interrupted capture, match the frozen kernel/initrd.

Three fresh one-CPU repetitions per arm, with normal accounting and no tracing,
give the following one-million-call elapsed times:

| Variant | Elapsed seconds | Median |
| --- | --- | ---: |
| Before | 0.448386, 0.542631, 0.436034 | 0.448386 |
| Immutable process type | 0.434755, 0.435038, 0.418538 | 0.434755 |

The median is 3.0% lower, but this quick sample does not establish a stable
speedup. Enclosing wall/user/system medians change from
0.517287/0.283328/0.191085 s to 0.492578/0.254539/0.204930 s; system time does not
improve. All observations are retained. No ten-million-call run was made.

The kernel and modules were rebuilt together because removing the virtual
method changes their shared ABI. All fifteen syscall/accounting/signal guest
suites pass on one CPU. A hosted constructor/getter consumer compiles; hosted
execution and four-CPU testing were not run. Construction and teardown were
reviewed for type-dependent casts: the tag persists through base destruction,
with existing unpublication and lifetime barriers protecting derived state.

Artifacts are under `/private/tmp/pedigree-process-type-20260918`, including
starting source snapshots, frozen payloads and read-back-verified images,
`measurements.json`, `clean-trace-counts.json`, the complete trace, disassembly,
and guest/hosted logs.

## Inline handler lookup and registered entries

Starting at `7fea0c0c0`, `loadHandler()` moves into its header. Its acquire
semantics remain unchanged, but inlining removes the call and the duplicate
service bounds check. The syscall manager also gains a compact entry-function
table. Registration publishes the entry before releasing the handler pointer;
the caller acquires the handler before loading that fixed entry.

POSIX supplies one static dispatcher for both `linuxCompat` and `posix`. This
function contains the existing switch body, including diagnostic paths and
unknown-syscall handling. The virtual method delegates to it for compatibility.
The registered entry is an indirect function-pointer call without a vtable
lookup or an additional wrapper. No kernel reference to a POSIX implementation
symbol is introduced, so POSIX remains an optional module.

Slots begin null. For a handler without a supplied entry, registration installs
a virtual-dispatch fallback. Choosing it at registration avoids testing for a
null entry on every syscall. The emitted fallback is a load followed by a
virtual tail jump, with no additional return frame. Attempting to replace a
live supplied entry with another supplied entry is fatal; ordinary duplicate
registration still fails without changing either pointer. Retirement clears
both pointers, so subsequent quiescent reuse cannot retain a stale entry.
Concurrent production-handler unloading remains unsupported, as before; this
does not restore leases or change the existing lifetime requirement.

IRQ eligibility, IRQ policy, accounting, errno, post-syscall actions, and pending
return work remain unchanged. The eligibility query still needs the handler
object; the new table replaces invocation through its vtable, not that query.
Both x64 dispatch paths, hosted dispatch, and the synthetic leased test path
use the same entry invocation.

The baseline payload is byte-identical to the preceding process-type pass.
Fresh traces of each new variant contain eight uninterrupted captures each,
with all observed PC/byte pairs matching the frozen kernel/initrd:

| Variant | Instructions per getuid | CALL/RET pairs |
| --- | ---: | ---: |
| Starting implementation | 433 | 12 |
| Inline lookup only | 423 | 11 |
| Initial table with per-call null check | 427 | 11 |
| Retained table with registration-time fallback | 424 | 11 |

The table refinement removes three instructions from the initial attempt. The
retained table costs one address-calculation instruction versus inlining alone,
while removing the dispatcher vtable lookup. Accounting remains 211
instructions. Eligibility and the POSIX UID accessor are the two remaining
virtual calls. No PLT stub executes in the clean trace.

Fresh one-CPU QEMU TCG timing uses the unchanged million-call benchmark, normal
accounting, the real UID handler, and independent writable overlays. Builds and
tracing are excluded from the retained timing runs. Four baseline and inline
repetitions and three retained repetitions give:

| Variant | Inner elapsed median | Range | Enclosing wall / user / system medians |
| --- | ---: | --- | --- |
| Baseline | 0.430414 s | 0.425843–0.434168 s | 0.503187 / 0.253350 / 0.207284 s |
| Inline lookup | 0.400707 s | 0.390441–0.415604 s | 0.473550 / 0.246156 / 0.185091 s |
| Retained entries | 0.395377 s | 0.382892–0.400027 s | 0.459591 / 0.233152 / 0.187864 s |

The combined inner median is 8.1% below baseline and 1.3% below inlining alone.
The latter ranges overlap, and system time is higher than the inline-only
median: this small sample does not establish an independent table speedup.
The initial nullable-table median was 0.399948 s; those observations are also
retained. Its first boot overlapped a short native harness compile, which ended
immediately before its measured phase. The retained implementation's earlier
pre-formatting build measured a 0.384817 s median; formatting changed embedded
source-line metadata, so the final payload was rebuilt, frozen, and measured
again for the table above. Both batches remain in the artifacts. No
ten-million-call run was made.

Kernel and modules rebuild together with
`cmake --build build --target kernel initrd --parallel 8`. Both installed
payloads are read back and hash-checked. All fifteen guest syscall/accounting/
signal suites pass on one CPU. A focused native harness compiles the actual
base manager with mocked kernel dependencies: 35 assertions pass with ASan and
UBSan, covering absent services, virtual fallback, exact owner/state/result
preservation, duplicate rejection and fatal overwrite, token moves, reset/reuse,
dual-service ownership, and synthetic lease admission. These are sequential
registration tests, not kernel scheduling or publication-concurrency proof.
The actual base and hosted manager sources also compile with the Darwin-hosted
configuration. Hosted execution and four-CPU testing are not run.

Artifacts are under `/private/tmp/pedigree-fast-dispatch-20260918`, including
starting source snapshots, all variant payloads and image manifests,
`measurements.json`, per-variant `summary/` traces and Callgrind output, build
logs, hosted compile commands, and the reproducible native harness in `tests/`.
The retained guest results are `verified/contracts-1cpu` and `verified/trace`; timing
and trace commands are captured in each run directory. The four unrelated SLAM
edits are preserved and included equally in all variants.

## Validation and remaining uncertainty

The contracts are described in the
[benchmark guide](../scripts/benchmarks/README.md#syscall-framework-contracts).
They check real nonzero UIDs and child IDs, errno, ordinary fallback calls,
asynchronous signals during a query-only loop, stop/continue/kill behavior,
TLS, argument passing, mappings, and signal return. Run both with one and four
CPUs; a timing result does not establish those properties.

The earlier dispatch build passes all eleven suites on one CPU, including entry/TLS,
queries, resource accounting, 4,113 x64 clock conversion checks, and seven
signal interruption/restart cases. The ten suites excluding entry stress also
pass on four CPUs. Eighteen native clock/accounting tests and seven benchmark
runner tests pass. Installed kernel/initrd hashes were read back and matched.
Nonzero GS remains explicitly skipped because ARCH_SET_GS/GET_GS is unsupported;
forced CPU migration and NMI injection were not tested.

The full four-CPU entry stress passed on the pre-clock-optimization build but
repeatedly entered the debugger or panicked on the final build. An earlier
metadata run also panicked with `Mapping mutation admission timed out`.
The unmodified starting commit `65558e329` reproduces the same failure in
`contracts/head-4cpu-3`, establishing that this intermittent SMP defect predates
the dispatch changes. A pre-metadata control also hung in the debugger. Preserve
these failures and their paused stack samples; passing retries do not establish
SMP reliability. The original kernel's stacks show debugger quiesce closing
mapping admission while console workers are freeing event stacks with IRQs
masked. Quiesce waits for those workers, while they wait for admission. The
initial debugger trigger is still unidentified and is deferred from this pass.

Artifacts for the dispatch pass are under
`/private/tmp/pedigree-framework-phase-20260917`. They include starting state,
frozen kernels/images, per-run `command.json`, `serial.log`, `report.json`, trace
summaries, and the Linux manifest recording kernel and benchmark hashes. Timing
arms are `baseline1`, `leaf-clock/run1`, `final/run1`, `lazy/run1` through
`lazy/run3`, and the retained `div/run1` through `div/run3`; the Linux reference
is `linux/run1`. The final trace and Callgrind output are in `div/summary`.
Final contract results are `div/contracts-1cpu` and `div/focus-4cpu`.
Failed SMP runs and controls remain under `lazy/contracts-4cpu`,
`div/contracts-4cpu*` and `contracts/`. The runner now stops on a complete panic
line instead of waiting for its deadline.

For a new timing run, prepare a disposable image using the
[compilation latency guide](../scripts/benchmarks/compile-latency.md), install
the unchanged `vm-syscall-latency` ELF, and set `/vm-syscall-latency.conf` to
`getuid 1000000 1`. Run one guest at a time with a fresh output directory:

```sh
uv run --no-project python scripts/benchmarks/run-compile-latency.py \
  --image /path/to/frozen-getuid.img --output /path/to/new-results \
  --firmware-code /path/to/OVMF_CODE.fd --cpus 1 --synthetic-vm --timeout 90
```

Freeze the kernel and initrd installed in the image before starting QEMU, and
keep backing images unchanged while overlays use them. Use uninstrumented
repetitions for timings and a separate plugin run for the path trace. Preserve
the inner `IOBENCH` metric, enclosing process accounting, and host phase time as
distinct measurements.
