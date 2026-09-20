# Native `which` compilation: historical findings

This records the September 14, 2026 investigation starting at
`e0ea791206dbb2973c64a5a4776db495853258fa`. It is historical evidence, not a
performance or qualification report for the final integrated source. The full
diary and experimental configurations are preserved on
`codex/performance-marathon-archive`.

For current settings and newer measurements, see [kernel build profiles](kernel-build-profiles.md),
[syscall performance](syscall-framework-performance.md), and the
[integration record](which-compilation-handoff.md). Earlier passes qualify only
their recorded source revisions.

## Workload and baseline

The unchanged `src/user/applications/which/main.cc` was installed as
`/root/compile-bench/which.cc` and compiled natively in Pedigree. The exact
requested `gcc -o which which.cc` took **160.863 seconds and failed to link**:
`gcc` does not automatically link the C++ library. The successful workload was
`gcc -o which which.cc -lstdc++`, followed by `./which gcc` and mapping contracts.
The failed-link duration is not the successful-compilation baseline.

The original successful clean run used one x86-64 vCPU on Apple Silicon macOS,
QEMU 11.1.1 TCG, q35, SandyBridge, 4 GiB RAM, no network, and serial-only display.
The Debug kernel used `-Os`, with lock tracking and log generation retained but
serial kernel log printing disabled. Userspace used GCC 15.3.0, binutils 2.46.1,
matching C++ libraries and dependencies, and matching musl. Its C++ headers
produced a 1,851,988-byte preprocessed input despite the small source file.

Each clean run used a new writable overlay on a frozen image, one guest, and no
concurrent build. Cold means a fresh guest cache, not necessarily a cold host
cache. “Sampling disabled” in these historical runs means the host profiler was
disabled; it does not mean today's sampled CPU-accounting option. These runs
predate scheduler-tick accounting becoming the x64 default.

| Original successful phase | Wall | Reported user | Reported kernel |
| --- | ---: | ---: | ---: |
| Cold compilation | 177.786 s | 23.243 s | 131.652 s |
| Warm compilation | 181.528 s | 22.539 s | 133.396 s |

## Cumulative checkpoints

Only the original row disabled disk writes; later rows enabled them. Each row
is a single cold/warm pair, not a repeated statistical trial or an isolated
measurement of every constituent change.

| Cumulative configuration | Cold | Warm |
| --- | ---: | ---: |
| Original kernel, writes disabled | 177.786 s | 181.528 s |
| mmap list retention and CPU identity reuse | 88.402 s | 85.799 s |
| Contiguous reservation ranges | 72.881 s | 66.165 s |
| Cache/accounting worker wake gates | 71.976 s | 58.257 s |
| Watchdog fix, RTC still 512 Hz | 70.389 s | 61.495 s |
| Same kernel, experimental RTC 128 Hz | 55.795 s | 52.023 s |
| Same kernel, experimental RTC 64 Hz | 57.188 s | 51.401 s |
| Validated TSS identity reuse, RTC 128 Hz | 55.734 s | 52.320 s |

The large reductions coincided with avoiding unrelated mmap-node reconstruction
and per-record reservation allocation. Worker profiles showed less background
activity, but the full warm wall-time difference cannot be assigned to worker
CPU savings. The watchdog-only and final identity pairs did not establish
isolated compilation speedups. The 64 Hz pair did not establish improvement over
128 Hz. Lower RTC frequency also coarsened alarm, published-clock, and input
service granularity; this table is not a recommendation to change timer policy.

Write-enabled mmap/lock and reservation checkpoints synced successfully. Fresh
boots verified the 1,980,136-byte executable against FNV-1a `41f5414073fc9aac`
and ran it without recompilation. Earlier writes-disabled runs attempted sync
and returned `EIO`; those failures were retained. Performance-only runs that omit
sync cannot establish persistence.

## What the profiles established

Original warm runs performed zero backing-disk reads and writes yet took about
three minutes. Disk traffic could not explain that workload. Profiles linked
substantial allocation, destruction, and lock work to mapping publication and
reservation snapshots. Later profiles exposed vector-buffer validation and
mapping searches even without backing reads. Permission checks, mapping lifetime,
and post-blocking revalidation remain necessary correctness requirements.

Two diagnostic runs counted 124,005 cold and 124,009 warm compiler-tree syscalls.
At that volume, saving one microsecond per entry saves only about 0.12 seconds;
substantial cost lay inside syscall bodies, faults, and scheduling. Instrumented
latency buckets included blocking/descheduling and excluded architecture entry
and final return, so they were not measurements of fixed syscall entry cost.

Paused/jittered host sampling captured IRQ-disabled work and resolved addresses
against frozen kernel/module ELFs. Inclusive stacks overlap, asynchronous walks
can omit callers, and QEMU stop points can bias samples toward MMIO boundaries.
Sample fractions are not exact function CPU times. Historical CPU totals used
TSC-derived accounting and could charge host descheduling to a guest interval;
reported fault/block/context-switch fields were unimplemented zeros. Storage
claims used QEMU block counters. None of this establishes T420 fan, power, input,
or physical compilation performance.

## Reproduction and validation

Use the [compile-latency guide](../scripts/benchmarks/compile-latency.md) for guest
preparation, bounded runs, profiles, symbolization, and reboot persistence checks.
Use the [compiler matrix](../scripts/benchmarks/compile-matrix.md) for repeated
stage comparisons and the RAM-root lane. Freeze source/diff, build flags,
compiler commands, kernel/initrd, firmware, toolchain, and image backing chains;
retain failures and all samples. Run one timing guest at a time and compare
uninstrumented images with matching configurations.

Historical native utility, hosted core/VM, and one/four-CPU signal/timer checks
passed at their recorded checkpoints. Four-CPU compilation and persistence were
qualified through worker gating, while the final identity checkpoint repeated
compilation/persistence only on one CPU. These checks cover those revisions only.
Follow the current integration record for checks of the consolidated source.
