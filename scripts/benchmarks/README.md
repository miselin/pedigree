# I/O latency benchmark

For native GCC compilation timing, kernel profiles, and comparison heatmaps,
see the [compilation latency guide](compile-latency.md).

## Synthetic VM syscall benchmark

`vm-syscall-latency.c` measures mmap/munmap without rebuilding GCC. Compile it
with the target static toolchain and install it, together with a marker file
named `synthetic-vm`, in the existing compile-latency guest root. Put a single
line such as this in `/vm-syscall-latency.conf`:

```text
gcc-pattern 100000 1
```

Run the disposable image with `run-compile-latency.py --synthetic-vm`. The
available modes are `anonymous`, `anonymous-touch`, `staircase`, `file`,
`fragmented`, and `gcc-pattern`. The last mode approximates the dominant
anonymous mapping sizes from the exact `which.cc` trace. Each run reports the
guest wall, user, and system time, plus the benchmark's own elapsed time.

Compile `io-latency.c` with the Pedigree userspace toolchain and install the binary as
`/io-latency` in a bootable test image. The image must provide root/root console
login, Bash, and `ls`; GNU nano is measured when installed. Use a build with
`PEDIGREE_CRIPPLE_HDD=FALSE` for storage and persistence checks.

Run one guest at a time for comparable timings:

```sh
uv run --no-project python scripts/benchmarks/run-io-latency.py \
  --image /path/to/test.img --output /path/to/results/one-cpu \
  --cpus 1 --firmware-code /path/to/OVMF.fd --keep-scratch
```

Use `--cpus 4` for the SMP check. For split OVMF firmware, also pass
`--firmware-vars /path/to/OVMF_VARS.fd`; both firmware files are copied into the
output directory. `--qemu` and `--qemu-img` select alternative executables.
The supplied image must already boot the intended kernel through UEFI.

The runner types through the emulated keyboard, captures serial output, and checks
for `IOBENCH PASS END`. It retains `disk.qcow2`, `report.json`, `command.json`,
`serial.log`, the QEMU log, and a framebuffer screenshot. Failed runs also capture
CPU registers and PIC state when QMP remains responsive. Each run requires a new
output directory. The backing image is never modified; keep it unchanged while
using or inspecting its retained overlay.

The report includes QEMU block statistics before and after the workload. Compare
read, write, and flush operation deltas alongside timings to distinguish reduced
work from delays shifted between phases.

Boot timing includes both QEMU-start-to-prompt and init-to-prompt intervals. The
runner observes the kernel's `Invoking userspace program at .../init` log line
and records `init_to_username_s`, `init_to_shell_s`, and `blocks_at_init` when it
appears. These observations use 100 ms polling and include QMP response latency. The
shell interval includes the runner's paced login keystrokes; the username interval
does not. A missing init marker sets `init_marker_observed` to false and omits
those intervals. Firmware and GRUB time are excluded from init-to-prompt timing.

The default workload measures three `ls -l /` launches, three `nano --version`
launches when available, first and repeated Bash reads, and a 1 MiB scratch file.
Write, `fsync`, mapped mutation, `msync(MS_SYNC)`, and `munmap` have separate
monotonic microsecond timings. Reads check every scratch byte. `--size-mib 1..8`
changes the file size; `--mode read-only` omits scratch operations. The first Bash
read is not guaranteed cold because the login shell may already have loaded it.
Wall-clock timings from QEMU TCG are comparison data, not physical SSD throughput.

Add `--read-diagnostics` to measure three `ls -1 /` and `ls -n /` launches, a warm
Bash read without byte checksumming, three fork-and-exit children, three minimal
executions of the benchmark itself, and 100 root-directory `stat` calls. Compare
`ls_single` and `ls_numeric` with the existing `ls_long` phases to distinguish
directory enumeration, metadata lookup, and user/group name lookup. Every measured
program has stdout redirected to `/dev/null`; kernel logging is still included.
These controls help separate reading from process creation and executable startup. They run after the
ordinary read/launch phases and work with `--mode read-only`; they cannot be used
with `verify-existing`.

Add `--read-under-sync` to the full workload to measure three Bash reads while a
child performs three full-file `fsync` calls on the scratch file, after its initial
write and sync and before mapped mutation. A pipe handshake coordinates each pair
without sleeps. The report includes individual read and sync timings, child exit
status, combined elapsed time, and the measured overlap between operation intervals.
Compare these reads with `bash_read_warm`. A zero overlap means scheduling did not
actually overlap the operations; the handshake alone is not proof of contention.
No pages are redirtied, so this specifically measures repeated sync of an already clean
file. Default workloads and persistence checks are unchanged without the flag.

The scratch file is `/pedigree-io-bench.bin`, outside the RAM-backed `/tmp`.
Creation uses `O_EXCL`, so an existing file is never overwritten. By default it is
removed after verification. `--keep-scratch` retains it, including after a failure,
for inspection. Guest-only invocations can override its path with
`IOBENCH_SCRATCH`; choose a disk-backed filesystem.

To verify retained data in a fresh guest, use the first run's overlay as the next
run's backing image, retaining the same size:

```sh
uv run --no-project python scripts/benchmarks/run-io-latency.py \
  --image /path/to/results/one-cpu/disk.qcow2 \
  --output /path/to/results/reboot-check --cpus 1 \
  --firmware-code /path/to/OVMF.fd --mode verify-existing --size-mib 1
```

`verify-existing` only reads and validates the scratch file, including the mapped
changes; it does not create, modify, or unlink it. The guest OS can still perform
its own writes during boot. The first guest is stopped with QMP `quit` after the
benchmark, without a guest-wide `sync` or graceful shutdown. A successful same-boot
read does not prove persistence: the fresh-guest check tests whether the requested
sync operations and filesystem metadata actually survived that boundary.

Use `--boot-timeout` and `--benchmark-timeout` to adjust bounded waits. Defaults are
240 seconds for the username prompt and the benchmark; login and QMP exchanges
have separate shorter bounds. The runner returns nonzero on failure and retains
all evidence. It requires a POSIX host and QEMU with QMP keyboard events.

`directory-sync.c` is a separate small-file contract. Compile and install it as
`/directory-sync`, then run `/directory-sync 1`. It creates `/pedigree-dir-sync`
exclusively, syncs its parent, writes and syncs a small file, renames it, syncs the
directory, and repeats directory `fsync` three times without another mutation.
Each sync and rename reports its return status and elapsed time. Verification
checks the renamed file's exact contents and size and absence of the old name.
The directory and files remain after success or failure. `IOBENCH_DIRECTORY` can
select another absolute path on a disk-backed filesystem; its parent must exist.

Run `/directory-sync 1 verify-existing` in a fresh guest, or verify the retained
overlay offline, to establish persistence. The verification mode only reads;
same-boot verification does not prove durability. The optional number selects
1–8 files. The existing runner accepts this binary with `--guest-binary
/directory-sync`; use its default `--size-mib 1` argument for one file and
`--mode verify-existing` for the fresh-guest check. Omit its workload flags and
`--keep-scratch`, since this contract always retains its files.

## Isolated launch and read benchmark

`launch-latency.c` measures one command without running the normal startup
services. Keep the existing init-to-login measurement as the end-to-end check;
use this benchmark to separate cold reads, warm execution and process overhead.
It does not change the kernel or disable writes.

Build a static guest driver so the harness itself does not preload the target's
shared libraries (substitute the configured compiler and musl sysroot):

```sh
x86_64-pedigree-gcc --sysroot=/path/to/build/musl -static -O2 \
  -std=c11 -Wall -Wextra -Werror scripts/benchmarks/launch-latency.c \
  -o /path/to/launch-latency
```

In a **disposable image**, replace `/usr/bin/init` with this executable, mode
0755. Install `/launch-bench.args` with one argument per line:

```text
--serial
--iterations
3
launch
git version
/usr/bin/git
--version
```

The kernel starts the driver directly. No login credentials or startup scripts
are used. The config is limited to 4096 bytes and 31 arguments. The same arguments
can be passed explicitly when running the driver from a shell. Running it without
`--serial` omits host gates and writes results to stdout.

The supplied image must already boot its intended kernel through UEFI and expose
COM1 as `/dev/ttyS0`. Add `--disable-log-to-serial` to its kernel command line so
kernel messages cannot interleave character by character with benchmark records.
For the direct UEFI loader this is the `cmdline` file beside the selected kernel
on the ESP; preserve its root selector. Keep that setting identical across arms.
The flag must be a separate space-delimited token: do not append a newline to it.
Kernel log generation and its debugger history remain enabled.
Prepare the ext2 root offline with `debugfs`, then check it
with `e2fsck -fn` and reinsert it at the original partition offset. Preserve the
original image; do not replace init on an installed system. Freeze each prepared
image while any retained qcow2 overlay refers to it.

```sh
uv run --no-project python scripts/benchmarks/run-launch-latency.py \
  --image /path/to/launch.img --output /path/to/results/git-cold \
  --firmware-code /path/to/OVMF.fd --cpus 4
```

The runner uses a local Unix socket for serial gates and disables the guest NIC.
Use a short output path for the socket. Each output directory must be new. Split
OVMF firmware also needs `--firmware-vars`. Host `--iterations` and `--mode` must
match the image config; mismatched or missing phases fail rather than silently
producing a partial result. `--timeout` bounds the complete run, default 240 s.
A guest used directly without the runner needs its own timeout for hung children.

Each launch creates a pipe before timing, then measures from immediately before
`fork()` until the parent receives the first stdout bytes, and separately until
`waitpid()` completes. The child uses cwd `/`, a fixed C-locale environment,
`/dev/null` for stdin/stderr, and a pipe for stdout. Expected output prefix and
successful child exit are both required. Output storage is bounded; more than
1 MiB fails. Three fork-with-output and three exec-self probes follow the target
launches as warm process controls. They include descriptor setup, pipe delivery,
and teardown; they are not pure syscall timings.

Iteration zero is target-cold only when the target and its libraries have not
already been read. The following iterations measure the same command with those
pages cached. Repeat the entire guest for independent cold samples. Record the
image, kernel, target/library hashes, compiler, CPU count, and host load when
comparing revisions. A fresh guest does not imply a cold host page cache.

For a separate explicit-prewarm arm, insert these two config lines before
`launch` and pass `--prewarm` to the host runner:

```text
--prewarm
/launch-prewarm.list
```

Install that list as newline-separated guest paths to the target, its ELF
interpreter, and recursive `DT_NEEDED` libraries. Resolve these offline, not by
executing the target in the cold guest. Deduplicate aliases of the same file.
The `prewarm` phase times full-file reads and reports their byte cost separately.
It can include non-loadable ELF sections: prewarm plus launch must be compared
with cold launch before claiming a net improvement. This is an experiment in
cache residency, not an implementation or prediction of read-ahead.

For read order comparisons, install a fixture on the root filesystem (not
RAM-backed `/tmp`) with byte `i` equal to `(i*37 + (i>>8)*17 + 0x53) & 255`.
Choose a power-of-two page count, for example 8 MiB. Replace the three launch
config lines with:

```text
read
/launch-input.bin
sequential
```

Use host `--mode read-sequential`; use `permuted` and `--mode read-permuted` in a
separate fresh guest for the other arm. Both perform the same number of 4 KiB
`pread()` calls into the correct destination offsets by default. Add
`--read-size` and a power-of-two byte count from 4096 through 131072 before `read`
in the guest config for larger requests; the fixture must be divisible by that
size. The permuted order is
`(page*1531+17) % page_count`. File open/stat, buffer allocation/prefaulting, and
byte verification are outside the read timer. The size limit is 32 MiB. Every byte
is checked after timing; the metric checksum is the byte sum plus byte count.
The repeating fixture pattern is a benchmark guard, not a comprehensive storage
correctness test. Warm repeats test cached reads and copies. Order-sensitive
read-ahead should improve sequential access without amplifying permuted reads.

For fault locality, use `mmap`, `/launch-input.bin`, and `sequential` as the three
guest config lines, with host `--mode mmap-sequential`. Use `permuted` and host
`--mode mmap-permuted` in a separate fresh guest. These modes map the same fixture
once with `PROT_READ` and `MAP_PRIVATE`, then time one volatile byte access per 4 KiB
page in the selected order. Open, stat, and mapping setup occur before the first
gate; the mapping is not touched before the cold timer. Full-byte validation and
checksum calculation follow each timer. All three iterations retain the same
mapping, so later iterations measure resident access. `--read-size` affects only
the `read` modes. The mmap metric's `bytes` is the **covered file span**, not the
number of bytes copied or sampled; the report labels it `mapped_file_span`.
Its checksum remains the sum of every fixture byte plus the file size.
Resident touch loops can finish below the guest clock's reported resolution;
a zero duration is not a throughput measurement. Visiting every page eventually
also cannot quantify cache pollution from sparse accesses to a much larger file.

`report.json` retains guest timings, per-phase QMP block-stat deltas, and NCQ
submission/completion counts, maximum outstanding tags, and read-size histograms.
The QEMU trace brackets use traced `query-blockstats` requests, so buffered trace
file writes cannot move events between phases. Gates and QMP calls are outside
the guest timers. These brackets also cover validation/reporting outside the
read timer and can include kernel background I/O. `clean_boundaries` means no
NCQ command crossed either boundary; it does not establish process ownership.
Trace accounting includes every AHCI port; this runner attaches one root disk.
Sizes assume its 512-byte logical sectors. Keep `serial.log`, `ahci.trace`,
`command.json`, the overlay, and failure logs alongside the report.
Add `--no-trace` for a timing control with NCQ trace logging disabled; QMP deltas
remain available. The parser rejects competing kernel serial logs rather than
attempting to repair interleaved measurements. Malformed or incomplete
measurements fail, and the original serial bytes remain in the log.

Compare command counts and sizes as well as elapsed time. Reducing 4 KiB commands
through batching can help while still using queue depth one. Increasing NCQ depth
requires overlapping requests; a larger userspace buffer alone does not prove
that. Warm latency with zero disk reads points to work outside the storage
transport. QEMU TCG timings and QMP backend time do not establish physical SSD
throughput or T420 latency.

Run the focused trace-parser checks with:

```sh
uv run --no-project python scripts/benchmarks/test_launch_latency.py -v
```

## Filesystem-wide sync

`sync-latency.c` measures `sync()` on an existing 1 MiB file, followed by an
immediate clean sync and a sync after rewriting the file. Compile it with the
Pedigree userspace toolchain (`-D_DEFAULT_SOURCE -std=c11 -O2 -static`). Install it
as `/usr/bin/init` in a disposable image and create `/sync-bench.bin` offline as
exactly 1 MiB of byte `0x11`. Keep disk writes enabled. Use the launch runner's
quiet kernel command line (`--disable-log-to-serial`) and serial setup.

```sh
uv run --no-project python scripts/benchmarks/run-launch-latency.py \
  --image /path/to/sync-test.img --output /path/to/sync-results \
  --firmware-code /path/to/OVMF.fd --cpus 1 --mode sync --iterations 1
```

The phases are `sync-dirty-0`, `sync-clean-0`, and `sync-redirty-0`. Their `bytes`
field is the file span, not the bytes actually written: use `block_delta` and
`trace.write_commands` for payload traffic, `flush_operations` for durability
barriers, and `trace.maximum_ncq` for observed outstanding requests. Setup reads
and writes occur outside the measured phases. Background writeback remains active.

Use `--cpus 4` for SMP validation. To qualify queue saturation independently of
fast host completion, add `--write-iops 1000`; this limits QEMU backing writes and
must not be mixed into an unthrottled speed comparison. Native admission tests
also exercise full slots, partial admission, and failure draining.

For persistence, boot again using the completed run's `disk.qcow2` as the new
image. Require `SYNCBENCH persistence=PASS bytes=1048576` in the second serial
log: it validates every byte before rewriting the file. `persistence=initial`
is expected only for the original fixture and is not persistence evidence.
The runner quits QEMU after completion without a guest-wide shutdown sync.
