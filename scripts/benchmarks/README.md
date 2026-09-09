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

The default workload measures three `ls -l /` launches, three `nano --version`
launches when available, first and repeated Bash reads, and a 1 MiB scratch file.
Write, `fsync`, mapped mutation, `msync(MS_SYNC)`, and `munmap` have separate
monotonic microsecond timings. Reads check every scratch byte. `--size-mib 1..8`
changes the file size; `--mode read-only` omits scratch operations. The first Bash
read is not guaranteed cold because the login shell may already have loaded it.
Wall-clock timings from QEMU TCG are comparison data, not physical SSD throughput.

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
