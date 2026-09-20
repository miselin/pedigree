# Performance integration

The performance marathon is consolidated for local integration into `develop`.
The original 123-commit history is preserved on
`codex/performance-marathon-archive`. Production improvements remain enabled;
metadata, user-return, and VM ablation controls have been removed. Historical
report parsers remain able to read the archived experiments.

Use [kernel build profiles](kernel-build-profiles.md) for T420/general-use and
separate diagnostic builds, and [syscall performance and verification](syscall-framework-performance.md)
for the retained architecture and profiling tools. The compiler matrix defaults
to one CPU; `--cpus 4` selects SMP qualification.

## Four-CPU failure

Repeated syscall-entry/query stress exposed an assertion in
`PerProcessorScheduler::scheduleWithInterruptState`: after `selectNext` released
the ready-queue lock, a second locked lookup asserted that no runnable thread
existed. A different CPU can legitimately publish a ready thread between those
operations. Disabling local interrupts does not prevent that publication.

The invalid assertion and its otherwise-unused virtual lookup were removed.
Thread selection and ready publication retain their existing locks. A late
publication remains queued for a subsequent scheduling interrupt. The fix does
not discard work or change the sleeping-thread transition protocol.

The initial failure was obscured by debugger processor quiescence stalling after
entry. Assertion locations and fatal/debugger entry details now reach serial
before that barrier, even when normal serial logging is disabled. The debugger's
ability to quiesce CPUs holding IRQ-disabled resources remains a separate
limitation; these diagnostics expose the initiating failure rather than claiming
to repair every debugger-stop case.

## Integration checks

Validation uses freshly rebuilt kernel/initrd pairs in immutable UEFI fixtures
with disposable writable overlays on Darwin/arm64 QEMU TCG, q35/SandyBridge,
4 GiB RAM. Builds use `-O3`, normal interrupts, sampled accounting, lock tracking
off, and automatic-variable pattern initialization off. `DEBUGGER=TRUE` keeps
assertions active despite `ASSERTS=FALSE`. Disk writes are enabled only against
the disposable overlays.

- Six fresh four-CPU entry/query/rusage boots passed after the scheduler fix;
  the preceding image had failed on its second repeated boot.
- The 14-suite accounting/signal suite passed on one CPU and on three fresh
  four-CPU boots: query, rusage, concurrent accounting, clock scaling, CPU timers,
  timers/lifetime, and interrupted/restarted read, wait, futex, and sigsuspend.
- Extended one/four-CPU suites passed syscall entry, GS/TLS across threads and
  exec, vector I/O and mapping lifetime, VM remap/residency/discard, and memory
  locking. Each suite records its exit status and an end marker.
- The four-CPU RAM-root compiler matrix passed all 38 phases, including
  repeated full builds and `-pipe` builds. Generated programs executed, inputs
  retained their identities, and measured phases issued no block requests.
  This is a correctness/stability run, not a replacement one-CPU timing baseline.
- Python repository tests: 196 run, 19 environment-related skips; benchmark
  runner/report tests: 53 passed.
- Linux hosted kernel rebuilt and passed its reduced getuid profile plus
  state-cleanup regressions; this excludes native x64 userspace entry/return.
- Linux native utility tests: 1,208 passed with the RadixTree group excluded.
  `PedigreeRadixTree.SplitKeysBackwards` aborts identically on an isolated clean
  `develop` build at `e0ea791206dbb2973c64a5a4776db495853258fa`, using GCC 16.2.
  It is an existing failure, not a passing suite or a newly fixed defect.

The CPU-timer test now permits unchanged system time across short syscalls under
sampled accounting while retaining monotonicity and strict user-time progress.
Other test updates follow the GS lookup API, signal-return diagnostic argument,
value-based range storage, and geometrically growing cache bitmap backing.

Run artifacts, including the failed boots, source identities, build logs,
fixture manifests, and per-suite results, are preserved at
`/private/tmp/pedigree-mainline-prep-20260919` on the Darwin host and
`~/pedigree-artifacts/mainline-prep-20260919` on mattpc. The clean-develop Radix
reproduction is in the latter directory's `radix-baseline` subdirectory.

## Limits and reproduction

These are bounded integration checks, not proof of arbitrary SMP correctness or
real-hardware qualification. T420 deployment has not been tested. The guest
contracts do not constitute a fresh crash/reboot writeback certification.
See the [compile-latency procedure](../scripts/benchmarks/compile-latency.md)
and [compiler matrix](../scripts/benchmarks/compile-matrix.md) for fresh fixtures,
serial protocols, and separate storage persistence checks.

Keep one integration owner for each source/build checkout. Never rebuild backing
images while a guest uses them. Restore a shared build's original configuration
after experiments. Keep local consolidation/merge separate from pushing and
hardware deployment.
