# x64 syscall stack entry

`IA32_KERNEL_GS_BASE` points to this CPU's `SyscallEntry` record, owned by
`X86CommonProcessorInformation`. Its first two words hold the current kernel
stack top and temporary user RSP. C++ offset assertions match the assembly's
offsets 0 and 8.

`setKernelStack` updates both TSS RSP0 and the entry record, then publishes the
record's address to the MSR. Scheduler and event-stack transitions continue to
use this existing operation. The entry scratch state is per CPU, not global.

The syscall stub swaps to that record, saves user RSP, loads the kernel stack,
and pushes the same sixteen saved registers as before. A second `swapgs`
restores user GS before C++ dispatch. The shared metadata helper captures
selectors and FS/GS bases before ordinary dispatch or pending return work.
Bounded IRQ-masked queries can leave the original selectors and bases installed;
C++ returns whether assembly must restore captured metadata. The 32-byte
metadata prefix, original-RAX slot, C++ frame layout and stack alignment remain
unchanged. Entry no longer reads
`IA32_KERNEL_GS_BASE` with `rdmsr` to recover the stack pointer.

IRQs remain masked while the scratch user RSP is live. Ordinary interrupt entry
does not use that scratch slot, and kernel interrupt return does not schedule.
This is not NMI-entry hardening: the existing lack of a dedicated NMI stack
still leaves a window before RSP switches to the kernel stack. The new sequence
shortens that window. Both swaps remain necessary for the current shared
FS/GS metadata convention.

See [syscall framework performance](syscall-framework-performance.md) for the
bounded callback contract, current timings, and validation of deferred metadata.

## Stack-lookup validation at `65558e329`

The one-CPU getuid trace at that commit contained 1,076 dispatches per uninterrupted call,
versus 1,081 before this change. It retains two `swapgs`, two `rdmsr` for user
FS/GS, and two `wrmsr` to restore them. The removed MSR read was solely the
kernel-stack lookup. This instruction reduction is not a wall-time claim.

`scripts/benchmarks/syscall-entry-contract.c` exercises concurrent fork workers,
syscall register arguments, TLS/errno and signal return without relying on
CPU-affinity migration. Nonzero GS coverage is reported separately because
the target currently lacks `ARCH_SET_GS`/`ARCH_GET_GS` support. Existing compile
and memory contracts provide broader guest coverage.

The contract passed on one and four vCPUs, observing APIC IDs 0–3 in the latter
run. All four workers completed their TLS/errno, six-argument mmap and signal
round trips. Nonzero GS was explicitly skipped. The one-vCPU GCC and memory
suites also passed. Forced cross-CPU migration and NMI injection were not tested.

Build the standalone contract against the packaged musl SDK, then run it in a
disposable guest:

```sh
pedigree-compiler-15.3.0-r2/bin/x86_64-pedigree-gcc \
  --sysroot="$PWD/build/musl" -I"$PWD/build/musl/include" \
  -L"$PWD/build/musl/usr/lib" -static -O2 -std=gnu11 -Wall -Wextra \
  scripts/benchmarks/syscall-entry-contract.c -o /tmp/syscall-entry-contract
```

Success ends with `ENTRY-CONTRACT PASS END workers=4`; each worker reports its
observed CPU IDs and whether nonzero GS was exercised.

The earlier one-CPU global-shadow diagnostic was discarded. Its source and
timing comparison remain under `/private/tmp/pedigree-stack-msr-spike-20260917`.
The retained implementation's frozen payloads, traces, test logs and review
are under `/private/tmp/pedigree-syscall-entry-20260917`.
