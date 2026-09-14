# `which` compilation performance handoff

Branch: `codex/which-compile-performance`. This is an implementation handoff
for two bounded follow-ups. Implement A, then B: both edit the same file and
need separate timing checkpoints. Use one writer for that file and one build/
QEMU integration owner. Do not widen this into another performance audit.

## Evidence and completed work

The investigation timed the unchanged native workload
`gcc -o which which.cc -lstdc++` in a disposable x86-64 QEMU guest. Clean
one-vCPU controls went from about **177.8 / 181.5 s** (cold / warm, writes disabled) to
**55.8 / 52.0 s** on the current cumulative kernel, with real disk writes
enabled. The large measured gains came from retaining mmap list nodes,
reusing CPU identity in lock bookkeeping, contiguous reservation storage,
worker wake gates, and the lower RTC rate. The watchdog fix did not establish
an isolated compilation speedup. The 128-Hz RTC arm is
**55.795 / 52.023 s**; 64 Hz is **57.188 / 51.401 s**, so 64 Hz supplies no
clear additional improvement. The final CPU-identity simplification measured
**55.734 / 52.320 s**, effectively unchanged.

Already implemented and measured: runnable-current scheduling; anonymous-page
address indexing; timer hazard-scan gating; mmap list retention; CPU identity
reuse; value-based `RangeList`; cache/accounting wake gates; elapsed-time
watchdog refresh and correct disable; explicit RTC rate selection. Preserve
these changes and unrelated dirty files.

The latest frozen artifacts are `/tmp/pedigree-which-perf-20260914/cpu-id`
and `quick-128.img`. The identity follow-up has a target build, one-CPU
compile/write/reboot, and one/four-CPU full signal/timer/clock suites passing.
The final identity kernel has not repeated the guest compilation/persistence
benchmark with four CPUs; the prior worker-gated kernel passed that qualification.

## A. Finish `MemoryMapManager::allows()`

Edit `src/modules/system/vfs/MemoryMappedFile.cc:1184`. In the inner mapping
scan, return success as soon as the updated `coveredUntil` reaches `end` (or
equivalently stop the scan and advance to the next outer iteration). Keep the
existing operation guard, page rounding, permission check, and hole failure.
Do not turn coverage into a simple single-object test: adjacent and
overlapping mappings, holes, and split permissions remain valid cases.

The operation guard and its post-blocking revalidation are part of the
contract. Preserve the revalidation in user-buffer callers, including the
blocking path that detects a concurrent unmap. Relevant call paths are
`src/modules/subsys/posix/PosixSubsystem.cc:573` and the user-copy/readv/writev
callers. Add or extend focused hosted coverage for complete coverage, holes,
`PROT_NONE`, partial faults, and adjacent mappings with different permissions.

## B. Reuse the selected mapping during user fault handling

`MemoryMapManager::resolveUserFault()` at
`src/modules/system/vfs/MemoryMappedFile.cc:1344` selects and validates an
object, then calls `handleTrap()` at about line 1374; `handleTrap()` searches
the mapping list again at about line 1478. Introduce the smallest internal
handoff that lets the already selected object perform the trap operation.

Preserve lock and lifetime rules: the selected pointer is found under the
mapping manager's operation/lock protocol, and must not be used after an
unmap can retire it. Preserve permission and backing-end checks, resident
population, final PTE validation, copy-on-write, swap, anonymous mappings,
split mappings, allocation/I/O failure, and rollback behavior. Do not remove
the final mapping/PTE checks merely because the first lookup succeeded.

Qualify existing contracts in `src/modules/system/hosted-smoke/swap-regressions.cc`
and `src/modules/system/hosted-smoke/vector-io-regressions.cc`; include
`src/modules/system/hosted-smoke/usercopy-regressions.cc`, VM permission
coverage, EFAULT/partial-progress cases, and split/COW/swap fault cases.
The useful page population work is included in the profile totals and is not
all avoidable lookup cost.

## Separate design work

Adaptive RTC service is a separate design slice. Do not bundle it with A or B
or infer that an empty alarm queue means no fine-timing demand. Timerfd,
POSIX timers, `ITIMER_REAL`, input repeat, vDSO snapshots, watchdog, writeback,
and shutdown need explicit demand publication, hysteresis, and race review.
The current evidence supports 128 Hz as the experimental balance point. The
default remains 512 Hz; reassess clock granularity and physical input before
changing that policy.

## Acceptance and integration ownership

Each implementation slice must pass focused hosted tests and formatting, then
the single integration owner rebuilds all affected consumers (`kernel` and
`initrd`) and runs the bounded QEMU workload from
`scripts/benchmarks/compile-latency.md` using a fresh disposable overlay.
Record cold/warm timing, successful compile, real writeback/sync, reboot
binary hash and `./which gcc`. Run one and four CPUs. Re-run the full
one/four-CPU signal/timer/clock suite and the mapping contracts; stop on a
failure and retain logs under `/tmp/pedigree-which-perf-20260914`.

Do not claim a speedup from static code inspection or one noisy run. Report
the exact command, image, CPU count, writeback result, and any skipped
coverage. The integration owner performs builds, VM runs and focused local
commits after the source edits. See `docs/which-compilation-performance.md`
for the complete evidence and `scripts/benchmarks/compile-latency.md` for commands.
