# Vector I/O and linker performance

This September 19, 2026 pass compares `gcc which.o -lstdc++ -o matrix-link`
on one-CPU QEMU 11.1.1 TCG (Q35, SandyBridge, 4 GiB). Both systems use the
same GCC 15.3.0/musl binaries, object file, and RAM-root inventory. Every
measured link produces an executable that successfully runs `./matrix-link gcc`.
Block-device counters remain unchanged throughout the measured phases.
These are link-only measurements, not full compilation or hardware results.

## Results

Each row aggregates two fresh guests, each with one excluded warm-up and five
measured links. Values are medians of the two per-guest medians, in seconds.
Normal accounting and interrupts remain enabled; syscall tracing, timing
diagnostics, and getuid ablations remain disabled.

| Configuration | Host wall | Guest wall | Guest user | Guest system | Host/Linux |
| --- | ---: | ---: | ---: | ---: | ---: |
| Linux 3.2.78, matched toolchain | 0.334 | 0.332 | 0.190 | 0.136 | 1.00x |
| Starting revision `c296665ea`, `-Os` | 2.229 | 2.157 | 0.266 | 1.751 | 6.68x |
| Through `361fd6b73`, original `-Os` | 1.770 | 1.697 | 0.269 | 1.379 | 5.30x |
| Same changes, existing `-O3` option | 1.351 | 1.268 | 0.245 | 0.992 | 4.05x |

The final `-Os` per-guest host medians ranged from 1.680 to 1.859 seconds;
the `-O3` medians ranged from 1.344 to 1.358. Host variation limits precise
percentage attribution. The source changes and compiler policy are separate
comparisons: the faster result is not the result of source changes alone.
The 2–3x Linux target has not yet been reached.

The original build settings were restored after the experiment. To select the
existing speed-oriented option explicitly:

```sh
cmake -S . -B build -DPEDIGREE_OPTIMIZE_SIZE=FALSE
cmake --build build --target kernel initrd -j6
```

Both configurations pass with normal compiler warnings treated as errors.
The x64 kernel end grows from `0xffffffff801be000` to `0xffffffff802ee000`;
the speed setting therefore has a substantial memory-footprint cost. No
optimization defaults or other architecture policies were changed.

## Retained changes

- Unobserved file events return before taking a publication lease.
- Read/write permission checks use immutable open-file access modes without
  acquiring the mutable-status/position mutex.
- Periodic cache dispatch skips caches that have no timer work; reclamation
  and shutdown still visit them.
- Uncontended mutex completion avoids unnecessary wait bookkeeping.
- Native x86 interrupt-state helpers and the normal hard-IRQ guard predicate
  inline while retaining their addressable exports and rejection paths.
- RAMfs acquires an exact pinned cache page in one lookup, preserving pin
  balance, shrink protection, and copy-on-write mapping behavior.
- Termination deferral publishes its return-work flag only on entry to the
  outermost scope. Depth, cleanup sequence, and list atomics remain intact.
- Descriptor lookup/publication use one mutex rather than a reader lock whose
  entry and exit each acquired an underlying mutex. Descriptor leases and
  retirement outside the table lock remain intact.
- Explicit bounds/result/lifetime checks let the existing `-O3` configuration
  build with normal warning enforcement.

The empty-event change alone did not establish a timing improvement. Other
changes were measured in cumulative groups; the total gain cannot be assigned
to one change or added up from individual profile percentages.

## Validation and profiles

The final kernels pass 38 one-CPU guest vector-I/O contracts, including concurrent
`dup2` replacement, shared-offset readers, invalid buffers, partial faults,
special descriptors, and mapped-cache lifetime. The two new descriptor races
also pass on Linux. Focused native validation passes 121 cache/VFS tests and
53 module-image/PIC state tests. Isolated Darwin hosted tests pass mutex
completion, state publication/cleanup, and all seven hard-IRQ guard denials.
The IRQ test uses source-identical module-local context setup; it does not
claim real hardware IRQ delivery coverage. Four-CPU testing was deferred.

The qualified `-Os` profile counts 672,648,364 kernel instruction dispatches,
35.7% below the starting profile, with identical user and syscall counts.
The qualified `-O3` profile counts 551,857,707 kernel instruction dispatches,
47.2% below the starting 1,045,467,775. Its 64,055 calls have the identical
syscall histogram. User dispatches differ by 12 (244,811,919 to 244,811,931);
the cause of that tiny difference was not established. All observed kernel
instruction bytes match the frozen images. Counts include background and
interrupt activity and are not function timings.

Spinlock acquire/exit/release still account for 84.95 million dispatches in
that profile. Only six acquisition CAS failures occur, all recursive entries.
The next bounded experiment is to move rare diagnostic logging into cold
helpers and inspect whether the common path loses its large stack frames.
Preserve ownership, IRQ restoration, recursion, tracking, and atomic behavior.

## Evidence and reproduction

Use [the vector-I/O guide](../scripts/benchmarks/vector-io.md) and
[compiler matrix guide](../scripts/benchmarks/compile-matrix.md). A target build
does not update an already frozen benchmark fixture. Install and verify the
new boot payload before timing; the direct UEFI fixture requires
`initrd.tar.uncomp`, not the compressed `initrd.tar`.

Artifacts are under `/private/tmp/pedigree-event-hillclimb-20260919`:
`comparison.{md,json}` retains individual samples, fixture identities, excluded
runs, profile qualification, and paired seek/vector microbenchmarks; `REPORT.md`
records validation and failures. `next-size/fixture.qcow2` and
`next-speed/fixture.qcow2` are frozen timing fixtures. Keep their backing chains
unchanged and use new disposable output overlays for each run.

Known validation limits are retained in the report: an unchanged cache-bitmap
test fails in both old and current native binaries; the broad Darwin hosted
suite has unrelated baseline failures; Linux 3.2 tmpfs fails an existing
partial-write-state contract before reaching the new descriptor tests, which
were therefore also run separately. No passing full-suite claim is made.
