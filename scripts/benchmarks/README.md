# I/O latency benchmark

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

Add `--read-diagnostics` to measure a warm Bash read without byte checksumming,
three fork-and-exit children, three minimal executions of the benchmark itself,
and 100 root-directory `stat` calls. These controls help separate reading from
process creation, executable startup, and metadata lookup. They run after the
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
