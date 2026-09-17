# Trace an x64 syscall

This QEMU TCG plugin records a bounded sequence from a known user instruction
through its matching continuation. It leaves guest interrupts enabled and
includes interrupt handlers in the trace when they execute. It requires QEMU
plugin **API 7**, x86_64 system emulation, and exactly one vCPU.

The output measures **instruction dispatches**, not time or hardware cycles.
Callbacks run before instructions; faults may prevent retirement. A REP string
instruction can represent many memory operations. The instrumentation also
changes QEMU execution speed and interrupt placement. Use an uninstrumented
benchmark for elapsed-time comparisons.

## Build and qualify

Use the header installed with the QEMU executable that will load the plugin.
The host needs a C compiler, GLib development files, pkg-config, and NASM.
On Homebrew the header is in `$(brew --prefix qemu)/include`.

```sh
trace_work=/private/tmp/pedigree-trace
mkdir "$trace_work"
export UV_CACHE_DIR="$trace_work/uv-cache"
sh scripts/benchmarks/qemu-trace/build.sh \
  "$(brew --prefix qemu)/include" "$trace_work/trace.so"
uv run --no-project python scripts/benchmarks/qemu-trace/qualify.py \
  --plugin "$trace_work/trace.so" --output "$trace_work/qualification"
```

The disk-free qualification ROM checks the exact instruction order, loop
iterations, CALL/RET stack movement, an INT/IRET transition, the instruction
limit, and refusal to overwrite existing evidence. Its deliberate debug-exit
device status is 33. The script reports PASS only after checking the trace.

QEMU emits both an exception notification and an interrupt notification for
the ROM's software INT. Notification counts are therefore not hardware IRQ
counts. RFLAGS snapshots can omit lazy condition-code bits; IF is retained.

## Prepare a fresh fixture

Start with the disposable compile benchmark image described in
[compile-latency.md](../compile-latency.md). The synthetic workload needs
`/root/compile-bench/synthetic-vm`, the static `vm-syscall-latency` executable,
and `/vm-syscall-latency.conf` containing `getuid 1000000 1`.
Remove any ablation or diagnostic marker files for a normal-path trace.
The harness prints their configuration before running the workload.

Build the payload with `cmake --build build --target kernel initrd --parallel 8`.
Copy the kernel, `kernel.debug`, uncompressed initrd, configuration, and exact
guest benchmark ELF into the run's artifact directory before another build can
replace them. Install that kernel/initrd in the disposable fixture's active
EFI slot. Verify their hashes by reading them back from the image. Rebuild and
install `compile-latency.c` as the fixture's init; it emits module load addresses
before the synthetic workload. Those addresses validate module symbol mapping.

Never modify a backing image used by a running guest. Reuse the prepared fixture
for later captures; the runner creates a fresh writable qcow2 overlay and a
firmware copy each time. Run `e2fsck` on the offline disposable root after image
edits. Keep the original image untouched.

Locate the actual `syscall` instruction in the **installed benchmark ELF** with
the target objdump. Do not assume an address from a differently linked binary.
The retained static getuid benchmark used on 2026-09-17 has SYSCALL at
`0x4010ec` and continuation at `0x4010ee`.

## Capture

```sh
uv run --no-project python scripts/benchmarks/run-compile-latency.py \
  --image "$trace_work/fixture.img" --output "$trace_work/run" \
  --firmware-code /path/to/compatible/firmware-code.fd \
  --cpus 1 --synthetic-vm --timeout 240 \
  --plugin "$trace_work/trace.so,start=0x4010ec,syscall=102,skip=128,count=8,limit=100000,output=$trace_work/getuid.tsv"
```

Use a new runner output directory and trace filename. Plugin specifications use
commas as separators, so the plugin/output paths must not contain commas.

The runner uses QMP over stdin/stdout and serial FIFOs, avoiding Unix/TCP socket
creation. It records the exact command, QEMU version, serial output, and result.
`UV_CACHE_DIR` keeps uv's cache inside the writable artifact directory. No disk
image rebuilding, socket setup, or interactive monitor is needed for a rerun.

Options:

| Option | Meaning |
| --- | --- |
| `start` | Required guest virtual instruction address, decimal or `0x` hex. |
| `syscall` | Require SYSCALL bytes at `start` and this RAX syscall number. |
| `skip` | Matching starts to skip before capture; default 128. |
| `count` | Maximum captures; default 8. |
| `limit` | Maximum dispatches per capture; default 100000. |
| `output` | New TSV output path; existing files are refused. |
| `stop` | Explicit continuation PC. Required without `syscall`; otherwise defaults to the instruction after SYSCALL. |

Syscall mode closes a capture only when execution reaches its continuation
with the original RSP and CR3. It records the return value in RAX. This is a
bounded nonblocking-syscall probe, not a general scheduler-aware thread tracer:
CR3 alone cannot identify switches between threads sharing an address space.
Inspect discontinuities and kernel context-switch paths before interpreting an
interrupted capture. There are no guest register writes or interrupt masking
operations in the plugin.

## Symbolize and inspect

```sh
uv run --no-project python scripts/benchmarks/qemu-trace/summarize.py \
  --trace "$trace_work/getuid.tsv" --kernel "$trace_work/kernel.debug" \
  --kernel-code "$trace_work/kernel" \
  --initrd "$trace_work/initrd.tar.uncomp" --user "$trace_work/vm-syscall-latency" \
  --nm /path/to/x86_64-pedigree-nm --serial "$trace_work/run/serial.log" \
  --output "$trace_work/summary"
```

Read `summary.md` for counts, `symbolized-trace.txt` for the full ordered path,
and `summary.json` for machine-readable results and artifact identities.
`callgrind.out` opens as a flat instruction-cost view in Callgrind viewers;
it does not invent caller relationships across tail jumps and assembly returns.

Module mapping requires at least two fresh serial anchors or an explicit
module-map manifest. A manifest without fresh anchors is labelled unverified.
Observed instruction bytes are compared with the supplied ELF bytes; `--kernel-code`
supplies the frozen executable when `--kernel` is a separate debug ELF. Assembly
symbols without a declared size use explicitly labelled inferred bounds.
Check completeness, requested capture count, byte validation, return values,
and interruption status before interpreting counts.

Raw TSV records:

- `B`: capture ID, start PC, continuation PC, initial RSP, initial CR3.
- `I`: capture ID, sequence, PC, RSP, CR3, RFLAGS, bytes, disassembly.
- `D`: capture ID, last instruction sequence, notification type, source PC, destination PC.
- `E`: capture ID, complete/limit/exit status, dispatch count, arrival PC, RAX.

Addresses and registers are hexadecimal; IDs, sequences, and counts are decimal.
The first I row is SYSCALL. The final I row is normally SYSRET; the continuation
is observed in E before that user instruction executes. The footer records
requested and observed captures. Missing or truncated output is not a completed
trace.

## First normal-path capture

The initial capture used kernel source `f576f5135be2`, the existing Debug/-Os
configuration with time accounting enabled, one vCPU, and no active ablations.
Eight warmed calls each executed 1,456 dispatches, returned UID 0, and had no
discontinuity or CR3 change. Every captured byte matched the frozen ELF files.
Each call included 86 CALLs, six LOCK prefixes, three RDMSRs, two WRMSRs, and
four REP-prefixed instructions. These counts establish the path to investigate;
they do not assign elapsed time to individual operations.
