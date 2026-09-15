# Profiling native `which` compilation

This investigation measures the repository's unchanged
`src/user/applications/which/main.cc` as `/root/compile-bench/which.cc` inside
Pedigree. The starting revision was `e0ea791206dbb2973c64a5a4776db495853258fa`.
The main workload is `gcc -o which which.cc -lstdc++`, followed by execution of
`./which gcc` and mapping contracts.

The exact requested `gcc -o which which.cc` was also timed: 160.863 seconds,
then a link failure because `gcc` does not automatically link the C++ library.
The successful command adds only `-lstdc++`; the source and compiler options
are otherwise unchanged. Although the program is small, its C++ headers
produce a 1,851,988-byte preprocessed file and exercise substantial template
compilation. That does not explain spending most of the time in the kernel.

## Measurement conditions

- Apple Silicon macOS host, QEMU 11.1.1, x86-64 TCG with threaded translation.
- Q35, SandyBridge CPU model, 4 GiB RAM, no guest network, serial-only display.
- One virtual CPU for the initial measurements; SMP qualification uses four.
- Debug kernel with its normal `-Os`, lock tracking and log generation retained.
  Serial log printing is disabled consistently to protect benchmark records.
- Native GCC 15.3.0, binutils 2.46.1, matching C++ headers/static library,
  GMP/MPFR/MPC/zlib dependencies, and the existing matching musl runtime.
  These were installed from local package artifacts. The image's original
  GCC 8 installation could not load `cc1plus` because dependencies were absent.
- A static benchmark driver replaces init only in disposable test images.
  Every arm uses the same compiler files and source. Each new run gets a fresh
  qcow2 overlay against a frozen image; a warm compile reuses the same guest.
- Early discovery runs overlapped host builds or another one-CPU guest. Their
  timings are exploratory. The separately identified clean runs use one guest,
  no concurrent build, and sampling disabled.

Fresh guests have cold guest executable/header caches, not necessarily cold
host caches. These are comparisons within QEMU, not T420 compilation times or
measurements of physical fan behavior.

## Results

The clean original-kernel run, with sampling disabled and writes disabled,
completed successfully:

| Phase | Wall seconds | User CPU seconds | Kernel CPU seconds |
| --- | ---: | ---: | ---: |
| Cold successful compilation | 177.786 | 23.243 | 131.652 |
| Warm successful compilation | 181.528 | 22.539 | 133.396 |

The successive clean runs had sampling disabled and no concurrent build or
other guest. Only the original arm omitted disk writes; all later rows enabled
them. Each row is one cold/warm pair, so small differences need repetition.

| Cumulative change | Cold seconds | Warm seconds | Cold kernel seconds | Warm kernel seconds |
| --- | ---: | ---: | ---: | ---: |
| Original kernel, writes disabled | 177.786 | 181.528 | 131.652 | 133.396 |
| mmap list retention and CPU identity reuse | 88.402 | 85.799 | 54.923 | 53.806 |
| Contiguous reservation ranges | 72.881 | 66.165 | 41.566 | 37.772 |
| Cache/accounting worker wake gates | 71.976 | 58.257 | 42.522 | 35.678 |
| Watchdog refresh/disable fix, still 512 Hz | 70.389 | 61.495 | 43.040 | 38.419 |
| Same kernel, RTC at 128 Hz | 55.795 | 52.023 | 35.664 | 33.557 |
| Same kernel, RTC at 64 Hz | 57.188 | 51.401 | 36.235 | 32.847 |
| Reuse validated TSS identity, RTC at 128 Hz | 55.734 | 52.320 | 35.015 | 33.019 |

The 128 Hz pair is **3.19 times faster cold and 3.49 times faster warm** than
the original pair, with real disk writes enabled. Its warm compile still spends
a reported 33.557 seconds in the kernel. The 64 Hz pair does not establish a
further improvement: cold is slower and warm is only 1.2% faster.

The worker-gating cold result was essentially unchanged from its preceding arm. Its
warm reported kernel time fell only 5.5%, while wall time fell 12%; that entire
wall difference should not be attributed to kernel work. The CPU control
ranged from 0.196 to 0.268 seconds across these single runs (original 0.235),
so small incremental differences remain approximate.

Before the range/worker changes, the write-enabled cold phase read 33.266 MiB
and wrote 4.164 MiB; the warm phase read nothing and wrote 4.816 MiB. Sync took
0.147 seconds and issued another 89 writes. Fresh boots after both that arm
and the range-storage arm verified all 1,980,136 binary bytes against the saved
FNV-1a checksum `41f5414073fc9aac` and executed `./which gcc` without recompiling.
Real writeback therefore preserves the measured improvement.

The full exploratory stage breakdown after the scheduler/page-index changes
was:

| Stage | Wall seconds | User CPU seconds | Kernel CPU seconds |
| --- | ---: | ---: | ---: |
| Preprocessing | 9.406 | 0.922 | 7.113 |
| Code generation from preprocessed input | 149.391 | 20.763 | 103.782 |
| Assembly | 3.808 | 0.906 | 2.154 |
| Linking | 25.692 | 0.812 | 19.560 |
| Execute `which gcc` | 0.042 | 0.007 | 0.024 |

The 128 Hz paused, jittered profile completed all stages:

| Stage | Wall seconds | User CPU seconds | Kernel CPU seconds |
| --- | ---: | ---: | ---: |
| Cold compilation | 55.644 | 17.301 | 35.439 |
| Warm compilation 1 | 51.686 | 16.238 | 33.075 |
| Warm compilation 2 | 52.357 | 16.406 | 33.501 |
| Preprocessing | 4.771 | 0.779 | 3.629 |
| Code generation | 33.329 | 15.103 | 16.402 |
| Assembly | 2.166 | 0.718 | 1.319 |
| Linking | 13.700 | 0.480 | 12.354 |

The stage tables come from profiled runs with different capture modes, so they
are discovery measurements rather than a controlled per-stage speedup trial.
Linking remains predominantly kernel work and deserves a focused next pass.

GCC's `-ftime-report` is retained in the serial log. Its compiler-pass wall
times include kernel work performed on behalf of each pass; a large pass time
alone does not establish expensive computation inside that pass.

### Progressive changes

1. **Keep a runnable current thread on its CPU.** When no other ready thread
   exists, a tick/yield previously selected idle before returning to the same
   runnable thread. The change retains worker readiness predicates and normal
   event dispatch. Hosted regression tests confirm the intended behavior.
2. **Index anonymous pages by address.** Anonymous page tracking used repeated
   linear scans of a list. A tree supplies direct lookups while retaining
   swap, copy-on-write, split, relocation and allocation-failure behavior.
   These first two changes did not materially improve this compilation:
   the initial failed-link attempt was 169.095 seconds versus 160.863 seconds
   in the original exploratory arm, with similar user/kernel proportions.
3. **Avoid an unnecessary timer hazard scan.** Ordinary callback completion
   need not scan the entire active-dispatch table unless a handler is being
   drained or removed. Existing adversarial timer tests exercise those paths.
4. **Retain unrelated mmap list nodes.** The profile exposed list destruction,
   allocation and lock work during ordinary `mmap`/`munmap`. Previously even
   disjoint insertion and whole-object removal rebuilt lists of unrelated
   mappings. Those cases now retain the nodes. Fixed replacements and partial
   splits keep the existing staged transaction. With changes 1–4, successful
   exploratory cold/warm compiles took **103.648 / 110.796 seconds**, with
   **63.602 / 68.483 seconds** of kernel CPU time. This was the first large win.
5. **Reuse CPU identity in spinlock bookkeeping.** Resolve the thread and CPU
   together after disabling interrupts, then reuse the held CPU identity when
   retiring its tracking entry. Lock diagnostics remain enabled. Bootstrap,
   recursive acquisition and scheduler handoff semantics are preserved.
   A sequential paused-profile run completed cold/warm compiles in
   **86.366 / 86.372 guest seconds** (89.943 / 89.922 host seconds), with
   **53.740 / 54.285 reported kernel CPU seconds**. Its compile phases each
   paused for approximately 2.6 seconds. The subsequent write-enabled control
   with sampling disabled reproduced approximately the same performance.
6. **Store reservation ranges contiguously.** Post-mmap profiles show repeated
   copying and destruction of hundreds of individually allocated reservation
   records. Replacing `RangeList`'s private vector of pointers with values
   removes per-record heap allocation while preserving its public API and the
   generation-checked reservation transaction. This reduced clean cold/warm
   compilation to **72.881 / 66.165 seconds** with writes enabled.
7. **Wake background workers when useful work exists.** Cache maintenance had
   woken its sleeping worker on each tick even when neither memory pressure nor
   the writeback deadline required it. Pressure is still checked each tick;
   healthy ticks now retain the wait until the 500 ms writeback deadline.
   POSIX processes now request deferred CPU reports only while virtual/profile
   timers are armed. Raw resource totals remain unconditional, and a separate
   coalesced request preserves five-second load-average sampling. The latest
   clean pair for this arm is **71.976 / 58.257 seconds**. These gates preserve actual CPU
   timer delivery and close the race between arming and report publication.

## What the profiles establish

Original successful compilation samples were approximately 89% running kernel
and 11% userspace. Both original warm compiles issued **zero disk read and write
operations**, yet still took about three minutes. Disk traffic cannot explain
that warm workload. CPU identity resolution, spinlock bookkeeping and allocator
work were prominent; deeper caller samples linked substantial allocation and
destruction work to mapping publication/removal.

After retaining mmap list nodes, the running-kernel sample fraction fell to
roughly 80–83%. Reservation snapshots became more visible. Register values
checked against the frozen disassembly showed snapshots copying 515, 545 and
569 dynamic free ranges. The profile, rather than the extent count alone,
motivates removing the per-record heap operations.

RTC IRQ8 arrived about 508–512 times per second, with PIT IRQ0 around 18.2 Hz.
The earlier arm's 57 direct MMIO samples were LAPIC EOI writes at offset `0xb0`,
not APIC-ID reads. The existing TSS-based CPU identification path is active.
The APIC local timer is not included in QEMU's IOAPIC/PIC counters, and those
two controllers can report the same hardware event: their counts must not be
added together.

Idle samples commonly observed `HLT`; the small idle sample sets do
not establish a precise idle duty cycle. Timer/worker activity remains visible,
but there is no evidence here that it explains the T420's fan speed. The RTC
rate experiment below improves compilation at the cost of coarser timer and
published-clock granularity.

### What became visible after the large allocation fixes

Across the three worker-gated compilation phases, the deferred accounting
worker appears in **0 of 3,707 CPU samples**, versus 186/1,619 cold and 196/1,626
warm before the range/worker changes. Cache worker descendants fell from
126/1,619 and 174/1,626 to 15/1,350 and 16/1,189. The union of reservation-range
frames fell from 198/1,619 and 186/1,626 to 23/1,350 and 21/1,189. These are
inclusive observations with overlaps; they demonstrate that the targeted work
became much less common, not additive percentages of recovered time.

The remaining link profile contains `readv` in 106/334 samples (31.7%) and
user-buffer validation frames in 83/334 (24.9%). `MemoryMapManager::allows`
appears in 33/334 (9.9%), despite zero backing-disk reads. It scans all mappings
even after finding an object that covers the entire requested range. An early
success return is a small next candidate; further changes must retain the
post-blocking revalidation that detects concurrent unmapping. Fault handling
also searches for a mapping twice, through `resolveUserFault` and `handleTrap`.

The profile also exposed IB700 watchdog refreshes at about 512 Hz. In the warm
phase, `IoPort::write16` accounts for 60/1,189 observations; register values
identify port `0x443` and timeout value `0x0a`. Tail calls hid the watchdog name
from those stacks. Most samples land immediately after the port instruction,
so QEMU stop-point bias may amplify this cost. An unprofiled ablation checked it.
The driver now refreshes once per elapsed second and uses byte-wide I/O.
The unprofiled watchdog-only pair at 512 Hz took **70.389 / 61.495 seconds**,
versus **71.976 / 58.257 seconds** before it. This does not establish an isolated
compilation speedup, despite the prominent I/O samples.
Shutdown now writes the disable register at `0x441`; the old offset instead
armed a 30-second timeout. These register semantics are documented by the
[Linux IB700 driver](https://raw.githubusercontent.com/torvalds/linux/master/drivers/watchdog/ib700wdt.c)
and implemented by the
[QEMU IB700 model](https://raw.githubusercontent.com/qemu/qemu/master/hw/watchdog/wdt_ib700.c).
A paired QMP control confirms this behavior with an actual emulated IB700:
both drivers survive 12 seconds while armed, but the old driver produces a
watchdog event 29.94 seconds after unload. The fixed driver survives the full
35-second post-unload interval without a watchdog event. An earlier instruction
trace was empty and is not used as evidence.

### RTC frequency

The RTC's **512 Hz** service interrupt is separate from the LAPIC scheduler's
**100 Hz** tick. No audited consumer requires exactly 512 Hz; the existing
BOCHS configuration already selects 64 Hz. The new `--rtc-hz=<rate>` boot
argument changes the runtime CMOS rate after the unchanged TSC calibration.
This permits a controlled 512/128/64 Hz comparison with one kernel binary.
The default remains unchanged; the lower rates are explicit boot selections.
At 128 Hz, the clean cold/warm pair took **55.795 / 52.023 seconds**, versus
**70.389 / 61.495 seconds** at 512 Hz with identical watchdog and kernel code.
That is **20.7% / 15.4% less wall time**. Host measurements agree (55.499 / 51.740
versus 70.046 / 61.194 seconds), so this is not a changed guest-clock scale.
QEMU counted 128.0 RTC interrupts per host second in both compilation phases.
The 64 Hz pair took **57.188 / 51.401 seconds**: cold was slower and warm only
1.2% faster than 128 Hz. These single pairs do not establish a further win at
64 Hz, so 128 Hz is the better starting point for balancing this workload and
timing granularity.

Across all three profiled compilations, RTC dispatch and descendants occur in
**90/2,973 observations (3.0%)**, versus **559/3,707 (15.1%)** in the earlier
worker-gated 512 Hz profile. The broader threaded IRQ worker falls from 22.0%
to 5.9%. This profile comparison includes the watchdog fix; the clean
same-kernel timing pair above isolates frequency.

The RTC acknowledges CMOS status C, advances time, delivers due alarm events,
ticks IRQ maintenance, and dispatches registered callbacks. The callback view
shows where some of that work went:

| Path beneath RTC | Worker-gated 512 Hz | 128 Hz plus watchdog fix |
| --- | ---: | ---: |
| Watchdog word-write leaf | 168/3,707 | 0/2,973 |
| Status-C read | 66/3,707 | 20/2,973 |
| Timer registry dispatch | 190/3,707 | 32/2,973 |
| vDSO snapshot refresh | 34/3,707 | 4/2,973 |
| Cache timer | 36/3,707 | 5/2,973 |
| Alarm event delivery | 22/3,707 | 8/2,973 |

These are overlapping sampled paths, not call counts or additive time savings.
No remaining callback dominates the small 128 Hz RTC sample set. The 128 Hz
link profile still finds buffer validation in 77/247 observations (31.2%),
including `allows` in 27/247, with no backing reads. That is a stronger next
target than another indiscriminate frequency reduction.

At 128 Hz, periodic alarm dispatch is quantized to approximately 7.8 ms rather
than 2 ms; at 64 Hz, approximately 15.6 ms. The vDSO currently exposes periodic
InfoBlock snapshots, so its visible clock updates become coarser too, although
kernel TSC-based time remains interpolated. USB connection polling and key
repeat also become coarser. Cache writeback, watchdog refresh, and timer
expiration logic use elapsed time rather than assuming a fixed tick count.
The full signal/timer/clock suite passes at both 128 and 64 Hz with one and four CPUs;
physical input responsiveness remains unmeasured. The 128 Hz arm includes
successful sync, actual disk writes, and the anonymous-memory contracts.
Fresh negative boots also reject an unsupported 65 Hz value and duplicate
rate arguments before runtime IRQ8 registration. Other supported rates are
available for experiments but were not functionally qualified in this pass.

### CPU identity and adaptive timing

The current CPU identity source is already the hardware task-register selector
(`STR`), not an APIC-ID MMIO read. Before the final follow-up, the steady lookup
made out-of-line calls and fetched the selected processor entry twice. The
change reuses the validated pointer and inlines the small lookup. Generated
code confirms inline `STR` and one vector lookup; the exported selector getter
and boot fallbacks remain intact. Clean timing was **55.734 / 52.320 seconds**,
essentially unchanged from **55.795 / 52.023** before this small simplification.
It is not counted as another measured speedup.
A new instruction path needs more care: `RDPID` is absent from the selected
SandyBridge QEMU model; `RDTSCP` also reads/orders the timestamp and is not
necessarily cheaper. The existing `TSC_AUX` value is a dense index, whereas
`Processor::id()` returns firmware identity, and early BSP initialization can
precede final topology discovery. A fast path must validate and publish final
per-CPU AUX state before trusting it. No new AUX/GS identity ABI is introduced.

The prominent MMIO leaf is still interrupt acknowledgement: registers and
disassembly identify 169/171 sampled 32-bit device writes across compilation,
codegen and linking as LAPIC EOI, with the other two from AHCI. Of those LAPIC
samples, 166 stop immediately after the store. These are required acknowledgements;
their concentration reinforces the QEMU stop-point-bias limitation, rather than
establishing slow CPU identity reads or removable interrupt work.

Adaptive RTC service is plausible, but an empty RTC alarm queue is insufficient:
`timerfd`, POSIX timers and `ITIMER_REAL` hold deadlines elsewhere, and disarmed
process timers remain registered. A bounded future policy could retain 128 Hz
normally and promote to 512 Hz near actual deadlines, with explicit demand from
other fine-timing consumers and held-key repeat. Producers must publish demand;
some alarm-arm paths hold wait locks with interrupts masked and cannot safely
program CMOS themselves. The threaded worker would program/read back the rate,
recheck a demand generation before reducing it, and use brief hysteresis.

Promotion driven only by the next low-rate tick still adds up to one 7.8 ms
interval to a newly armed short timer. Retaining fine latency from the first arm
needs an immediate deferred-work kick or a one-shot clock-event path. The current
LAPIC timer already owns the 100 Hz scheduler tick, and there is no HPET timer
driver here; converting either to a deadline-driven service is a separate slice.
Required checks include arm/downshift races, independent timer ownership,
shutdown, short sleeps/overruns, realtime changes, input repeat/hotplug, pressure,
writeback and watchdog behavior on one and four CPUs.

### Syscall count

An opt-in `PEDIGREE_SYSCALL_COUNTER` build counts once at the POSIX syscall
dispatch boundary and folds each terminated child's own and already-reaped
descendant counts into its parent. The benchmark queries the accumulated count
after `wait4`, outside the timed interval. This gives the complete child-side
`gcc` invocation, including `execve`, `cc1plus`, assembler and linker descendants,
while excluding the benchmark supervisor's `fork` and `wait4`.

Two fresh one-vCPU, 128-Hz, writes-disabled quick runs produced the same counts:

| Phase | Syscalls | Reported kernel seconds |
| --- | ---: | ---: |
| Cold `gcc -o which which.cc -lstdc++` | 124,005 | 30.304 |
| Warm `gcc -o which which.cc -lstdc++` | 124,009 | 26.506 |
| `./which gcc` | 18 | 0.019 |
| Anonymous-memory contract | 812 | 0.175 |

So the successful GCC run is approximately **124 thousand syscalls**, with only
four calls separating the cold and warm runs. The counter itself adds an atomic
increment and the diagnostic timings are not controls; the stable count is the
result to retain. At that call volume, removing 1, 5 or 10 microseconds from
every entry/exit would save at most about 0.12, 0.62 or 1.24 seconds,
respectively. The approximately 30-second reported kernel interval therefore
points to substantial work inside syscall bodies, faults, I/O and scheduling in
addition to the raw entry/return boundary. The reported system interval is still
the TSC-derived Pedigree accounting described below, so its quotient by syscall
count is a sizing signal rather than a per-call CPU measurement.

The same diagnostic records a latency histogram at the POSIX dispatch boundary,
from entry to return from `PosixSyscallManager::syscall`. It includes dispatch,
syscall bodies, faults, blocking and descheduling that occur before that return;
it excludes the architecture entry stub and final return tail. The buckets are
`<1`, `1-2`, `2-4`, ... microseconds, ending at `>=16.384 ms`. The useful
grouping was:

| Duration range | Cold | Warm |
| --- | ---: | ---: |
| `<64 us` | 39,974 (32.236%) | 45,533 (36.717%) |
| `64-256 us` | 74,408 (60.004%) | 67,273 (54.248%) |
| `256-512 us` | 7,483 (6.034%) | 9,422 (7.598%) |
| `512 us-1.024 ms` | 1,369 (1.104%) | 1,324 (1.068%) |
| `>=1.024 ms` | 771 (0.622%) | 457 (0.369%) |

This is a real long tail, but not a universal fixed entry cost: roughly 54-60% of
calls land in `64-256 us` and another 32-38% are below `64 us`; 7.2-9.0% take
at least `256 us` and hundreds reach the millisecond range. That makes a single
entry/exit fast path unlikely to explain the whole slowdown. The current
histogram is count-only; the raw bucket counts are retained in the report, and
the next useful refinement would be per-bucket elapsed-time totals plus
syscall-number attribution so the tail can be assigned to specific operations.

## Profiler design and limits

The host samples QEMU CPU registers, identifies userspace, kernel execution
and halted CPUs, and resolves kernel/module addresses against frozen ELF files.
This can observe IRQ-disabled kernel work. A timer-IRQ-only sampler would have
a blind spot precisely where many of these locks execute.

The initial eight-frame stack walks were asynchronous and sometimes raced with
scheduling. Their valid instruction-pointer samples remain useful, but caller
percentages are approximate. The newer mode pauses all CPUs, captures registers
and up to 24 frame-pointer entries, then resumes in `finally`. It records query
and pause durations. Timings with sampling disabled remain the performance
control. Frame-pointer walks can still omit leaf callers and stop at interrupt
or assembly boundaries; they are not a complete DWARF unwinder.

Module addresses are reconstructed from the frozen initrd and the x64 loader's
allocation rule, then checked against every retained runtime module anchor.
Missing/mismatched anchors are errors. DWARF resolves static functions absent
from `nm`. Synthetic thread-start returns are labeled as roots, not attributed
to a neighboring function. Caller PCs outside verified executable sections are
discarded with explicit omission counts. Flat percentages use all CPU samples;
inclusive stack entries overlap and must not be summed. The comparison
heatmap uses exclusive instruction-pointer counts and conserves all CPU samples;
the HTML flamegraph provides the inclusive caller view.

Paused captures obtain caller chains for approximately 95–98% of running
kernel samples in the retained compilation phases. This does not make sampling
equivalent to CPU-time accounting:
the cold run observed 15.8% userspace samples while `getrusage` reported user
time equal to 25.1% of guest wall time. Randomizing the sample interval
from 0.5 to 1.5 times 50 ms did not eliminate the gap: worker-gated 512 Hz cold/warm samples
were 21.3% / 21.1% userspace, versus reported user time of 30.3% / 30.5% of guest
wall time. At 128 Hz those proportions are 25.3% / 26.2% sampled, versus
31.1% / 31.4% reported. Fixed-interval aliasing is not a sufficient explanation.
QEMU stop-point bias and accounting attribution remain possible contributors.
Use these profiles to locate and compare work, not to multiply percentages by
wall time and claim precise function durations. The reported fault, block and
context-switch fields in `rusage` are unimplemented zeros; I/O conclusions use
QEMU's block counters instead. CPU accounting uses a TSC-derived wall clock and
charges interrupt/return work to the interrupted process. Host descheduling
inside a kernel interval can therefore appear as system time, especially on
SMP. Four-CPU mode attribution was not independently established and is not
used to infer exact compiler CPU consumption.

## Reproduction and evidence

See [the benchmark guide](../scripts/benchmarks/compile-latency.md) for image
preparation, quick/full runs, paused profiling, symbolization and persistence
verification. The source remains unchanged in the fixture. All outputs and
logs are under `/tmp/pedigree-which-perf-20260914`, with short QEMU output paths:

| Directory | Evidence |
| --- | --- |
| `/tmp/wperf-b15-1` | Original sampled compilation; deadline reached during stage breakdown |
| `/tmp/wperf-a15-2` | Scheduler/page-index arm; all compiler stages finished, sync returned EIO |
| `/tmp/wperf-m1` | mmap fastpath arm; successful cold/warm compilation, sync returned EIO |
| `/tmp/wperf-bclean1` | Original kernel, sequential run, sampling off, explicit sync omission; PASS |
| `/tmp/wperf-lprof1` | Lock-identity reuse, coherent paused stacks, mapping contracts; PASS |
| `/tmp/wperf-lwrite1` | mmap/lock changes, writes enabled, sampling off; PASS |
| `/tmp/wperf-lreboot1` | Reboot of that same overlay, binary hash and execution; PASS |
| `/tmp/wperf-rwrite1` | Contiguous reservation ranges, writes enabled, sampling off; PASS |
| `/tmp/wperf-rreboot1` | Reboot after range-storage changes, binary hash and execution; PASS |
| `/tmp/wperf-wclean1` | Worker gates, writes enabled, sampling off; PASS |
| `/tmp/wperf-wprof1` | Worker-gated paused/jittered profile, full compiler and memory stages; PASS |
| `/tmp/wperf-w4` | Worker-gated four-CPU compilation/writeback/mapping qualification; PASS |
| `/tmp/wperf-wreboot1`, `/tmp/wperf-wreboot4` | Binary identity and execution after one/four-CPU reboots; PASS |
| `/tmp/wperf-cpu-timers1`, `/tmp/wperf-cpu-timers4` | Native virtual/profile CPU timer contracts; PASS |
| `/tmp/wperf-wdt512` | Watchdog fix alone, 512 Hz, clean run, writes enabled; PASS |
| `/tmp/wperf-rtc128` | Same code, 128 Hz, clean run, writes enabled; PASS |
| `/tmp/wperf-rtc64` | Same code, 64 Hz, clean run, writes enabled; PASS |
| `/tmp/wperf-rtc128-prof` | 128 Hz paused/jittered profile, all compiler/memory stages; PASS |
| `/tmp/wperf-rtc128-signals1`, `/tmp/wperf-rtc128-signals4` | Full signal/timer/clock contracts at 128 Hz; PASS |
| `/tmp/wperf-rtc64-signals1`, `/tmp/wperf-rtc64-signals4` | Full signal/timer/clock contracts at 64 Hz; PASS |
| `/tmp/wperf-watchdog-qmp-new` | Fixed IB700 survives armed refresh and post-unload wait; PASS |
| `/tmp/wperf-watchdog-qmp-old` | Negative control reproduces watchdog expiry after old unload; PASS |
| `/tmp/wperf-cpuid128` | Final identity simplification, 128 Hz, sampling disabled; PASS |
| `/tmp/wperf-cpuid-signals1`, `/tmp/wperf-cpuid-signals4` | Final kernel, full native signal/timer/clock suite; PASS |
| `/tmp/wperf-cpuid-reboot1` | Final kernel, saved binary hash and execution after reboot; PASS |
| `/tmp/wperf-watchdog-qmp-new4`, `/tmp/wperf-watchdog-qmp-old4` | Four-CPU watchdog positive/negative pair, promoted runner; PASS |
| `/tmp/pedigree-which-perf-20260914/rtc-negative` | Unsupported/duplicate RTC boot-option rejection; PASS |
| `/tmp/pedigree-syscall-counter-20260914/run1`, `/tmp/pedigree-syscall-counter-20260914/run2` | Opt-in syscall-count quick runs; PASS |
| `/tmp/pedigree-syscall-histogram-20260914/run2`, `/tmp/pedigree-syscall-histogram-20260914/run3` | Opt-in syscall-latency histogram quick runs; PASS |

The target kernel and initrd were rebuilt with `cmake --build build --target
kernel initrd -j8`, including all consumers after shared layout changes. The
focused native utility suite passed **198 tests from 28 suites**, covering
cache, range, tree, lock, deferred-accounting, load-average and interval-timer
logic. Darwin hosted core and swap/VM contracts also passed. The hosted core
exercises the actual cache worker, accounting arm catch-up, load-request races,
runnable-current scheduling, timer-handler retirement and shutdown.

Native one/four-CPU signal suites exercise actual POSIX timer delivery; the
Linux-only hosted POSIX syscall fixture was not run on macOS. Compilation,
mapping contracts and binary persistence were separately qualified on one and
four CPUs through the worker-gating changes. The final identity simplification
passed compilation/persistence on one CPU and full signal/timer/clock contracts
on one and four; its four-CPU compilation/persistence repeat remains pending.
Formatting checks cover 33 changed C/C++ files, and the Python tools
have syntax, CLI and output-conservation checks. Interactive hardware input,
physical power consumption and T420 runtime remain outside this QEMU pass.

The initial `CRIPPLE_HDD=TRUE` driver tried `fsync`; it returned `EIO`, and the
failed runs are retained. The revised performance-only mode explicitly omits
sync and continues into the memory contracts. It does not claim persistence.
Write-enabled qualification requires successful sync and a fresh boot that
checks the saved binary against the prior run's size/hash and executes it
without recompiling. The shared build originally had writes enabled; that
setting has been restored (`PEDIGREE_CRIPPLE_HDD=FALSE`).

Other retained setup failures include missing original GCC dependencies, an
unsupported serial termios operation (now handled explicitly), and an initrd
mistakenly installed as gzip where the direct UEFI loader needed raw tar.
These are separated from compiler performance results.
