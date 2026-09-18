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
