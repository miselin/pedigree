# Profile the which compile

`profile.c` counts every dispatched kernel instruction across the first cold
compile, without writing one record per execution. It records one count per
kernel PC/byte sequence, a low-address instruction total, syscall-number counts,
and interrupt/exception notification counts. Output size depends on the code
visited, not how many times loops execute.

The scope is the whole VM between two guest markers. It includes the GCC driver,
its compiler/assembler/linker children, the waiting parent, faults, interrupts,
and any background kernel work. Userspace addresses overlap across executables,
so they are counted together without attributing their PCs to a particular ELF.
The high/low address split is not a measurement of CPU privilege level.

This is an instruction profile, not elapsed-time attribution. Every instruction
has equal weight in the count, while actual instruction costs differ. REP
instructions may do many memory operations. Instrumentation changes execution
speed and interrupt placement; run a separate uninstrumented control if timing
is relevant. The synchronous output dump also delays the stop marker's return.

## Build and qualify

As with the detailed tracer, use the installed QEMU API 7 header, x86_64 TCG,
and one vCPU. The plugin uses inline per-instruction additions, not translation
block instruction-count estimates, which can overcount when execution faults
before a block finishes.

```sh
trace_work=/private/tmp/pedigree-which-profile
mkdir "$trace_work"
export UV_CACHE_DIR="$trace_work/uv-cache"
sh scripts/benchmarks/qemu-trace/build.sh \
  "$(brew --prefix qemu)/include" "$trace_work/profile.so" profile
uv run --no-project python scripts/benchmarks/qemu-trace/qualify-profile.py \
  --plugin "$trace_work/profile.so" --output "$trace_work/qualification"
```

The qualification ROM checks exact per-instruction counts through a loop,
CALL/RET and INT/IRET, marker boundaries, and refusal to overwrite evidence.
For an additional real-kernel check, profile the existing getuid fixture using
`start=0x4010ec,stop=0x4010ee,syscall=102,skip=128`. Its per-PC histogram should
match a clean detailed trace from the same payload, not merely its total count.

## Freeze the fixture and find the markers

Reuse the disposable image workflow in [README.md](README.md), including its
frozen kernel, separate debug ELF, raw initrd, and module-load serial anchors.
Clone a validated image before changing its root. In that offline root, remove
the synthetic-vm marker, keep quick-run/link-cxx/no-sync, remove all ablation and
diagnostic markers, and install the rebuilt `compile-latency.c` harness as init.
Check the root with e2fsck before putting it back at its original offset.

The harness calls `profile_compile_begin()` immediately before forking the cold
compile, and `profile_compile_end()` in the parent after successfully waiting
for the GCC driver. GCC normally waits for its children; this is not a generic
orphan-process tracking facility. The two functions have distinct NOP bodies.
They execute harmlessly in the uninstrumented control too.

Get their addresses from the exact installed harness:

```sh
x86_64-pedigree-nm "$trace_work/compile-latency" | rg 'profile_compile_(begin|end)'
```

Inspect those instructions with objdump. The 2026-09-17 harness had begin
`0x401a40` and end `0x401a50`; rebuilding can move them. The stop gate requires
the starting CR3 and RSP, preventing another executable at the same virtual PC
from ending the capture. The start marker executes once before fork, so it is
not repeated by the child.

## Run and summarize

```sh
uv run --no-project python scripts/benchmarks/run-compile-latency.py \
  --image "$trace_work/fixture.img" --output "$trace_work/profile-run" \
  --firmware-code /path/to/compatible/firmware-code.fd \
  --cpus 1 --quick --skip-sync --timeout 600 \
  --plugin "$trace_work/profile.so,start=0x401a40,stop=0x401a50,output=$trace_work/compile.tsv"

uv run --no-project python scripts/benchmarks/qemu-trace/summarize-profile.py \
  --profile "$trace_work/compile.tsv" --kernel "$trace_work/kernel.debug" \
  --kernel-code "$trace_work/kernel" --initrd "$trace_work/initrd.tar.uncomp" \
  --serial "$trace_work/profile-run/serial.log" --nm /path/to/x86_64-pedigree-nm \
  --output "$trace_work/summary"
```

Run the same command with a new runner output directory and no `--plugin` for
the control. Each run gets a fresh overlay and firmware copy, using FIFO serial
and QMP stdio. Do not rebuild the fixture while either guest is running.
`--skip-sync` omits explicit fsync/sync; it does not disable kernel disk writes.

The quick suite must pass cold compile, warm compile, execution of the resulting
which binary, and the anonymous-memory contract. Only the cold compile is inside
the profile markers. Require its exit status zero, a nonempty output binary,
the complete B/E profile bracket, matching count totals, and validated instruction
bytes before interpreting the profile. Missing/partial output is not success.

Read `summary.md` for function counts and syscall frequencies. The JSON and
symbolized TSV preserve every recorded instruction count; `callgrind.out` is a
flat kernel cost view with the event named `InsnDispatch`. It contains no call
graph or chronology. Interrupt notification counts can include software entry;
they are not necessarily hardware IRQ counts.

The symbolizer uses the matching addr2line beside the selected nm (or explicit
`--addr2line`) to check size-less symbol bounds. Anonymous C++ code following an
assembly symbol must not be charged to that assembly routine. Where a source
location is available but the function name is ambiguous, the report groups it
by source file and marks the function unresolved. Exact source locations remain
in the JSON provenance.

The profiler reserves 524,288 stable instruction counters. Counter reset occurs
at execution of the start marker, covering already translated blocks. Counters
survive retranslation and are shared by equal PC/byte pairs. Start is inclusive;
stop is exclusive. The kernel boundary defaults to `0xffff800000000000`;
`kernel-base` exists only to classify the low-address qualification ROM.

TSV records are `B start_pc cr3 rsp`, `P pc count bytes disassembly`, `U count`,
`S syscall_number count`, `D type from_pc to_pc count`, and
`E complete|exit kernel_count user_count`, separated by tabs. Addresses are hex;
counts and syscall numbers are decimal. Only nonzero P/S/D counts are emitted.
