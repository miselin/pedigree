# Compiler stage matrix

`compile-matrix.c` compares tiny builds, individual `which.cc` compilation stages,
and full builds with and without `-pipe`. Both kernels execute the same static
driver and GCC/musl filesystem. Run one guest at a time, without concurrent builds.
The default is one CPU for QEMU TCG comparisons. Use `--cpus 4` for SMP
qualification, and keep CPU counts matched when comparing timings. These are
not physical-hardware measurements.

## Prepare a shared fixture

Start with a frozen, bootable Pedigree image containing the complete native GCC
15.3.0 toolchain described in [compile-latency.md](compile-latency.md). Its root
must have `/root/compile-bench/which.cc`, but no `which.ii`, `which.s`, or `which.o`.
The Linux runner requires exactly one MBR partition of type `0x83`; it discovers
that partition rather than assuming its number. Keep the kernel/initrd and every
backing image unchanged throughout the comparison.

Set absolute paths for the local inputs; every output directory below must be new:

```sh
MATRIX_ARTIFACTS=/absolute/path/to/new-matrix-artifacts
MATRIX_BASE=/absolute/path/to/frozen-pedigree.img
MATRIX_LINUX_ROOT=/absolute/path/to/linux-root.raw
MATRIX_LINUX_KERNEL=/absolute/path/to/vmlinuz
MATRIX_LINUX_INITRD=/absolute/path/to/initrd.img
MATRIX_FIRMWARE=/absolute/path/to/OVMF_CODE.fd
mkdir -p "$MATRIX_ARTIFACTS/setup/root/usr/bin"
mkdir -p "$MATRIX_ARTIFACTS/setup/root/root/compile-bench"

compilers/dir/bin/x86_64-pedigree-gcc --sysroot="$PWD/build/musl/usr" \
  -static -O2 -std=c11 -Wall -Wextra -Werror \
  scripts/benchmarks/compile-matrix.c -o "$MATRIX_ARTIFACTS/compile-matrix"
install -m 755 "$MATRIX_ARTIFACTS/compile-matrix" \
  "$MATRIX_ARTIFACTS/setup/root/usr/bin/init"
install -m 755 "$MATRIX_ARTIFACTS/compile-matrix" \
  "$MATRIX_ARTIFACTS/setup/root/root/compile-bench/compile-matrix"
install -m 644 scripts/benchmarks/compile-matrix-tiny.cc \
  "$MATRIX_ARTIFACTS/setup/root/root/compile-bench/tiny.cc"
xorriso -as mkisofs -R -J -o "$MATRIX_ARTIFACTS/setup.iso" \
  "$MATRIX_ARTIFACTS/setup"

uv run --no-project python scripts/benchmarks/run-compile-matrix.py \
  --os linux --mode prepare --image "$MATRIX_BASE" \
  --linux-root "$MATRIX_LINUX_ROOT" --linux-kernel "$MATRIX_LINUX_KERNEL" \
  --linux-initrd "$MATRIX_LINUX_INITRD" --setup-iso "$MATRIX_ARTIFACTS/setup.iso" \
  --output "$MATRIX_ARTIFACTS/prepare"
```

The Linux boot disk must support the runner's direct boot to `/bin/sh`, ext2,
`env`, `chroot`, `mount`, and `cp`. Supply that disk and its matching kernel/initrd
explicitly; the recorded reference used Debian Linux 3.2.78.

Preparation installs the driver, generates the `.ii`, `.s`, and `.o` once, and
records their sizes and FNV identities. Success requires both the matrix PASS
and a successful Linux writeback/unmount marker. Verify `prepare/report.json`
has `result: PASS` and `linux_cleanup_rc: 0` before using its overlay:

```sh
MATRIX_IMAGE="$MATRIX_ARTIFACTS/prepare/pedigree.qcow2"
```

This is the immutable shared backing for **both** subsequent guests. Preserve
its entire backing chain; do not boot it directly or overwrite its parents.
Each run creates a fresh qcow2 child, so neither flattening nor full image copies
are needed. Record SHA-256 identities for the driver, tiny source, original
source, prepared overlay, kernel/initrd, and toolchain files alongside reports.
The initial investigation's `setup-manifest.json` and `frozen-fixture.json` are
examples of that provenance record.

## Run and compare

```sh
uv run --no-project python scripts/benchmarks/run-compile-matrix.py \
  --os linux --image "$MATRIX_IMAGE" --output "$MATRIX_ARTIFACTS/linux" \
  --linux-root "$MATRIX_LINUX_ROOT" --linux-kernel "$MATRIX_LINUX_KERNEL" \
  --linux-initrd "$MATRIX_LINUX_INITRD"

uv run --no-project python scripts/benchmarks/run-compile-matrix.py \
  --os pedigree --image "$MATRIX_IMAGE" --output "$MATRIX_ARTIFACTS/pedigree" \
  --firmware-code "$MATRIX_FIRMWARE"

uv run --no-project python scripts/benchmarks/summarize-compile-matrix.py \
  --linux "$MATRIX_ARTIFACTS/linux/report.json" \
  --pedigree "$MATRIX_ARTIFACTS/pedigree/report.json" \
  --output "$MATRIX_ARTIFACTS/comparison"
```

Each run has CPU controls before and after the matrix, one warmup of every case,
then three measured rounds. Round 2 reverses the case order. The summary excludes
warmup, reports medians and observed ranges, and pairs `-pipe` comparisons within
each round. Three samples do not establish statistical significance.

| Case | Compiler work |
| --- | --- |
| `tiny`, `tiny-pipe` | Header-free, libc-free `_start` compiled and linked; second adds `-pipe` |
| `preprocess` | `gcc -E which.cc` |
| `syntax` | `gcc -fsyntax-only which.ii` |
| `codegen` | `gcc -S which.ii` |
| `assemble` | `gcc -c which.s` |
| `link` | `gcc which.o -lstdc++` |
| `full`, `full-pipe` | `gcc which.cc -lstdc++`; second adds `-pipe` |

All commands run in `/root/compile-bench` with the same normalized environment:
`PATH=/usr/bin:/bin`, `LC_ALL=C`, `HOME=/root`, `TMPDIR=/tmp`, and matching GCC
15.3.0 `CPLUS_INCLUDE_PATH`. The tiny flags remove dependencies of the generated
program; GCC and its own shared libraries still load normally. Each case reuses
its output filename across rounds. Outputs and diagnostic logs reside on ext2;
`/tmp` is memory-backed on both kernels (explicit tmpfs in the Linux chroot).
Generated executables are checked outside their measured compile interval.

## Interpret the results

Guest wall time encloses fork/exec through `wait4`; user/system values are the
GCC process tree's accounting, including reaped descendants. It excludes the
benchmark parent's fork/wait/reap CPU and independent kernel workers, so
`wall - (user + system)` is not a pure I/O-wait measurement. Host wall spans
the serial gate through DONE and includes polling, serial, and reporting
overhead, especially noticeable for tiny rows. Compare both wall measurements.
Stages are not additive: syntax and code generation overlap, and each stage
launch repeats executable loading, runtime setup, and filesystem work.

Pedigree currently supplies user/system CPU time but leaves the other `wait4`
rusage fields zero. Zero faults, block counts, or context switches mean those
counters are unavailable, not that the events did not occur. QMP block and IRQ
snapshots give additional context, but their boundaries can include untimed
verification/setup after DONE. CPU controls can reveal broad guest/host timing
differences; they cannot establish that interrupts caused a compiler slowdown.
QEMU's `info irq` also omits some interrupt sources, including local APIC events.
Linux also has an extra boot disk, though benchmark storage settings match.

Keep plugin traces separate from timing runs. Select one exact phase with
`--profile-phase` and configure the plugin's begin/end markers to the driver's
`profile_compile_begin`/`profile_compile_end` symbols. Linux receives the selection
through `MATRIX_PROFILE`; a Pedigree trace fixture needs a matching
`/root/compile-bench/matrix-profile` file. Make that change in a separate fixture,
preserving the uninstrumented backing. The summarizer rejects profiled runs.

The runner retains commands, options, source hashes, serial output, guest input
identities, and all phase metrics in each output directory. A timeout, failed
phase, identity change, or incomplete protocol remains a failed run; retain its
artifacts rather than replacing it with a successful retry.

## Run the toolchain entirely from RAM

Start with the prepared `MATRIX_IMAGE` above. `compile-matrix-ramroot.c` mounts
a fresh `ramfs`, copies and verifies the paths in
`compile-matrix-ramroot-paths.txt`, enters that root, then executes the unchanged
matrix driver. The compiler, dynamic loader, libraries, headers, inputs, logs,
outputs and temporary files are all memory-backed. The whitelist is specific
to this GCC 15.3.0 C++ workload; it omits unused C/LTO frontends.

```sh
MATRIX_RAM=/absolute/path/to/new-ram-matrix-artifacts
mkdir -p "$MATRIX_RAM/setup/root/usr/bin"
mkdir -p "$MATRIX_RAM/setup/root/root/compile-bench"
compilers/dir/bin/x86_64-pedigree-gcc --sysroot="$PWD/build/musl/usr" \
  -static -O2 -std=c11 -Wall -Wextra -Werror \
  scripts/benchmarks/compile-matrix-ramroot.c -o "$MATRIX_RAM/compile-matrix-ramroot"
install -m 755 "$MATRIX_RAM/compile-matrix-ramroot" "$MATRIX_RAM/setup/root/usr/bin/init"
install -m 755 "$MATRIX_RAM/compile-matrix-ramroot" \
  "$MATRIX_RAM/setup/root/root/compile-bench/compile-matrix-ramroot"
install -m 644 scripts/benchmarks/compile-matrix-ramroot-paths.txt \
  "$MATRIX_RAM/setup/root/root/compile-bench/ramroot-paths"
xorriso -as mkisofs -R -J -o "$MATRIX_RAM/setup.iso" "$MATRIX_RAM/setup"

uv run --no-project python scripts/benchmarks/run-compile-matrix.py \
  --os linux --mode install --image "$MATRIX_IMAGE" \
  --linux-root "$MATRIX_LINUX_ROOT" --linux-kernel "$MATRIX_LINUX_KERNEL" \
  --linux-initrd "$MATRIX_LINUX_INITRD" --setup-iso "$MATRIX_RAM/setup.iso" \
  --output "$MATRIX_RAM/install"
```

Require installation PASS and clean Linux unmount before freezing
`$MATRIX_RAM/install/pedigree.qcow2`. This installs only the bootstrap and path
list; it preserves the existing driver and frozen intermediate files.

```sh
MATRIX_RAM_IMAGE="$MATRIX_RAM/install/pedigree.qcow2"
chmod 444 "$MATRIX_RAM_IMAGE"
uv run --no-project python scripts/benchmarks/run-compile-matrix.py \
  --os pedigree --storage ramfs --image "$MATRIX_RAM_IMAGE" \
  --firmware-code "$MATRIX_FIRMWARE" --boot-timeout 600 \
  --output "$MATRIX_RAM/pedigree"
uv run --no-project python scripts/benchmarks/run-compile-matrix.py \
  --os linux --storage ramfs --image "$MATRIX_RAM_IMAGE" \
  --linux-root "$MATRIX_LINUX_ROOT" --linux-kernel "$MATRIX_LINUX_KERNEL" \
  --linux-initrd "$MATRIX_LINUX_INITRD" --boot-timeout 600 \
  --output "$MATRIX_RAM/linux"
uv run --no-project python scripts/benchmarks/summarize-compile-matrix.py \
  --linux "$MATRIX_RAM/linux/report.json" --pedigree "$MATRIX_RAM/pedigree/report.json" \
  --output "$MATRIX_RAM/comparison"
```

The bootstrap retains only standard descriptors and a real `/dev` directory
descriptor at fd 3. A procfs descriptor link provides device access inside the
root on both kernels; Pedigree does not support bind mounts. No source file or
source-root directory descriptor remains. Copied regular files are read back
and hashed, then audited again after chroot; symlink text, modes and target
device identity are checked. A shared inventory hash verifies cross-OS equality.
Pedigree's current `statfs` reports ext2's type even for RamFs; that field alone
cannot establish filesystem identity.

Copying, auditing, sync and settling occur before timing. The runner waits for
two quiet one-second disk snapshots, then rejects read/write/flush requests
on any QEMU block device throughout the matrix, including between phases.
The summarizer independently validates those snapshots. Boot/setup still use
disk, and disk drivers and caches still exist in the kernel; zero requests
does not mean all background cache bookkeeping has ceased.

The compact RAM root has fewer directory entries than the original full
installation. Comparing their times tests removal of the disk-backed workload,
but does not isolate directory-population effects from the filesystem backend.
Use a matching compact disk-root control before assigning a precise gain to
RamFs or a particular storage component.

## Observed baseline, 2026-09-18

The unchanged kernel image from the disk-view pass was measured with QEMU
11.1.1, one SandyBridge TCG CPU and 4 GiB RAM. The Linux boot kernel was
3.2.78; both guests used the same GCC 15.3.0/musl root, driver, normalized
environment and frozen intermediate files. Both completed all 38 phases.
All five input identities matched across guests and remained unchanged.

Host wall medians, in seconds, excluding warmup:

| Case | Linux | Pedigree | Pedigree / Linux |
| --- | ---: | ---: | ---: |
| Tiny | 0.099 | 0.441 | 4.47x |
| Tiny with `-pipe` | 0.102 | 0.430 | 4.22x |
| Preprocess | 0.867 | 2.705 | 3.12x |
| Syntax only | 4.713 | 6.558 | 1.39x |
| Compile `.ii` to assembly | 12.737 | 21.201 | 1.66x |
| Assemble | 0.727 | 1.409 | 1.94x |
| Link | 0.329 | 3.148 | 9.56x |
| Full | 13.713 | 24.634 | 1.80x |
| Full with `-pipe` | 14.493 | 24.850 | 1.71x |

Paired full-build `-pipe` savings were -0.94% to +0.95% on Pedigree, with
mixed direction. Linux was slower with `-pipe` in all three rounds, by
2.41% to 9.38%. Tiny guest-wall differences also changed direction, so they
do not establish a RamFs benefit. On Linux, voluntary context switches grew
from 17 to 2,630–2,633 with `-pipe`; this is a plausible cost on one CPU,
not a measured causal breakdown.

Linking is the most disproportionate small workload: median reported system
time was 2.619 seconds on Pedigree versus 0.128 on Linux. Compilation has the
largest standalone absolute gap, with system time 5.654 versus 1.176 seconds.
The arithmetic controls took 0.162–0.176 guest seconds across both kernels;
this short hot loop does not cover compiler memory access or episodic work.
All 27 measured brackets on each kernel recorded zero benchmark-disk read
bytes in QMP; writes remained active. These results do not identify interrupt
cost or isolate the filesystem from VM/process work.

Next, profile the link-only phase and compare its output on ext2 versus
`/tmp`. Separately compare compilation to assembly with output discarded to
`/dev/null`. Keep the frozen inputs and ordinary kernel behavior for those
experiments. Independent stage medians must not be subtracted as a pure
backend, filesystem, or I/O-time decomposition.

Full ranges, raw triples, guest CPU times, commands and logs are retained at
`/private/tmp/pedigree-compile-matrix-20260918`; the comparison is
`comparison/summary.md`. These warmed measurements use a different harness
from earlier whole-build runs and are not evidence of a new kernel speedup.

## RAM-root comparison, 2026-09-18

Both RAM runs passed all 38 phases, with identical inventories of 2,328 regular
files, 150 explicitly copied directories and 11 symlinks. File contents totaled
117,900,568 bytes; the inventory hash was `a877f9b2461f6e89`. All QEMU devices
showed zero read, write, flush and discard requests from initial settling
through the final phase, including intervals between phases. The kernel image
and matrix driver were unchanged from the disk-root comparison.

Host-wall medians in seconds (three warmed repetitions):

| Case | Linux disk | Linux RAM | Pedigree disk | Pedigree RAM | RAM ratio |
| --- | ---: | ---: | ---: | ---: | ---: |
| Preprocess | 0.867 | 0.796 | 2.705 | 1.948 | 2.45x |
| Syntax only | 4.713 | 4.721 | 6.558 | 6.924 | 1.47x |
| Compile `.ii` to assembly | 12.737 | 12.610 | 21.201 | 20.192 | 1.60x |
| Assemble | 0.727 | 0.711 | 1.409 | 1.264 | 1.78x |
| Link | 0.329 | 0.340 | 3.148 | 2.821 | 8.29x |
| Full | 13.713 | 13.838 | 24.634 | 25.616 | 1.85x |
| Full with `-pipe` | 14.493 | 14.196 | 24.850 | 25.640 | 1.81x |

Pedigree preprocessing improved 28% and linking 10% in this comparison. The
preprocessing ranges were 2.537–2.761 seconds on disk and 1.923–2.044 in RAM;
linking was 3.103–3.223 versus 2.779–2.908. Full-build ranges overlap
(24.619–25.921 versus 24.723–25.725), so no full-build improvement is established.
These are observations across separate runs, not backend-only causal estimates:
the compact tree also changes directory population and the copy changes cache
and memory state. No matching compact disk-root control was run in this pass.

Reported RAM-root system times remain disproportionately high for preprocessing
(Pedigree 1.081 seconds, Linux 0.148) and linking (2.293 versus 0.124). This
establishes substantial cost without disk requests, while leaving VFS/RamFs,
file mapping, process lifecycle, interrupts and background bookkeeping as
candidates. A link-only trace in this RAM fixture is the next compact probe;
the matrix does not identify which candidate dominates.

RAM artifacts: `/private/tmp/pedigree-ram-matrix-20260918`, including
`comparison/summary.md` (all cases and raw triples), `storage-comparison.json`
(old/new results and whole-matrix block deltas), and `frozen-fixture.json`.
Boot/setup disks remain attached and kernel disk/cache services remain present;
this experiment removes disk-backed benchmark files and requests, not every
possible background storage-related CPU cost.
