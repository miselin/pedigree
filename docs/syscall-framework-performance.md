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

The retained implementation's median is 0.718402 s: about 2.9 times faster than
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

The final trace contains eight uninterrupted calls of 710 instructions each,
down from 1,076 at the starting commit. All return UID 0 and all observed bytes
match the frozen kernel and modules. Each call has two SWAPGS, two RDTSC, four
LOCK-prefixed additions and 40 CALLs. No RDMSR, WRMSR or REP initialization executes
on this clean query path. Pending work still uses full metadata capture/restore.

Accounting occupies 380 of the 710 dispatches (53.5%), including clock reads,
mode transitions and thread/process counter publication. It is the next focused
target; this pass does not establish Linux parity. Replacing thread counter
atomic additions with load/store is unsafe: NMI accounting can nest despite
masked IRQs and its update would be lost. Any batching or deferred publication
design must preserve resource-usage reads, CPU timers, migration and exit totals.

Instruction traces explain executed paths, not cycles or elapsed cost. A
serializing MSR instruction and an ordinary register operation each count as a
dispatch, while REP/string-operation dispatch counts depend on translation and
instrumentation. Do not use their shares as a time profile or infer the cost of
a whole copy from its REP count. Compare clean complete syscall captures
separately from captures containing interrupts or scheduling.

## Validation and remaining uncertainty

The contracts are described in the
[benchmark guide](../scripts/benchmarks/README.md#syscall-framework-contracts).
They check real nonzero UIDs and child IDs, errno, ordinary fallback calls,
asynchronous signals during a query-only loop, stop/continue/kill behavior,
TLS, argument passing, mappings, and signal return. Run both with one and four
CPUs; a timing result does not establish those properties.

The final build passes all eleven suites on one CPU, including entry/TLS,
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

Artifacts for this pass are under
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
