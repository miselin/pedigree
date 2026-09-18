# x64 kernel GS

All x64 C++ kernel code runs with a kernel GS base. The active base points to a
resident per-CPU anchor; `IA32_KERNEL_GS_BASE` holds the current thread's user GS
base. `SWAPGS` exchanges those bases at the user/kernel boundary. FS retains its
existing role as the user TLS base.

The anchor has a fixed assembly layout:

| Offset | Field |
| --- | --- |
| 0 | Current kernel stack top, also published in TSS.rsp0 |
| 8 | Syscall entry's saved user RSP |
| 16 | `ProcessorInformation*` |
| 24 | Dense logical CPU index |

`Processor::information()` remains the common API. Its x64 header definition is
an always-inline GS-relative load; the current-thread getter is a second load.
The compiler memory clobber prevents reuse across operations that could change
the current CPU or thread. Callers still need their normal exclusion when
retaining a per-CPU reference across several operations. Hosted and other
backends retain their own lookup implementations.

## Bootstrap and scheduling

The BSP installs a constant-initialized anchor before calling C++, including
global constructors. It references the existing safe BSP information object.
After constructors, processor initialization installs that object's permanent
anchor. No initialization conditional is needed in the x64 lookup.

The BSP owns logical slot zero. Each AP's information object and index are
published before its SIPI; trampoline slot `0x7FE0` carries the anchor address.
The AP installs GS before entering C++. A bootstrap IDT has no IST selectors;
the permanent IDT is loaded only after the local GDT and TSS are active.

Changing the kernel stack updates the anchor and TSS, without overwriting the
inactive user GS bank. Changing the current thread saves the actual outgoing
user GS base and installs the incoming base. Reading the actual base matters
because userspace can load a GS selector without calling `arch_prctl`.
Fork/clone inherit captured GS metadata; exec clears user GS.

`ARCH_SET_GS` and `ARCH_GET_GS` operate on the inactive user bank. Setting GS
also updates the saved syscall return metadata. User GS bases must be low
canonical addresses. BSP and AP startup explicitly disable CR4.FSGSBASE, so a
user instruction cannot bypass that restriction.

## Entry and return

Syscall entry swaps to kernel GS, fences, and loads the kernel stack. It keeps
kernel GS throughout C++ and swaps back immediately before SYSRET. A quiet
syscall needs no RDMSR for its stack or processor lookup.

Ordinary IRQ entry checks the saved CS. Kernel-mode exceptions additionally
inspect the sign of the active GS base: an NMI, debug trap, or fault can arrive
in a swap window with kernel CS and user GS. The low-user/high-kernel address
invariant makes that test unambiguous. Entry records its swap decision in a
callee-saved register and reverses exactly that swap on return. This follows
the [x86 entry constraints documented by Linux](https://www.kernel.org/doc/html/latest/arch/x86/entry_64.html).

NMI, double fault, debug, machine check, general protection, and stack faults
have separate resident IST stacks. User debug/GP/SS frames move to the thread's
normal kernel stack before return work can block or schedule. NMI/DF/MC do not
run ordinary pending user-return work. These stacks do not implement recovery
from arbitrary nested faults, or the repeated-NMI protocol needed when an
intervening exception's IRET unblocks a second NMI.

Entry metadata captures GS from the inactive bank. Loading a GS selector changes
the active base, so metadata restoration temporarily swaps to the user bank,
loads the selector and exact saved base, then swaps back. IRQs remain masked;
paranoid exception entry covers both intermediate states. Context-switch,
fork, signal, and first-user-entry assembly obey the same ownership rule.

## Validation

See the [syscall contracts](../scripts/benchmarks/README.md#syscall-framework-contracts).
Nonzero GS must survive direct loads after syscalls, blocking, preemption,
signals, and fork; exec must reset it. Check one and four CPUs. A timing run or
a successful boot alone does not validate those paths.

### NMI window injection

`PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS` provides opt-in NMI records for precise
window injection. Each 64-byte record in `pedigree_nmi_entry_diagnostics`
contains count, interrupted RIP/CS/RSP, information pointer, CPU index, active
GS, and frame address. Count is published last. In that diagnostic build only,
NMI handling records and returns without logging, accounting, or scheduling.
Normal NMI policy is unchanged. Assembly exports entry, exit, and selector
restore labels so QEMU's debugger can stop at the exact windows.

Build with both `PEDIGREE_X64_USER_ENTRY_DIAGNOSTICS` and
`PEDIGREE_ACTIVITY_DIAGNOSTICS` enabled. After toggling diagnostics, explicitly
force the metadata assembly to rebuild before freezing the artifacts:

```sh
touch src/system/kernel/core/processor/x64/asm/UserReturnMetadata.s
cmake --build build
```

The current incremental NASM rules can otherwise retain an object compiled with
the previous diagnostic flags, leaving diagnostic counters undefined at link
time. Keep the rebuilt code ELF, separate debug ELF, generated `config.h`, and
installed kernel/initrd together. The debug ELF supplies symbols but does not
contain executable bytes; `--kernel-code` must name the matching code ELF.

Prepare a disposable compile-latency image whose `synthetic-vm` phase launches
the static `kernel-gs-contract` executable **without arguments**. Keep its ELF
for `--guest-elf`; the runner stops at its `kernel_gs_user_loop` symbol after
nonzero per-thread GS canaries are installed. The guest must emit the existing
`COMPILEBENCH READY phase=vm-synthetic` gate and both contract/workload end
markers. The regular compile-latency fixture's extra `1` argument must be removed
for this contract. Freeze the image before running:

```sh
uv run --no-project python scripts/benchmarks/run-kernel-gs-nmi.py \
  --image /path/to/frozen/gs.img \
  --kernel-code /path/to/frozen/kernel \
  --kernel-debug /path/to/frozen/kernel.debug \
  --config /path/to/frozen/config.h \
  --guest-elf /path/to/frozen/kernel-gs-contract \
  --firmware-code /path/to/OVMF.fd \
  --output /path/to/new/nmi-results
```

Use `--firmware-vars` when firmware has a separate variable image, and `--nm`
when the cross-toolchain is installed outside the repository's default location.
The runner creates a writable overlay and firmware copies, uses serial FIFOs,
QMP, and a loopback GDB stub, and needs no host GDB executable. The output
directory must not already exist.

It injects nine NMIs: at five syscall entry/exit boundaries, three selector
restore labels, and immediately after the verified `MOV GS,AX` instruction.
That ninth boundary is derived from the frozen code bytes and requires active
GS to be zero. Each record must identify the exact interrupted RIP/CS/RSP,
valid CPU-zero kernel GS anchor, and an NMI frame on IST2. Each NMI must return
with RIP/RSP/CS/GS unchanged before the next injection, and the guest's final
GS/TLS/errno canaries must pass. `report.json`, `serial.log`, and the GDB/QEMU
logs retain the evidence.

This is a one-CPU diagnostic check, with one NMI at each boundary. It does not
test nested NMIs, intervening faults, machine checks, or arbitrary nested-fault
recovery, and it provides no timing evidence. Run the separate one- and
four-CPU guest contracts for scheduling and per-CPU coverage.
