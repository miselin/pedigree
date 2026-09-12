# vfork performance comparison

This experiment compares warm process creation and child completion on amd64.
Variant A uses true `vfork`; variant B maps Linux syscall 58 to ordinary `fork`.
Both run the same `vfork-benchmark` executable and libc `vfork()` call. Only the
syscall mapping changes; `fork`, `execve`, scheduling, and memory management stay fixed.

## Prepare the variants

Build the current kernel, POSIX module, initrd, and `app-vfork-benchmark` target
with the normal amd64 configuration. Keep that completed build idle throughout
preparation. Its `compile_commands.json`, module link command, and tool paths
must be available. From the repository root, choose a new output directory
outside both the checkout and build directory:

```sh
uv run --no-project python scripts/prepare-vfork-comparison.py \
  --build-dir build \
  --output-dir /private/tmp/pedigree-vfork-comparison
```

The helper copies the original into `true-vfork/`, then compiles a shadow POSIX
dispatcher with the syscall-58 mapping changed from `POSIX_VFORK` to `POSIX_FORK`.
It relinks `fork-mapped/posix.o` and replaces only that initrd member. It checks
that original inputs and unrelated initrd members remain unchanged, recording
commands, hashes, source state, and the mapping diff in the output directory.
Require `provenance.json` to report `status: complete` before using its artifacts.
The helper prepares modules and initrds, without building boot media or running QEMU.

Create separate boot media using the two prepared initrds and the same kernel,
benchmark binary, libraries, and disk contents. Use a fresh disposable disk
snapshot per boot. Record QEMU version, machine, CPU model/count, memory, TCG
settings, kernel configuration, media hashes, and host conditions with the logs.
Run sequentially in a balanced A-B-B-A order; do not run competing guests or
modify their backing images during measurement.

## Run the benchmark

Start with a short pilot in each variant:

```sh
vfork-benchmark --iterations 3 --samples 2 --warmup 1 --sizes 0,16 --expect shared
vfork-benchmark --iterations 3 --samples 2 --warmup 1 --sizes 0,16 --expect fork
```

Use `shared` only for A and `fork` only for B. An untimed shared-memory write
probe always calls `vfork`, independently of `--call`; a mismatch fails the run.
Each child must exit with the expected status. Retain the full serial output,
process exit status, and `VFORK-BENCH: PASS` marker; failed or incomplete runs
are not samples.

Defaults are 30 iterations per sample, five samples, three warmup iterations,
0/16/64 MiB payloads, and both `exec` and `exit` modes. Set `--iterations N`,
`--samples N`, `--warmup N`, `--sizes 0,16,64,128`, or `--mode exec|exit|both`
as needed, with a space between every option and value. Select a fixed iteration
count after the pilots so the shortest batches last at least about 250 ms;
use the same count and options in both variants. Keep pilots out of the results.

Run `--call fork --sizes 0` as a control on each boot, retaining the appropriate
`--expect shared` or `--expect fork`. This exercises an unchanged creation path
and helps reveal drift between boots. Use `--call vfork` for the main comparison.

## What is measured

The executable and loader are warmed before sampling. The timed child only
calls `execve` with precomputed arguments/environment, or `_exit`; the parent
validates and reaps every child with `waitpid`. The `exec` worker immediately
exits after program startup. Each batch therefore includes creation, scheduling,
optional exec/startup, exit, and reaping. It is not syscall-return latency.
Successful timed loops contain no output or payload allocation. With both modes
selected, their order alternates between samples.

Payload sizes describe extra private anonymous memory touched once per page
before warmup, in addition to the program's baseline memory. They are not measured
total RSS. The parent leaves those pages untouched during sampling. Fork warmup
can leave its page-table entries marked copy-on-write, so this measures a steady
state with a quiescent payload, not a parent that dirties every page between spawns.

Wall endpoints use `syscall(SYS_clock_gettime, CLOCK_MONOTONIC, ...)` to sample
current kernel ticks rather than a potentially older vDSO timer snapshot.
CSV rows contain aggregate `wall_ns` and parent/children user/system CPU
microsecond deltas from `getrusage`; divide by `iterations` for per-child values.
Children's usage is sampled after reaping. CPU snapshots bracket a slightly wider
interval than the wall endpoints. CPU totals are supported; maximum RSS, page
faults, and context-switch counters are currently zero placeholders and cannot
support conclusions. The benchmark labels RSS/fault counters unsupported.

The maintained Bash 5.3.15 job-control path calls `make_child` and `fork`, so this
syscall-58 experiment does not establish a shell-launch improvement. Musl's
`posix_spawn` uses the syscall-56 `clone(CLONE_VM | CLONE_VFORK | SIGCHLD)` route,
which remains unchanged in both variants. Relevant code is in
`src/user/applications/vfork-benchmark/main.c`, the amd64 mapping under
`src/modules/subsys/posix/syscalls/`, and `system-syscalls.cc` in that subsystem.

QEMU TCG measurements describe this emulated workload and configuration. Report
per-boot distributions and the fork control alongside any ratio; they cannot establish
a physical laptop speedup or general application performance.

## Results

Recorded on 2026-09-12 using QEMU 11.1.1 on macOS 26.6.2/arm64. The guest
used Q35, the default x86-64 CPU model, 512 MiB RAM, UEFI, and TCG with one
execution thread for the one-CPU runs and multiple threads for four CPUs.
The kernel was a Debug build with debug logging and lock tracking enabled.
POSIX verbose tracing was disabled. Guests ran sequentially; the host remained
interactive and was not pinned or thermally controlled. These are emulation
measurements, not physical laptop timings.

Each CPU configuration uses A-B-B-A boots, five batches per cell per boot,
three warmup iterations, and 15 iterations per exec batch. The one-CPU suite
also uses 50 iterations per exit-only batch. Both variants have identical
kernel, root contents, libraries, and initrd members except the POSIX dispatcher
mapping. Every run uses a fresh disposable QEMU disk snapshot.

Values below are medians of ten batch averages per cell, in milliseconds per
completed child. Brackets show the minimum and maximum batch averages, not
individual-child tail latency. There are only two included boots per variant;
these ranges are descriptive, not confidence intervals.

### One guest CPU

| Call / child action | Extra parent MiB | True-vfork build, ms [range] | Fork-mapped build, ms [range] | B/A time ratio |
| --- | ---: | ---: | ---: | ---: |
| fork / exec (control) | 0 | 26.82 [26.33–27.41] | 26.57 [26.04–26.89] | 0.99× |
| vfork / exec | 0 | 23.02 [22.16–26.16] | 26.74 [25.58–29.66] | 1.16× |
| vfork / exec | 16 | 22.96 [22.54–24.34] | 66.72 [65.18–68.72] | 2.91× |
| vfork / exec | 64 | 22.98 [22.22–24.48] | 185.09 [182.55–191.82] | 8.06× |
| vfork / exit | 0 | 6.94 [6.31–8.12] | 9.45 [9.28–10.17] | 1.36× |
| vfork / exit | 16 | 6.51 [6.19–7.09] | 48.61 [48.32–50.83] | 7.46× |
| vfork / exit | 64 | 6.39 [6.18–7.87] | 163.90 [162.10–166.41] | 25.65× |

### Four guest CPUs

| Call / child action | Extra parent MiB | True-vfork build, ms [range] | Fork-mapped build, ms [range] | B/A time ratio |
| --- | ---: | ---: | ---: | ---: |
| fork / exec (control) | 0 | 73.17 [70.60–76.13] | 71.89 [70.13–75.18] | 0.98× |
| vfork / exec | 0 | 49.69 [48.19–51.88] | 74.39 [71.61–75.99] | 1.50× |
| vfork / exec | 64 | 50.02 [47.30–51.94] | 1206.06 [1181.62–1263.46] | 24.11× |

### Boot medians and interpretation

Each pair below lists the two included boot medians, in milliseconds. A is
true vfork; B is the fork mapping.

| CPUs | Call / child action | MiB | A boot medians | B boot medians |
| ---: | --- | ---: | --- | --- |
| 1 | fork / exec | 0 | 26.69, 26.90 | 26.49, 26.61 |
| 1 | vfork / exec | 0 | 22.96, 23.08 | 27.40, 26.47 |
| 1 | vfork / exec | 16 | 22.99, 22.87 | 66.78, 65.95 |
| 1 | vfork / exec | 64 | 22.98, 22.97 | 187.58, 183.01 |
| 1 | vfork / exit | 0 | 7.25, 6.56 | 9.50, 9.42 |
| 1 | vfork / exit | 16 | 6.66, 6.45 | 48.39, 48.62 |
| 1 | vfork / exit | 64 | 6.55, 6.35 | 163.11, 164.75 |
| 4 | fork / exec | 0 | 74.89, 70.91 | 72.71, 70.82 |
| 4 | vfork / exec | 0 | 51.22, 48.66 | 74.75, 73.19 |
| 4 | vfork / exec | 64 | 50.98, 49.03 | 1239.59, 1185.27 |

The one-CPU exec workload saves 13.9%, 65.6%, and 87.6% at the baseline,
16 MiB, and 64 MiB payloads. Total accounted parent-plus-child CPU time at
64 MiB is 17.59 ms versus 161.54 ms per child. The unchanged fork control
differs by 0.9% between one-CPU variants and 1.8% between four-CPU variants.

True vfork stays nearly flat as this parent payload grows. That is consistent
with avoiding fork's page-table/CoW setup and inherited-address-space teardown;
ordinary fork already uses CoW, so this is not evidence of copying every payload
byte. Both paths still create process/thread and POSIX state. The larger SMP
ratios are specific to this TCG/debug configuration and must not be interpreted
as native SMP scaling. The result supports retaining true vfork for callers
that use it, especially larger parents immediately followed by exec. It does
not establish an improvement for Bash's existing fork path.

### Provenance and exclusions

The included suites contain 200 batches and 5,100 measured child cycles; all
child statuses, semantic probes, suite exit codes, and completion markers passed.
The primary suite uses `up-a1`, `up-b1`, `up-b2`, and `up-a3`; the four-CPU suite
uses `smp-a1`, `smp-b1`, `smp-b2`, and `smp-a2`.

`up-a2` is retained but excluded in full: file timestamps establish a concurrent
build at 10:46:14–10:46:17 local time within that boot. Its replacement, `up-a3`,
passed with 78 half-second process snapshots detecting no competing build or
guest. Other host activity remains uncontrolled. The frozen measurement inputs
were never replaced; preparation recorded source head `889e999f817403fef7a2a40e592493ae6a8f72b5`
and the local changes at that point. A later attempt against the changing shared
build correctly rejected a module/initrd mismatch.

A pilot boot also faulted in libvterm before userspace or any benchmark sample.
Disassembly exposed red-zone locals in a kernel-linked archive. The kernel
archive now uses `-mno-red-zone -mno-sse -mno-mmx`; userspace keeps its own archive.
The rebuilt function reserves stack space, and its archive has no SIMD-register
instructions. This fixes a confirmed kernel ABI violation, but the missing
fault-time registers prevent proving that it caused that particular fault.
All measured images include the fix. Earlier serial-input harness failures and
the failed boot are preserved and contribute no samples.

Local raw logs, CSV rows in JSON, selected-run manifests, image hashes, commands,
host guard records, and analysis are under `/private/tmp/pedigree-vfork-bench/`.
`variants-abi/provenance.json` records the exact A/B inputs and verifies that only
`posix.o` differs across the initrds. `up-summary.json` and `smp-summary.json`
contain the numbers above; `up-exploratory-summary.json` preserves the earlier
summary that included the excluded boot. Final preparation-tool validation used a fresh initrd built after all timed runs.

Validation: `cmake --build build --target uefi-image -j4` passed before freezing
the measurement media. After timing, `cmake --build build --target initrd -j4`
and the final preparation script passed again; `variants-final-verify/provenance.json`
reports `complete`. Python syntax and `git diff --check` also passed. The final
preparation run is a tool check and was not substituted into the measured images.
