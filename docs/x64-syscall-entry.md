# x64 syscall stack entry

Kernel code runs with `IA32_GS_BASE` pointing to this CPU's `KernelGsAnchor`,
owned by `X86CommonProcessorInformation`. The inactive `IA32_KERNEL_GS_BASE`
holds the user base; `SWAPGS` reverses these roles in userspace. The anchor
contains the kernel stack top, temporary user RSP, processor-information pointer,
and permanent CPU index at offsets 0, 8, 16, and 24. C++ assertions match the
assembly layout.

`setKernelStack` updates both TSS RSP0 and the anchor's stack field. Activating
the anchor is separate from updating the stack. Scheduler and event-stack
transitions use this operation; the scratch state is per CPU.

The syscall stub swaps to the anchor, saves user RSP, loads the kernel stack,
and pushes sixteen saved registers. Kernel GS remains active throughout C++.
Every syscall captures user selectors and FS/GS bases before ordinary dispatch,
then enables interrupts after entry accounting. The final return boundary masks
interrupts, restores metadata, and swaps back to user GS before `SYSRET`.
The C++ callback currently always requests metadata restoration; no query-only
route bypasses it. The 32-byte metadata prefix includes the original-RAX slot.
Stack lookup itself performs no MSR read.

IRQs remain masked while scratch user RSP is live. Exception entry checks the
active GS base when kernel CS alone cannot distinguish an entry/exit window;
dedicated IST handling protects the relevant exception stacks. Metadata restore
has its own brief pair of swaps because loading a GS selector changes the active
base. Those swaps do not expose ordinary C++ to user GS. A clean ordinary syscall
therefore executes four swaps overall, two metadata MSR reads, and two writes.

See [syscall framework performance](syscall-framework-performance.md) for current
timings, validation, and the history of the removed query-only experiments.

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
