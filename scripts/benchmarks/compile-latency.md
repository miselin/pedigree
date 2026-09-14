# Compilation latency benchmark

`compile-latency.c` compiles the repository's `which` application inside Pedigree.
`run-compile-latency.py` boots a prepared image, admits each phase through COM1,
and retains timings, exit status, serial output, QMP block counters, and IRQ
counters. Run one guest at a time. Use fresh disposable overlays for each arm,
and freeze their backing images, kernels, initrds, symbols, and compiler files.
These QEMU TCG comparisons do not establish physical T420 timings.

## Prepare the image

Build the two static guest programs with the configured userspace cross compiler.
The musl sysroot here is **`build/musl/usr`**, including its `include` and `lib`
directories:

```sh
compilers/dir/bin/x86_64-pedigree-gcc --sysroot="$PWD/build/musl/usr" \
  -static -O2 -std=c11 -Wall -Wextra -Werror \
  scripts/benchmarks/compile-latency.c -o /tmp/compile-latency
compilers/dir/bin/x86_64-pedigree-gcc --sysroot="$PWD/build/musl/usr" \
  -static -O2 -std=c11 -Wall -Wextra -Werror \
  scripts/benchmarks/anonymous-contract.c -o /tmp/anonymous-contract
```

In an offline disposable ext2 root, install the driver as `/usr/bin/init`, mode
0755. Create `/root/compile-bench` and install:

| Guest path beneath `/root/compile-bench` | Source or purpose |
| --- | --- |
| `which.cc` | `src/user/applications/which/main.cc` |
| `anonymous-contract` | The static contract executable above, mode 0755 |
| `link-cxx` | Optional empty marker: start with the known successful `-lstdc++` command |
| `quick-run` | Empty marker for the shorter suite; also requires `link-cxx` |
| `no-sync` | Empty marker for a performance-only run with disk writes disabled |
| `persist-check` | Optional empty marker to create a persistence sentinel during sync |

Do not seed `persisted-output`: the driver creates it from the compiled binary.
On its next boot, an existing sentinel with `persist-check` selects verification
instead of compilation. The `no-sync` marker disables that persistence behavior.

The compiler must run natively in the guest. The current driver environment sets
`PATH=/usr/bin:/bin`, `LC_ALL=C`, `HOME=/root`, `TMPDIR=/tmp`, and
`CPLUS_INCLUDE_PATH=/usr/include/c++/15.3.0:/usr/include/c++/15.3.0/x86_64-pedigree`.
It does not set `LD_LIBRARY_PATH`. Keep this environment identical across arms;
update the include paths if selecting a different native GCC package.

For the GCC 15.3.0 fixture, install its driver, `cc1plus`, `collect2`, target
headers/start files, assembler/linker, and the complete `DT_NEEDED` library
closure. This includes the matching GMP, MPFR, MPC, zlib, and musl libraries and
their guest symlinks. Check unresolved dynamic symbols as well as library names.
Keep matching `libstdc++.a` and `libsupc++.a` in
`/usr/lib/gcc/x86_64-pedigree/15.3.0` when an older image supplies an incompatible
system `libstdc++.so`. A compiler driver that starts successfully does not prove
that `cc1plus` can load or that its headers and linker inputs match.

Check the offline root with `e2fsck -fn`, and put it back at its original image
partition offset. Install the intended kernel and **uncompressed tar initrd**
beside the selected loader on the ESP, normally
`EFI/PEDIGREE/current/{kernel,initrd.tar}`. The direct UEFI loader passes initrd
bytes through; a gzip file named `initrd.tar` is still compressed. Use the raw
archive produced by `scripts/create_initrd.py --uncompressed`, and verify the
installed bytes rather than trusting the filename. Preserve the root selector
in the adjacent `cmdline` file and add a separate `--disable-log-to-serial` token
without a trailing newline. This retains kernel log generation while keeping
benchmark records intact. Freeze the exact raw archive and matching module
symbol files for later profiling.

## Run and compare

For a short run with `quick-run`, `link-cxx`, and `no-sync` installed:

```sh
uv run --no-project python scripts/benchmarks/run-compile-latency.py \
  --image /path/to/frozen.img --output /tmp/compile-arm1 \
  --firmware-code /path/to/OVMF.fd --cpus 1 --quick --skip-sync --timeout 500
```

Use `--firmware-vars` for split OVMF firmware. Every output directory must be new.
The default creates `disk.qcow2` against the immutable image. Retained artifacts
include `report.json`, `serial.log`, `qemu.log`, `command.json`, and
`samples.jsonl`; failures retain their partial measurements. The timeout covers
boot and all phases. It is a bound, not a promise that compilation will finish
within 500 seconds.

Quick mode measures idle, a dependent integer CPU control,
`gcc -o which which.cc -lstdc++`, one warm repeat, `./which gcc`, sync when
enabled, and `anonymous-contract 1`. Default full mode first runs the exact
`gcc -o which which.cc`, retains its exit status, and adds `-lstdc++` if it fails.
It then measures two warm repeats, preprocessing, code generation, assembly,
linking, execution, sync, anonymous write faults at 1/4/16/64 MiB, and the contract.
`link-cxx` plus host `--linked` skips the initially unlinked attempt in full mode.
Keep guest markers and host options in agreement; unexpected phases fail closed.

Use `PEDIGREE_CRIPPLE_HDD=TRUE` with `no-sync` and `--skip-sync` to isolate the
performance work from disk writeback. The report explicitly labels the omitted
sync as `writes-disabled-performance-only`; unrelated errors still fail. This
mode cannot establish persistence. Remove `no-sync`, omit `--skip-sync`, and
build with `PEDIGREE_CRIPPLE_HDD=FALSE` for a write-enabled arm. Restore the
shared build's original setting after that arm, retaining its frozen artifacts.

For persistence, seed `persist-check` before the first write-enabled boot. After
that run passes and QEMU exits, reboot the same disposable overlay with new logs:

```sh
uv run --no-project python scripts/benchmarks/run-compile-latency.py \
  --image /path/to/frozen.img --output /tmp/compile-reboot \
  --firmware-code /path/to/OVMF.fd --cpus 1 --verify-persisted \
  --reuse-overlay /tmp/compile-arm1/disk.qcow2 --timeout 180
```

Use the actual successful write-enabled output in `--reuse-overlay`. The runner
checks its prior report and backing-image identity. The fresh guest reads the
sentinel, checks the binary's size and FNV-1a checksum against both the sentinel
and prior report, then executes `./which gcc`. It does not rewrite the sentinel
or recompile. A missing sentinel stops at the unexpected initial gate. The OS
may still perform its own boot writes. Same-boot reads or successful cached
compilation alone do not establish disk persistence.

## RTC-rate experiments

Use the same kernel, initrd, compiler, and workload with separate frozen image
copies. In the ESP `cmdline` file described above, add exactly one standalone
`--rtc-hz=128` or `--rtc-hz=64` token, preserving the other boot arguments and
omitting a trailing newline. Replace the token between arms. The option accepts
the RTC table's powers of two from 4 through 8192; malformed, unsupported, or
repeated values reject boot. Without the option, the default remains 512 Hz
(64 Hz for BOCHS builds).

The override changes runtime interrupts after the unchanged TSC calibration.
Confirm the kernel's `RTC: periodic interrupt frequency ... Hz` message and
measure IRQ8 deltas. The LAPIC scheduler remains independently at 100 Hz.
At 128/64 Hz, timer service and vDSO clock refresh occur about every
7.8125/15.625 ms. Kernel monotonic accounting still uses the TSC, but alarm
delivery, voluntary cache-pressure checks, USB connection polling, and key
repeat become less frequent. Qualify timer deadlines/overruns, timerfd, sleep,
USB port changes, and writeback on one and four CPUs alongside the timing arm.

## Watchdog qualification

Build the standalone watchdog fixture with the same static userspace toolchain:

```sh
compilers/dir/bin/x86_64-pedigree-gcc --sysroot="$PWD/build/musl/usr" \
  -static -O2 -std=c11 -Wall -Wextra -Werror \
  scripts/benchmarks/watchdog-contract.c -o /tmp/watchdog-contract
```

Install it as `/usr/bin/init`, mode 0755, in a separate disposable image with the
`ib700_wdt` module in its initrd. Run the candidate image with:

```sh
uv run --no-project python scripts/benchmarks/run-watchdog-contract.py \
  --image /path/to/watchdog-new.img --output /tmp/watchdog-new \
  --firmware-code /path/to/OVMF.fd --cpus 1
```

Use `--firmware-vars` for split OVMF firmware. The runner creates a fresh overlay,
enables `-device ib700 -watchdog-action pause`, and allows at most 120 seconds
after starting the VM; setup and cleanup have separate bounded waits. It retains
`command.json`, `serial.log`, `qemu.log`, raw messages in `qmp.jsonl`, and
`report.json` with event times and serial phases. Exceeding 64 MiB in any log
fails the run and triggers cleanup.
The fixture's own deadline is 65 seconds. A positive run requires all of:

```text
WATCHDOG-CONTRACT: PASS armed-refresh elapsed_us=...
WATCHDOG-CONTRACT: PASS unload
WATCHDOG-CONTRACT: PASS disabled-after-unload elapsed_us=...
WATCHDOG-CONTRACT: END PASS
```

The armed phase lasts at least 12 seconds against the configured 10-second
watchdog. After unloading the driver, the fixture waits another 35 seconds.
Retain QMP events and require no `WATCHDOG` event for this positive run.

Pair it with the same fixture in a frozen image containing the old watchdog
driver, whose unload write mistakenly arms a 30-second timeout:

```sh
uv run --no-project python scripts/benchmarks/run-watchdog-contract.py \
  --image /path/to/watchdog-old.img --output /tmp/watchdog-old \
  --firmware-code /path/to/OVMF.fd --cpus 1 --expect-expiry
```

This control passes only on an actual QMP `WATCHDOG` event with action `pause`
during `disabled-after-unload`, after the armed phase and successful unload.
Early expiry, missing expiry, guest failure, timeout, or failed QEMU cleanup
fails the run. The event's elapsed time after unload is recorded. Keep the VM
running until expiry; QMP `stop` also pauses the watchdog's virtual clock.
Repeat with `--cpus 4` and new output directories. Retain both outcomes: survival
alone cannot distinguish a working disable path from a device never armed, and
an empty I/O trace does not establish refresh counts.

## Profile and symbolize

First retain a run with sampling disabled (`--sample-interval 0`, the default).
For profiling, add these options to the runner command above:

```sh
--sample-interval 0.05 --sample-jitter --sample-stacks --paused-samples
```

`--sample-jitter` chooses each delay uniformly from 0.5–1.5 times the interval,
using fixed seed 0. The delay starts after the preceding query completes. This
reduces alignment with periodic kernel work; the report retains the interval,
jitter setting, and seed. It requires a positive sampling interval.

Paused sampling stops all CPUs, walks up to 24 kernel frame pointers, and resumes
in `finally`. Without `--paused-samples`, CPUs keep running and walks stop at
eight frames; these asynchronous chains can race with scheduling. Omit both
`--sample-stacks` and `--paused-samples` for register-only samples. Walks require
kernel frame pointers, stop at noncanonical/nonmonotonic frames, and can truncate
at interrupt boundaries.

`paused_wall_s` measures acknowledged stop to resume request. Stop/resume query
latencies are retained separately; `query_wall_s` includes the complete exchange,
so do not add these durations together. Sampling perturbs scheduling and timing;
the viewer does not subtract overhead from guest or host times. Use the separate
sampling-disabled run to measure speedups.

```sh
uv run --no-project python scripts/benchmarks/summarize-compile.py /tmp/compile-arm1 \
  --kernel /path/to/frozen/kernel.debug --module-dir /path/to/frozen/modules \
  --initrd /path/to/frozen/initrd.tar.uncomp \
  --write-module-map /tmp/compile-arm1/module-map.json \
  --output /tmp/compile-arm1/summary.json
```

`--initrd` also accepts the corresponding gzip archive. It reconstructs x64
module allocations in archive order, using the configured
`0xffffffff90000000` module base and the loader's page-rounded sum of
`PT_LOAD.p_vaddr + p_memsz`. It reads module names from ELF, requires at least two
retained runtime anchors, and checks every observed start/end before using the
map. Mismatches fail rather than shifting addresses to fit. The saved manifest
records hashes and its inference rule; `--module-map` can reuse it with the same
runtime checks. Other architectures or module loading schemes need their own
mapping method.

The symbolizer prefers `.debug`, accepts explicit `--module NAME=FILE` entries,
and falls back to matching `.o` files. It records symbol-file hashes. Sized `nm`
symbols use their actual bounds; zero-sized assembly symbols have explicitly
inferred next-symbol bounds. Batched `addr2line` resolves remaining DWARF names,
including static helpers omitted from `nm`; choose tools with `--nm` and
`--addr2line`. ET_REL section relocation remains unsupported.

The seeded `Thread::threadExited` return becomes an explicit synthetic thread
root, excluded from hot-function counts. Caller chains stop before addresses
outside known executable ELF sections or modules without symbol metadata;
omitted, invalid, and unverified callers are counted separately.

After summarizing both runs, export an offline interactive HTML viewer and a
static SVG heatmap:

```sh
uv run --no-project python scripts/benchmarks/render-compile-profile.py \
  --summary 'Baseline=/tmp/compile-baseline/summary.json' \
  --summary 'Candidate=/tmp/compile-arm1/summary.json' \
  --phase 'compile*' --output /tmp/compile-profile.html \
  --heatmap-svg /tmp/compile-heatmap.svg
```

The HTML provides function search, phase selection, and click-to-zoom kernel
flamegraphs without external scripts. Keep companion `samples.jsonl`,
`report.json`, and frozen symbols at the paths recorded by the summaries: the
renderer uses complete raw samples when available. `--summary-only` uses retained
aggregates and explicitly labels omitted leaves and stacks. Run metadata,
sample counts, unwind gaps, and profiler durations remain visible in the viewer.

Heatmap cells count exclusive sampled functions. Both their percentages and
flamegraph percentages use **all CPU samples in that phase** as the denominator,
even when zoomed: each observed CPU contributes one sample per snapshot. User
execution, halted CPUs, unknown addresses, and missing stacks remain distinct;
user-mode functions are not symbolized. Stack percentages in the JSON summary's
`percent_stack_samples` fields instead use attempted stacks, and inclusive caller
counts are not additive. None of these percentages is an exact CPU-time measure.

IRQ counts from the IOAPIC and PIC may describe the same event and must not be
added together. QEMU block service times can overlap and do not directly measure
time the compiler was blocked. Guest `wait4` user/system accounting, host time,
I/O deltas, and sample attribution provide complementary evidence.
