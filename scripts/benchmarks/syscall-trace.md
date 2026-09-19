# Opt-in syscall tracing

Build with `-DPEDIGREE_BENCHMARK_SYSCALL_TRACE=TRUE` and
`-DPEDIGREE_LOG_TO_SERIAL=TRUE`. Both are diagnostic settings for this experiment;
restore the previous build settings afterward. Install the freshly built kernel
and initrd together in a disposable benchmark image. Replace its
`--disable-log-to-serial` boot token with `--enable-log-to-serial`.

In a trace-enabled kernel, `syscall(SYS_syslog, 20, NULL, 1)` enables tracing for
the calling process; zero disables it. Forked children inherit the setting and
exec preserves it. Ordinary processes are unaffected. The control returns
`ENOSYS` when tracing was compiled out.

## Trace the RAM-backed link

Start with a prepared RAM-root fixture from [compile-matrix.md](compile-matrix.md).
Build and install the current `compile-matrix.c` driver, preserving the existing
RAM bootstrap as `/usr/bin/init`. In that disposable fixture, create
`/root/compile-bench/matrix-trace` containing `link` and append that absolute path
to `/root/compile-bench/ramroot-paths`. The marker must reach the copied RAM root.

The driver runs `warm-link`, then `trace-link`, enabling tracing in the second
GCC child immediately before exec. The command is
`gcc which.o -lstdc++ -o matrix-link`. Descendants such as collect2 and ld inherit
tracing; the driver and its executable verification do not. Input hashes and
the linked executable are checked as in the normal matrix.

```sh
uv run --no-project python scripts/benchmarks/run-compile-matrix.py \
  --os pedigree --mode trace-link --storage ramfs \
  --image /absolute/path/to/frozen-trace-fixture.qcow2 \
  --firmware-code /absolute/path/to/OVMF_CODE.fd \
  --output /absolute/path/to/new-trace-run

uv run --no-project python scripts/benchmarks/summarize-syscall-trace.py \
  --input /absolute/path/to/new-trace-run/serial.log \
  --output /absolute/path/to/new-trace-analysis
```

The runner captures serial continuously, enforces phase order and RAM disk-I/O
checks, and labels the run instrumented. Never compare its elapsed time with an
uninstrumented timing baseline. Keep failed captures and their raw logs.

## Record semantics

Records start with `STRACE` and share a unique call ID. All numeric fields are
hexadecimal. `E` records identify process, thread, ABI and raw syscall number;
two `A` records contain the six raw arguments. Selected pathname arguments have
hex-encoded `P` chunks and an `S` validity/truncation status. `X` contains the raw
dispatch result, saved errno and elapsed nanoseconds. Short records fit the
kernel's 128-byte log entry limit; the in-memory log ring cannot retain a whole
compiler trace.

Path snapshots use the guarded user-copy interface and are limited to 256 bytes.
They may fault in memory before dispatch. Buffers, argv vectors, structures and
file contents are not dumped. Unused argument registers are still recorded.

Elapsed time starts after entry logging and ends before completion logging.
It includes blocking and preemption, including trace output from other tasks.
Serial logging, pathname capture and timer reads perturb execution; sums across
processes can overlap and are not CPU-time attribution. This is a behavioral
trace with diagnostic dispatch latencies, not a performance profile.

Completion means the POSIX dispatcher returned. The x64 return tail, accounting,
pending work and restart handling run afterward. Exec and exit use deferred
actions, so their completion does not mean userspace received a normal return.
A fork completion describes the parent; the constructed child return of zero
does not pass through this hook. Failed Linux calls expose `-errno` to userspace
even when the raw dispatch result differs.
