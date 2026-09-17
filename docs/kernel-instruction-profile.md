# Kernel instruction profile: syscall and GCC overhead

Lock tracking is disabled by default, including in Debug builds. Enable the
existing diagnostics when investigating locking with:

```sh
cmake -S . -B build -DPEDIGREE_TRACK_LOCKS=TRUE
```

Existing build directories retain their cached setting. To disable tracking in
one of those builds, configure it explicitly with `-DPEDIGREE_TRACK_LOCKS=FALSE`.
This leaves normal spinlock synchronization enabled.

The profiling procedure is documented in
[the QEMU compile profiler](../scripts/benchmarks/qemu-trace/PROFILE.md).

## Changes

The initial complete GCC profile counted 11.620 billion kernel instruction
dispatches. Explicit lock diagnostics accounted for 1.468 billion (12.63%);
CPU-information helpers and their checked vector lookups accounted for another
1.384 billion (11.91%). `resolveUserFault` tested 31.556 million mapping objects
for 114,374 faults, rejecting 99.64% of candidates. `faultInUnlocked` performed
another 27.669 million probes.

Three changes address that work:

- Leave lock diagnostics available behind `PEDIGREE_TRACK_LOCKS`, default off.
- Use bounded direct access to the published CPU topology and isolate APIC
  scanning in a cold fallback. Early bootstrap and TSS validation remain intact;
  GS/TLS layout is unchanged. The compiled single-CPU `information()` path takes
  nine instructions with no calls or stack frame, versus 23 of its own
  instructions plus a checked vector call previously.
- Index each address space's mapping list by start address in a balanced tree.
  Fault lookup finds the greatest start at or below the address, then checks
  that one candidate's extent. The append-ordered list remains available for
  iteration. Publication, splitting, removal, replacement and fork maintain the
  index together with the list; split/clone reservations are allocated before
  altering their source. Original-access permission checks remain unchanged.

The index relies on the manager's existing disjoint mapping ranges and stable
start addresses while published. Range operations that need to visit multiple
objects still iterate the list. The optional reverse-lookup benchmark ablation
still explicitly selects a scan.

## Results

The before payload came from `f576f5135`; the patch started at `10d0f0422`.
Both used Debug/-Os, QEMU 11.1.1 x64 TCG, one vCPU, the same GCC 15.3.0 files,
and `gcc -o which which.cc -lstdc++`. Generated configurations differ only in
`PEDIGREE_TRACK_LOCKS`. Runtime ablations remain zero and disk writes stay
enabled. Each run used a fresh writable overlay against a frozen UEFI fixture.

| Kernel instruction dispatches | Before | After | Reduction |
| --- | ---: | ---: | ---: |
| Whole compile | 11,619,574,729 | 7,330,759,211 | 36.9% |
| Explicit lock diagnostics | 1,467,761,168 | 0 | 100% |
| CPU information/id and checked vector lookup | 1,383,575,657 | 414,426,788 | 70.0% |
| Fault lookup functions, matching, list advance and new indexed lookup | 1,518,653,783 | 56,305,145 | 96.3% |
| Spinlock acquire/exit/release, excluding diagnostic helpers | 1,280,851,547 | 983,322,478 | 23.2% |

These are disjoint self-count groups, not inclusive call costs or elapsed-time
shares. Index maintenance costs are included in the whole-kernel total.
Userspace counted 8,942,851,074 instructions before and 8,942,851,075 after;
both captures contain 124,011 syscall instructions. The new profile validates
all 90,820 distinct recorded kernel PC/byte pairs, with no mismatches or gaps.

The workload still enters `resolveUserFault` exactly 114,374 times and
`faultInUnlocked` exactly 102,783 times. Calls to `MemoryMappedObject::matches`
fell from 59,229,079 to 217,272: the faults did not disappear; finding their
mapping became cheaper.

The saved uninstrumented cold/warm compile was 43.303 / 35.753 seconds; the new
uninstrumented run was 32.643 / 27.740 seconds (24.6% / 22.4% lower). This is one
pair per payload, not a statistical latency estimate. The CPU-only control was
0.192 / 0.193 seconds respectively. Instrumented compile timings are excluded
from that comparison. Both compilations produced a 1,980,136-byte executable
and successfully ran `which gcc`.

Eight warmed getuid captures each fell from 1,456 to 1,143 dispatches, including
the user SYSCALL instruction. CPU helper work fell from 452 to 139 dispatches,
accounting for the entire reduction. Every capture returned UID 0 at the
original continuation/RSP/CR3, with no discontinuity notifications. The new
trace validates all 904 distinct PC/byte pairs. Interrupts remained enabled
during the handler. Repeated uninstrumented million-call measurements overlap;
this pass does not establish a getuid wall-time improvement.

## Verification and follow-up

- The x64 kernel and all initrd consumers rebuilt with tracking off. Fresh
  payloads were read back from the fixture and matched their frozen hashes.
- All 30 native Tree/MappingList tests pass, including boundaries, append order,
  reservation rollback, independent replacement/clone indexes, shuffled removal
  and a 4,096-entry comparison-cost bound.
- One- and four-vCPU GCC runs and anonymous-memory/remap/residency/discard suites
  pass. The new remap case covers 64 out-of-order mappings, guarded gaps, kernel
  copies into untouched pages, protection splits, fork CoW, fixed relocation and
  address reuse.
- The additional CPU affinity diagnostic passes with one vCPU but fails with
  four. The identical diagnostic fails on both frozen old and new kernels:
  `sched_getaffinity` reports CPU 0 while `getcpu` and hardware CPUID report CPU
  3. The four-vCPU runner therefore retains an overall FAIL result despite its
  successful compile and memory phases. Forced migration to every CPU remains
  unverified; the failure is not hidden by a retry or a weakened assertion.
- The Linux x86-64 hosted deferred-write replay harness is unavailable on this
  macOS host. Its original-access permission checks were preserved; actual
  guest CoW and protection tests passed on one and four vCPUs.

The affinity failure predates this patch (`48e5c8ca1`): worker completion in
`Thread-affinity.cc` can publish an allowed mask and clear return work without
rebinding the owner; the return fast path then skips the owner/mask check. That
separate repair should also inspect the no-work load before interrupts are
disabled. No affinity code changes are included here.

The new GCC profile's largest remaining source group is the allocator bitmap
scan (about 7.9%); ordinary spinlock acquire/exit/release sum to 13.4%.
CPU-information lookup is still called about 44 million times, but its cost per
lookup is much lower. Cache hashing and lookup, deferred scopes, and wait
bookkeeping are visible next targets. They remain unchanged in this pass.

Full reports, exact commands, frozen binaries, overlays, raw traces and failed
diagnostic runs are retained under
`/private/tmp/pedigree-syscall-trio-20260917`. The prior GCC profile is under
`/private/tmp/pedigree-which-profile-20260917`; the prior syscall trace is under
`/private/tmp/pedigree-getuid-trace-20260917`. The new artifact manifest records
file hashes and links the controls to the instruction reports.
