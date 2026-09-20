# Syscall performance and verification

POSIX calls use the ordinary interruptible x64 entry and registered dispatcher.
The retained improvements include kernel GS access, lazy argument extraction,
deferred FS/GS metadata materialization, and the common no-pending-work return
predicate. Signal/restart handling, termination deferral, and metadata restoration
remain part of the syscall contract. `getuid` uses a checked POSIX process cast
and inline UID access, with the generic fallback retained; it does not select a
separate syscall entry route.

Scheduler-tick CPU accounting is the default on x64 and hosted targets. Set
`PEDIGREE_SAMPLED_TIME_ACCOUNTING=OFF` for precise accounting. Keep
`PEDIGREE_TIME_ACCOUNTING=ON` in both modes. Existing build caches require an
explicit selection when changing modes. See [CPU-time accounting](cpu-time-accounting.md)
for sampling granularity and the attribution limits of both modes, and
[x64 syscall entry](x64-syscall-entry.md) for the architectural contract.

## Latest bounded evidence

The September 19 sampled-accounting cleanup compared the same frozen benchmark
ELF and firmware on one-CPU QEMU TCG on mattpc, with normal interrupts and no
concurrent builds or guests. Three rotating rounds measured one million `getuid`
calls per image:

| Median | Previous sampled image | Final image |
| --- | ---: | ---: |
| Host-observed wall | 0.513970 s | 0.484719 s |
| Guest wall | 0.507325 s | 0.483979 s |
| Guest user / system | 0.130 / 0.370 s | 0.130 / 0.350 s |

Host wall improved 5.7%; guest wall improved 4.6%. Host timing includes serial
marker overhead. Guest CPU splits are estimates at the native 10 ms tick; these
are neither hardware measurements nor exact syscall CPU costs.

Three clean final structural captures contain 14 call/return pairs and no
RDTSC/RDMSR/WRMSR. Their 490 debugger snapshots are not retired instruction
counts: REP steps can repeat. Interrupt suppression was used only for structural
single stepping, never for timings. A fully inlined TimeTracker experiment
regressed latency and was discarded.

All eight guest contract suites in that checkpoint passed, covering signal
interruption/restart, main/worker exec, syscall entry, accounting, accounting
concurrency, and kernel GS. Precise and sampled hosted preflights also passed.
For subsequent checks of the combined source and the repaired four-CPU
assertion, see the [integration record](which-compilation-handoff.md).

## Retained profiling and regression tools

- [Syscall contracts](../scripts/benchmarks/README.md#syscall-framework-contracts)
  provides static target build commands and the entry, ABI, UID/query, accounting,
  concurrency, GS, and parallel-getuid workloads. Run correctness separately
  from timings, with one and four CPUs, an outer timeout, exit status, and a
  serial end marker.
- [Compile latency](../scripts/benchmarks/compile-latency.md) and
  [compiler matrix](../scripts/benchmarks/compile-matrix.md) document immutable
  fixtures, disposable overlays, warmups, stage workloads, and summaries. Use
  the matrix RAM-root lane to separate CPU/kernel work from storage requests.
- [Syscall trace](../scripts/benchmarks/syscall-trace.md) describes opt-in call
  and bounded path tracing. Per-syscall timing requires precise accounting;
  it is incompatible with sampled mode.
- [QEMU instruction profiling](../scripts/benchmarks/qemu-trace/PROFILE.md)
  and [trace qualification](../scripts/benchmarks/qemu-trace/README.md) verify
  executable bytes and distinguish clean captures from interrupted paths.
- [Hosted profiling](hosted-profiling.md) runs the actual hosted kernel on Linux
  amd64. It excludes x64 userspace entry/return assembly and does not replace
  guest contracts. For an already configured profiling build, run:

  ```sh
  scripts/profile-hosted.sh "$HOSTED_BUILD" "$NEW_OUTPUT" perf "$HOST_CPU"
  ```

Freeze the kernel, matching initrd, fixture backing chain, source revision/diff,
CMake cache, compiler commands, and toolchain identities before comparison.
Keep one integration owner and one timing guest at a time. Preserve failed runs
and all measured samples; instruction counts explain executed work, not elapsed
time shares. Deployment and diagnostic settings are listed in
[kernel build profiles](kernel-build-profiles.md).

The full experiment history, including removed query-only routes and ablation
arms, is preserved on `codex/performance-marathon-archive`. Historical timings
from different accounting modes, diagnostics, storage fixtures, or host machines
are not a cumulative speedup estimate.
