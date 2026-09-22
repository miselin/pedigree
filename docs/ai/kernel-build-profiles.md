# Kernel build profiles

Use separate configured build directories for deployment and debugging. These
are build-time choices: a GRUB menu entry must select a separately built,
matching kernel and initrd. A boot argument cannot change compiler optimization,
allocator scribbling, lock diagnostics, or accounting instrumentation.

## T420 and general use

For an existing x64 target build with its toolchain and image settings already
configured, apply the following profile. Replace `build-t420` with that build's
path; this command does not bootstrap a fresh cross-toolchain configuration.

```sh
cmake -S . -B build-t420 \
  -DPEDIGREE_OPTIMIZE=TRUE \
  -DPEDIGREE_OPTIMIZE_SIZE=FALSE \
  -DPEDIGREE_TRACK_LOCKS=FALSE \
  -DPEDIGREE_STRICT_LOCK_ORDERING=FALSE \
  -DPEDIGREE_SPINLOCK_DIAGNOSTICS=FALSE \
  -DPEDIGREE_DEBUG_ALLOCATOR=FALSE \
  -DPEDIGREE_AUTO_VAR_INIT=FALSE \
  -DPEDIGREE_ADDITIONAL_CHECKS=FALSE \
  -DPEDIGREE_EXCESSIVE_ADDITIONAL_CHECKS=FALSE \
  -DPEDIGREE_ASSERTS=FALSE \
  -DPEDIGREE_DEBUGGER=FALSE \
  -DPEDIGREE_DEBUG_LOGGING=FALSE \
  -DPEDIGREE_TRACING=FALSE \
  -DPEDIGREE_MEMORY_LOG=FALSE \
  -DPEDIGREE_LOG_TO_SERIAL=FALSE \
  -DPEDIGREE_TIME_ACCOUNTING=TRUE \
  -DPEDIGREE_SAMPLED_TIME_ACCOUNTING=TRUE \
  -DPEDIGREE_SYSCALL_COUNTER=FALSE \
  -DPEDIGREE_ACTIVITY_DIAGNOSTICS=FALSE \
  -DPEDIGREE_X64_USER_ENTRY_DIAGNOSTICS=FALSE \
  -DPEDIGREE_BENCHMARK_SYSCALL_TIMING=FALSE \
  -DPEDIGREE_BENCHMARK_SYSCALL_TRACE=FALSE \
  -DPEDIGREE_BENCHMARK_VM_DIAGNOSTICS=FALSE \
  -DPEDIGREE_HOSTED_FUNCTION_PROFILE=FALSE
cmake --build build-t420 --target kernel initrd -j8
```

`PEDIGREE_OPTIMIZE=TRUE` with `PEDIGREE_OPTIMIZE_SIZE=FALSE` selects `-O3` for
the kernel and modules; size optimization selects `-Os`. This is independent of
`CMAKE_BUILD_TYPE`. Existing CMake caches retain their values until explicitly
changed. Rebuild the deployment image after producing the matched pair.

SLAM freed-block scribbling is controlled by `SCRIBBLE_FREED_BLOCKS` in
`src/system/include/pedigree/kernel/core/SlamAllocator.h`, currently `0`.
It is not a CMake option. Leave it at zero for this profile. Normal locking and
CPU accounting remain enabled. Disabling additional checks, tracing, memory
reports, and kernel serial logging matches the measured performance configuration.

Assertions are enabled when either `PEDIGREE_DEBUGGER` or `PEDIGREE_ASSERTS`
is true (`utilities/assert.h` uses `#if DEBUGGER || ASSERTS`). The measured
`-Os`/`-O3` builds had `PEDIGREE_ASSERTS=FALSE` and `PEDIGREE_DEBUGGER=TRUE`,
so assertions remained enabled. The deployment profile above explicitly sets
both to false to disable assertions and the kernel debugger. That is a deployment
policy choice, not the configuration measured in the comparison below. It reduces
diagnostic coverage; use the separate debugging profile when investigating
failures. Keep hardware, disk-write, and image settings explicit for the intended
installation; this profile does not certify a T420 hardware deployment. See the
[integration record](which-compilation-handoff.md) for bounded SMP qualification.

## Optional debugging

Start from a separate x64 build configured with the same toolchain and hardware
settings. Enable only the diagnostics needed for the investigation, for example:

```sh
cmake -S . -B build-debug \
  -DPEDIGREE_DEBUGGER=TRUE \
  -DPEDIGREE_ADDITIONAL_CHECKS=TRUE \
  -DPEDIGREE_ASSERTS=TRUE \
  -DPEDIGREE_TRACING=TRUE \
  -DPEDIGREE_MEMORY_LOG=TRUE \
  -DPEDIGREE_LOG_TO_SERIAL=TRUE \
  -DPEDIGREE_TRACK_LOCKS=TRUE \
  -DPEDIGREE_SPINLOCK_DIAGNOSTICS=TRUE \
  -DPEDIGREE_AUTO_VAR_INIT=TRUE
cmake --build build-debug --target kernel initrd -j8
```

Serial kernel logs, tracing, and periodic memory reports are enabled in this
example for diagnosis; disable unneeded output for a focused investigation.
Automatic variable initialization requires compiler support, checked by CMake.
SLAM scribbling needs a separate source/build variant with
`SCRIBBLE_FREED_BLOCKS=1`; it is not enabled by the command above.
`PEDIGREE_DEBUG_ALLOCATOR=TRUE` replaces SLAM with an allocator that never reuses
allocations, so reserve it for bounded allocator debugging. Debug profiles are
not comparable to deployment profiles for timing.

For precise CPU accounting select `PEDIGREE_SAMPLED_TIME_ACCOUNTING=FALSE` while
keeping `PEDIGREE_TIME_ACCOUNTING=TRUE`. Per-syscall timing additionally requires
`PEDIGREE_BENCHMARK_SYSCALL_TIMING=TRUE`; sampled accounting and syscall timing
cannot be enabled together. See [accounting limits](cpu-time-accounting.md) and
[profiling tools](syscall-framework-performance.md#retained-profiling-and-regression-tools).

## Evidence for speed optimization

On September 19, the same source at `85743d9e49` plus the six preserved mapping
edits was built with `-Os` and `-O3`. Both images ran on a Darwin/arm64 host with
QEMU 11.1.1 TCG, one SandyBridge vCPU, q35, and 4 GiB RAM. Firmware, immutable
RAM-root fixture, GCC 15.3/musl binaries, inputs, and guest compiler commands
matched. Normal interrupts and sampled accounting were enabled; other diagnostic
flags matched between images. This was not a test of every deployment setting
above.

| Host-wall median | `-Os` | `-O3` | Reduction |
| --- | ---: | ---: | ---: |
| Preprocess | 1.479 s | 1.259 s | 14.9% |
| Syntax | 6.640 s | 5.569 s | 16.1% |
| Code generation | 18.928 s | 15.270 s | 19.3% |
| Assemble | 1.132 s | 1.005 s | 11.2% |
| Link | 1.427 s | 1.098 s | 23.1% |
| Full build | 22.079 s | 18.656 s | 15.5% |
| Full build with `-pipe` | 22.153 s | 18.700 s | 15.6% |

There were three measured rounds after warmup, within one boot per configuration.
All 38 phases passed, generated binaries executed, input identities matched,
and the runner verified zero measured block requests. These bounded TCG results
support the profile choice; they are not T420 measurements or a broad statistical
trial. Follow the [compiler matrix procedure](../scripts/benchmarks/compile-matrix.md)
to prepare a fresh immutable fixture and reproduce the comparison with only
`PEDIGREE_OPTIMIZE_SIZE` changed. Retain compiler-command manifests and all samples.
