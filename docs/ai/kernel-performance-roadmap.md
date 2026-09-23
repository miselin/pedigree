# Kernel performance roadmap

This checklist orders the performance work most likely to improve Pedigree.
QEMU is the development and completion environment. A measured QEMU improvement
with the relevant correctness checks is sufficient to finish a work item and
wrap up the checklist. ThinkPad T420 testing is an optional manual follow-up.

The companion [storage checklist](t420-storage-latency-checklist.md) expands the
ext2, AHCI, FAT, and ISO9660 work. This document owns the overall ordering.

Post-checklist work covers sustained sequential reads and safe syscall-handler
unloading. The original cold 8 MiB result (66.06 to 79.68 MiB/s) is a short
sample; the follow-up below uses 128 MiB and reports actual MiB/s.

**2026-09-22: all ten stages complete in QEMU.** The retained work includes the
RCU core, descriptor lookup, VFS file ownership, Thread-owned accounting, and
cheaper synchronization. Focused native and guest checks include compiler/link
qualification. Conditional items record their assessment, not implementations
of every proposed alternative. The initial syscall-handler unloading trials
were deferred because of their admission cost; the follow-up below revisits
the lifetime protocol and measures that tradeoff explicitly.
The retained gains, tradeoffs, and checks are recorded below.

### Measured outcome

These results come from the matched comparisons recorded below. They measure
different workloads and must not be added together as a whole-kernel speedup.
The refreshed compiler scorecard precedes RCU and safe syscall unloading;
later quick compiler checks establish no additional consistent build gain.

| Workload or work count | Before | Retained result | Change |
| --- | ---: | ---: | ---: |
| RAM-root full build | 18.437 s | 17.041 s | 7.6% less time |
| RAM-root link | 1.230 s | 1.010 s | 17.9% less time |
| Kernel instructions during compile | 3.097 billion | 2.815 billion | 9.1% fewer |
| File-cache hash work during compile | 240.2 million instructions | 60.5 million | 74.8% fewer |
| Small overwrite plus fsync, initial pair | 127.922 ms / 6 flushes | 59.640 ms / 2 flushes | 53.4% less time; 66.7% fewer flushes |
| Rename plus directory fsync, initial pair | 147.559 ms / 7 flushes | 40.819 ms / 2 flushes | 72.3% less time; 71.4% fewer flushes |
| Shutdown flushes | 11 | 2 | 81.8% fewer |
| Descriptor lookup, 1 CPU / 1 worker | 600,810 calls/s | 798,244 calls/s | 32.9% higher throughput |
| Descriptor lookup, 4 CPUs / 4 workers | 1,132,708 calls/s | 1,801,023 calls/s | 59.0% higher throughput |
| VFS path lookup, 4 CPUs / 4 workers | 17,591 operations/s | 22,617 operations/s | 28.6% higher throughput |
| Cold sequential read, 8 MiB | 66.06 MiB/s / 2,048 commands | 79.68 MiB/s / 512 commands | 20.6% higher throughput; 75% fewer commands |
| Sustained cold read, larger window | 94.34 / 95.55 MiB/s | 101.08 / 104.42 MiB/s | 7.1% / 9.3% higher throughput |

Clean file/directory sync performs no writes or flushes. Descriptor mutations
cost 8.6–9.2% more time on one CPU. Accounting and synchronization changes
improve cheap-call throughput by 11.5–16.3% in their isolated comparison, before
safe syscall unloading adds 3.4–14.1% time in its subsequent matched comparison.
The latter is a retained lifetime guarantee, with its cost explicitly accepted.
No material cold-compile gain or consistent VM elapsed-time gain is established.

## Working rules

- [x] Measure the exact workload before changing it.
- [x] Compare the same source configuration, payload, QEMU command, CPU count,
  firmware, and disk backing chain.
- [x] Use disposable writable overlays for persistence work.
- [x] Measure elapsed time and the work that should change, such as syscall,
  fault, allocation, lock, or ATA command counts.
- [x] Prefer existing benchmarks, QEMU traces, and focused tests. Add a new
  harness only when the existing mechanisms cannot answer the question.
- [x] Use one-vCPU QEMU for stable latency comparisons. Add four-vCPU QEMU when
  the change affects shared state, scheduling, interrupts, or reclamation.
- [x] Match validation to risk: focused tests first, subsystem tests when useful,
  and fresh guest or persistence checks only when the change requires them.
- [x] Use short, individual guest runs during iteration; reserve broader suites
  for changes whose scope or failures justify them.
- [x] Keep a change only when it improves a useful workload, removes measured
  work, or materially simplifies a recurring correctness-sensitive protocol.
- [x] Stop a line of work when its cost is no longer material.
- [x] Do not leave an agent or task waiting for physical T420 access. Record the
  optional hardware follow-up and complete the QEMU-qualified work.

## Core workloads

Keep the permanent scorecard small:

- [x] Native compile: the frozen `which.cc` workload, with its RAM-root control.
- [x] Link and launch: the existing frozen link payload and cold/warm executable
  launch workload.
- [x] Durable ext2: small overwrite/create plus `fsync`, rename plus directory
  `fsync`, global `sync`, and clean shutdown.
- [x] SMP smoke: the existing parallel syscall, scheduler, timer, IRQ, and
  lifetime checks selected for the subsystem being changed.

Use a focused VM, descriptor, allocator, read, FAT, or ISO9660 benchmark only
while working on that subsystem. Do not run every workload after every edit.

## 1. Freeze a QEMU control

- [x] Record the current commit and dirty files.
- [x] Freeze one normal one-vCPU fixture containing the compiler and storage
  payloads.
- [x] Prepare a four-vCPU fixture only for concurrency-sensitive work.
- [x] Confirm `PEDIGREE_CRIPPLE_HDD=FALSE` only in fixtures that test persistence.
- [x] Run the core workloads enough times to distinguish a useful change from
  normal host variance.
- [x] Keep diagnostic builds separate from normal timing builds.

**Done when:** the core workloads are repeatable and a candidate can be compared
without rebuilding an unrelated benchmark environment.

## 2. Fix the ext2 durability path

Complete the first section of the
[storage checklist](t420-storage-latency-checklist.md):

- [x] Establish the small-file and shutdown command-count baseline in QEMU.
- [x] Separate payload submission from device durability barriers.
- [x] Batch ext2 data and metadata by required ordering phase.
- [x] Preserve batching across lower-cache aliases when the baseline shows the
  fallback is relevant.
- [x] Avoid redundant clean-sync and shutdown barriers.
- [x] Verify failure retry and fresh-guest persistence with the existing storage
  tests and runners.

**Done when:** ext2 barriers follow documented ordering phases rather than page
or helper count, and the accumulated change improves the QEMU storage workload.

## 3. Re-profile the integrated kernel

The storage work changes blocking and background activity, so refresh the CPU
profile before starting a broad kernel project.

- [x] Re-run native compile, link, and launch on the integrated kernel.
- [x] Use the existing syscall and instruction profiling tools to find the
  largest remaining kernel costs.
- [x] Measure lock contention separately from lock call frequency when locks are
  a candidate.
- [x] Take straightforward local wins first when they remove visible lookup,
  allocation, wakeup, or cache work without introducing a new protocol.
- [x] Record recurring lifetime or reclamation mechanisms as possible RCU
  consumers instead of extending each one independently.

**Decision:** start RCU only if the current tree still shows several real
consumers and the shared primitive is simpler than their combined bespoke
machinery.

## 4. Build the smallest useful RCU subsystem

**Core implemented and tested.** Per-CPU, nonpreemptible read sections protect
membership until the caller obtains an existing durable lease. Reader exit is
the quiescent point; no scheduler polling or background reclamation worker is
needed. The descriptor pilot below determines whether to broaden adoption.

- [x] Define whether readers may be preempted, migrate, nest, or run in interrupt
  context. Do not leave these as caller conventions.
- [x] Provide an RAII read guard with no allocation or contended global lock on
  the normal path.
- [x] Define the scheduler and idle points that report quiescence.
- [x] Provide pointer publication, deferred reclamation, and a synchronous grace
  period for teardown.
- [x] Bound retired-object backlog when a reader delays a grace period.
- [x] Ensure module callback code remains resident through the grace period.
- [x] Test the core at its natural boundary: nested readers, publication,
  retirement, a delayed reader, and SMP reclamation.
- [x] Benchmark the read path and grace-period behaviour against the mechanism
  selected for the first migration.

The writer-owned retirement queue holds at most 32 callbacks, drains on
destruction, and applies synchronous backpressure when full. Owners must stop
enqueueing and drain before releasing callback code or unloading a module;
there is no global worker that can outlive that owner. The descriptor pilot
uses synchronous grace periods and keeps descriptor teardown outside its
writer lock. It adds no unloadable callbacks.

Do not add test-only production modes, hooks, or exported internals for RCU.
Use normal interfaces and external test scaffolding.

**Done when:** the core is smaller than the protocols it is intended to replace,
its read path is cheaper, and reclamation remains bounded under four-vCPU stress.

## 5. Prove RCU with one consumer

**Pilot retained: ordinary descriptor lookup.** Read sections only traverse
membership and retain the existing descriptor lease; no I/O or callback runs
under RCU. The measured reader gain justifies proceeding to workload-level
qualification, with the one-vCPU mutation cost recorded below.

- [x] Choose one read-mostly registry with bounded mutation and teardown. Timer,
  page-fault, IRQ, and syscall-handler publication are candidates.
- [x] Keep writer serialization; replace reader membership and reclamation only.
- [x] Preserve synchronous unregister by unpublishing, waiting for a grace
  period, then releasing object and module lifetime.
- [x] Exercise the important consumer-specific race, including self-removal or
  interrupt context only when that consumer supports it.
- [x] Compare reader cost, writer cost, code size, and lifecycle complexity with
  the old implementation.

**Decision:** broaden RCU only if the pilot is measurably faster or clearly
simpler without weakening teardown.

## 6. Migrate high-value RCU consumers

**Complete: descriptor membership and VFS file ownership migrated.** Both pass
short compiler/link qualification; neither establishes a whole-build speedup.
VM and other registries have been assessed below. Thread accounting and
owner-local synchronization changes also pass focused qualification.

RCU protects membership during lookup. Convert to an existing durable lease or
reference before a pointer escapes the read section or an operation blocks.

### Descriptor table

- [x] Make ordinary descriptor lookup read-mostly.
- [x] Retain the descriptor or open-file description before leaving RCU.
- [x] Keep install, close, `dup2`, table growth, fork, and exec serialized.
- [x] Measure compile, link, vector I/O, and parallel descriptor workloads.

### VFS and path lookup

- [x] Apply RCU to stable-key child, path, mount, cache, or file-owner membership
  where the current profile justifies it.
- [x] Retain `ChildLease`, parent, or cache ownership before blocking or escape.
- [x] Keep rename, unlink, mount, unmount, and eviction under writer control.
- [x] Measure launch, header-heavy compilation, and directory traversal.

The shared tracked-file registry is converted; child and mount membership
keep their existing locks. Path traversal, compiler, and launch checks pass.
The short launch comparison does not establish an additional latency gain.

### Virtual memory

- [x] Consider RCU publication for mapping lookup only if fault or mapping
  retention remains material after the existing tree index improvements.
- [x] Preserve writer-side range and reservation rules.
- [x] Retain a mapping before blocking or populating pages.
- [x] Verify concurrent fault, `munmap`, protection, fork/CoW, and exit behaviour.

Mapping lookup remains under the existing `OperationGuard`, which retains the
mapping across page population and blocking work. A last-hit cache avoids
repeating the tree search; every membership mutation invalidates it. The profile
does not justify replacing that lifetime contract with RCU and new mapping
leases. Concurrent fault/protection/unmap, fork/CoW, and exit checks pass.

### Other registries

- [x] Migrate process/thread or event registries only when measured use or a
  recurring lifecycle protocol justifies it.
- [x] Stop once the remaining consumers are cold or already simple.

Process/thread lookup and timer/event registries do not justify another
membership migration in the current profile. IRQ and page-fault dispatch
already have lock-free reader admission, with callback lifetimes that can
block, remove themselves, or abandon stacks. Preserve those contracts.
The syscall-handler retirement experiment and its rejection are recorded below.

## 7. Revisit scheduler, wait, interrupt, and allocation costs

- [x] Re-profile ready-queue selection, wakeups, wait completion, timer dispatch,
  IRQ delivery, allocator search, slab paths, and cache lookup.
- [x] Remove unnecessary work before changing synchronization.
- [x] Narrow lock scope only when contention or cache-line traffic is visible.
- [x] Do not replace interrupt-aware locks with a simpler lock that loses the
  required interrupt-state contract.
- [x] Improve fragmented bitmap search only if it remains material.
- [x] Batch per-CPU allocation or RCU reclamation only if shared traffic is
  measured and memory remains bounded.
- [x] Use focused host tests where possible and one/four-vCPU guest checks for
  actual scheduler, timer, IRQ, or reclamation behaviour.

## 8. Revisit syscall bodies

- [x] Re-rank `readv`, `writev`, `mmap`, `munmap`, and `lseek` after descriptor,
  VFS, VM, and allocator work.
- [x] Remove repeated validation or lookup only while preserving partial
  progress, `EFAULT`, nonseekable, signal, and restart behaviour.
- [x] Prefer syscall-body improvements over another special entry path.
- [x] Consider vDSO `getpid()` only when the current profile gives it useful
  volume and fork/exec identity publication is straightforward.
- [x] Run the existing ABI and signal contracts relevant to each change.

## 9. Improve the read and transport path

**Done:** bounded adjacent read coalescing reduces command count and sequential
latency. The focused follow-up and limits of its evidence are recorded below.

- [x] Use the storage checklist to determine whether command setup, bounce
  copies, completion waits, or read size now dominate.
- [x] Coalesce adjacent AHCI transfers before attempting zero-copy DMA.
- [x] Add bounded sequential read-ahead only if sequential command count remains
  important and random/small reads stay stable.
- [x] Address ISO9660 batching or optical geometry only for a measured workload.
- [x] Evaluate MSI or completion-path redesign only after interrupt overhead is
  isolated from the generic scheduler and wait paths.

## 10. Finish secondary filesystems and policy experiments

- [x] Apply the proven submission/barrier model to FAT when FAT performance is a
  current goal.
- [x] Keep FAT crash ordering and terminal unlinked-file retirement explicit.
- [x] Compare size- and speed-oriented kernel builds only after source changes
  have been measured independently.
- [x] Treat local binding, LTO, PGO, or similar linker/compiler changes as
  separate ABI and footprint experiments.

## Checklist completion

- [x] Re-run the core workload affected by the retained work.
- [x] Show an elapsed-time improvement, a meaningful reduction in measured work,
  or a simpler shared protocol with no material performance regression.
- [x] Pass the focused correctness checks selected for the changed subsystem.
- [x] Record any useful physical-hardware question as follow-up rather than a
  blocker.

These gates apply to each retained change. Conditional investigation items are
complete when the measured decision is recorded; rejected implementations and
post-checklist follow-ups are explicitly identified. A physical result is not
required to complete a stage.

## Post-checklist: optional T420 confirmation

When the user has time and suitable media, the QEMU-qualified source can be
checked on the T420 with the existing `g++` baseline and any relevant storage,
launch, or read workload. Record enough controller, drive, interrupt, and CPU
information to interpret differences from QEMU, and verify persistent data
after reboot when storage changed.

This is a separate user-assisted qualification task. It must not keep the
performance checklist, an agent run, or the underlying implementation task
open.

## Result note

### Stage 1: QEMU control (2026-09-22)

- Starting commit: `640f25eef`. The only initial dirty files were this roadmap
  and `docs/ai/t420-storage-latency-checklist.md`, both untracked.
- Previous compiler fixture images have missing backing files. Reconstructed
  a fresh ext2 fixture from the local GCC 15.3.0, binutils 2.46.1, musl 1.2.6,
  GMP, MPFR, MPC, and zlib packages. Historical timings are not the baseline.
- Fresh current-source kernel/initrd build passed. The 179 focused ext2,
  cache-sync, SCSI-command, and partition host tests passed.
- Fixture hashes, build configuration, preparation commands, and run logs:
  `/private/tmp/pedigree-performance-roadmap-20260922`.
- Two complete disk-backed runs passed: cold compile 22.215–22.633 s, warm
  compile 18.583–20.714 s, isolated link 1.394–1.412 s. The RAM-root matrix
  passed all 38 phases, verified 1,237 files (104,512,679 bytes), and observed
  no disk requests during measurements. Cold/warm launch and fork/exec passed.
- Fixed the benchmark serial admission gates to use finite polling, matching
  the existing compile runner. macOS symlink permissions were normalized only
  in the reconstructed RAM fixture so its existing permission audit passes.
- Shutdown initially failed because POSIX detached `/dev` before `/dev/shm`.
  Reordered `terminalQuiesce` to detach the child first. The corrected control
  powers off, passes offline `e2fsck`, and has a clean ext2 marker. A fresh
  four-vCPU boot verified both persisted files and rename, repeated all eight
  durability phases, and powered off. This is the storage SMP control; later
  scheduler/IRQ/RCU work still needs its own selected concurrency contracts.
- Corrected one-vCPU baseline: overwrite/create `fsync` 127.922/118.680 ms
  (6/7 flushes); clean file sync 108.501 ms (5 writes, 5 flushes); rename plus
  directory sync 147.559 ms (7 flushes); clean directory sync 90.040 ms
  (6 writes, 6 flushes); clean global sync 9.299 ms (0 writes, 2 flushes).
  Shutdown: 2 writes and 11 flushes. Trace-enabled timings vary; command
  counts establish the redundant work. Normal accounting/interrupts remain on.
- Storage control: `shutdown-fixed-control/fixture.qcow2` under the artifact
  directory above, based on `640f25eef` plus only the detach-order fix.
  Reports: `/private/tmp/perf-0922-{compile-b1,compile-b2,ram-b3,launch-b2,shut-b2,shut-b4}`.
  Stage 1 is complete. No commit has been made.

For each retained slice, record only:

- starting commit and relevant dirty files;
- workload and baseline;
- changed symbols;
- focused validation;
- before/after time and work counters;
- artifact path, if logs are worth retaining;
- commit and remaining uncertainty.

### Stage 2: ext2 durability (2026-09-22)

- Reused the existing `writeFromBatch`/`syncData` submission and durability
  contracts through Disk, Partition, SCSI, and AHCI; no new API was needed.
- `Ext2Filesystem::syncBlock` uses dirty-aware cache sync. `syncInode` batches
  allocation metadata before inode publication and stops after a failed phase.
  Xattr/quota and indirect-mapping dependencies retain their existing order.
  The small-file trace does not justify additional lower-cache alias machinery.
- `AhciPort::command` and `transferBatch` track pending writes under the existing
  submission lock. The lock excludes later writes through barrier completion,
  so a whole-port flag replaces the proposed generation protocol. Failed
  barriers do not clear pending state.
- One-vCPU control versus integrated candidate, in microseconds and ATA flushes:

  | Operation | Before | After | Flushes before / after |
  | --- | ---: | ---: | ---: |
  | Overwrite fsync | 127922 | 59640 | 6 / 2 |
  | Create fsync | 118680 | 81926 | 7 / 3 |
  | Clean file fsync | 108501 | below clock resolution | 5 / 0 |
  | Rename + directory fsync | 147559 | 40819 | 7 / 2 |
  | Clean directory fsync | 90040 | below clock resolution | 6 / 0 |
  | Dirty global sync | 78017 | 50290 | 4 / 2 |
  | Clean global sync | 9299 | 9540 | 2 / 0 |
  | Orderly shutdown | host-timed | host-timed | 11 / 2 |

- Timings are single trace-enabled samples; command-count reductions are the
  principal evidence. Clean global-sync elapsed time is unchanged despite
  eliminating its hardware commands. Background writeback can move a metadata
  write between adjacent phases.
- 180 ext2/cache/SCSI/partition tests pass, including allocation-barrier failure
  and retry. AHCI admission/completion tests pass with ASan/UBSan; 15 benchmark
  runner tests pass. A fresh four-vCPU boot verifies all 8192 persisted bytes
  and rename, repeats the workload, and powers off. Offline `e2fsck` passes and
  the filesystem is marked clean.
- Reports: `/private/tmp/perf-0922-{phase-a1,barrier-a1,barrier-a4}`. Frozen
  candidate: `barrier-candidate/fixture.qcow2` under the Stage 1 artifact path.
  T420 latency and drive-cache behavior remain optional hardware follow-up.

### Stage 3: CPU profile and local lookup work (2026-09-22)

- The storage-integrated compiler, link, and anonymous-memory workload passes.
  Warm compile is 18.984-18.989 s, link 1.407 s, and compiler-output sync
  0.496 s versus the original 0.891-0.950 s. Cold compile remains variable.
- The qualified QEMU instruction plugin captured the same cold compile before
  and after the local CPU changes. Both profiles have complete brackets and
  verified kernel/module bytes: 84,450 and 84,241 matched PC/byte pairs, zero
  mismatches or unmapped instructions.
- `HashTable::findNextSet` now stops at an unused bucket. Both removal APIs
  rehash, so deletion cannot leave a hole in a live probe chain. Two focused
  tests bound miss work and cover collision/removal/reinsertion with linear
  and quadratic probing. `File::setCachedPage` uses one update lookup and
  removes invalid entries instead of inserting negative cache entries.
- `PageFaultHandler::unpublishDispatch` checks whether removal is active before
  scanning readers, following the existing timer-registry pattern. A later
  remover observes the already-cleared hazard; an active remover still gets
  the existing scan, generation check, and waiter notification.
- Kernel instruction dispatches fall from 3,097,374,202 to 2,814,838,506
  (**9.1%**). File-cache hash-table work falls from 240,217,830 to 60,541,894
  (**74.8%**); ordinary page-fault retirement scans fall from 88,756,552 to
  zero. Userspace counts differ by only 1,888 of 8.943 billion dispatches.
  Instruction counts are not elapsed-time attribution.
- All 1,241 native tests pass (one pre-existing disabled test). Artifact roots:
  `profile-{b1,a1}-summary`, `lookup-compile`, and `cache-compile` under the
  Stage 1 directory; runner reports `/private/tmp/perf-0922-{lookup-a1,profile-b1,profile-a1}`.

### Stages 4-8: initial decision gates

- Timer, IRQ, page-fault, and syscall handlers are real lifetime consumers.
  Their callbacks can block, remove themselves, or abandon kernel stacks.
  Membership-only RCU would still need their durable callback leases and
  cleanup rules. No smaller shared replacement was established, and the
  measured fault-reader scan was removed locally. RCU core/pilot/migrations
  were initially deferred. The subsequent RCU experiment below starts with
  short descriptor membership lookups instead of those callback protocols.
- Spinlock acquisition/release is frequent, but `acquireContended` accounts for
  only 340 dispatches in each one-vCPU profile. This is not an SMP contention
  measurement. No lock contract or scope was changed.
- After the local changes, timer registry work is 9.7 million dispatches
  (0.34%), IRQ registry work 41.6 million (1.48%), and `SlamBitmap` 8.1 million
  (0.29%). No allocator-search, per-CPU reclamation, scheduler, or interrupt
  redesign is justified by this workload.
- Syscall volume is led by lseek (30,234), mmap (27,344), munmap (26,871),
  readv (20,446), and writev (13,641). The retained cache/lookup changes
  reduce work below these interfaces. No new entry path or getpid vDSO was
  added; getpid has no volume in the measured compile. ABI, partial-progress,
  signal, and restart semantics remain unchanged.

### Stage 9: AHCI read trials (2026-09-22)

- The follow-up retains 16 KiB read commands formed from whole pages with
  adjacent disk and destination addresses. Grouping happens before the existing
  transfer loop; its slot admission, reaping, and failure draining are reused.
  Each input receives its group's completion status. Single-page reads bypass
  grouping, and writes remain separate.
- Matched one-vCPU `-O3` runs use the same kernel, root image, configuration,
  payload, and runner; only the AHCI module changes. The cold 8 MiB sequential
  read takes 121.109 ms before and 100.404 ms after (17.1% less), with 2,048
  4 KiB commands reduced to 512 16 KiB commands (75% fewer). Host-wall time
  changes from 161.466 to 128.551 ms. Both retain a maximum of four outstanding
  commands. Warm repeats issue no I/O and all bytes verify.
- The matched cold random read takes 407.870 ms before and 392.601 ms after,
  with 2,048 4 KiB commands in both cases. Host-wall time is 436.709/426.388 ms;
  backend read time is 31.819/32.265 ms. This is within normal variance, not a
  claimed random-read speedup. A fresh four-vCPU sequential read also verifies
  all bytes, 512 commands, drained boundaries, and warm cache-only repeats.
- These are individual short runs, not tail-latency qualification. The earlier
  implementation had two unexplained cold random-read stalls of 5.101/5.165 s,
  amid alternating runs of 0.35-0.50 s. Elevated backend time and a host-cache
  experiment did not establish their cause. They were not reproduced in this
  follow-up, and this patch does not claim to fix them. Larger 64 KiB read
  commands and write coalescing remain rejected after their latency regressions.
- The focused ASan/UBSan tests cover non-NCQ and NCQ grouping, saturation,
  partial completion and failure draining, noncontiguous destinations, a short
  final range, LBA48 bounds, and unchanged write/barrier behavior. Validation
  uses five short guest runs total: sequential and random control/candidate
  pairs, then one SMP smoke. Each guest completes in 7-8 seconds. No compiler
  matrix or full native suite was repeated for this change.
- Reports: `/private/tmp/perf-0922-next-{read-control,read,random-control,random,read4}`.
  Frozen candidates are `next-read` and `next-random` under the Stage 1 artifact
  directory. Earlier trial artifacts remain under
  `/private/tmp/perf-0922-{read*,random*,uncached-*,coal-*}`. No read-ahead, direct
  DMA, MSI, completion redesign, or optical changes were selected.
- The 1 MiB dirty/rewrite workload after restoring separate writes takes
  91.081/80.496 ms, with 257 writes and five flushes per phase. Immediate clean
  sync issues no writes or flushes. Shutdown and a fresh four-vCPU boot pass,
  including verification of all 1,048,576 persisted bytes. These write paths
  are unchanged by removal of the read experiment.

### Stage 10: compiler policy (2026-09-22)

- Compared the accumulated source at `-Os` and `-O3`, changing only
  `PEDIGREE_OPTIMIZE_SIZE`. One-vCPU QEMU, firmware, backing-chain topology,
  compiler binaries, commands, and RAM-root contents match. Interrupts,
  sampled accounting, assertions, and persistence settings remain unchanged.
- All 38 RAM-root phases pass for both builds, with identical inputs and zero
  measured disk requests. Host-wall medians from three rounds after warmup:

  | Workload | Source changes, `-Os` | CPU policy, `-O3` |
  | --- | ---: | ---: |
  | Full build | 19.332 s | 18.088 s |
  | Full build with `-pipe` | 19.622 s | 18.288 s |
  | Preprocess | 1.354 s | 1.207 s |
  | Link | 1.320 s | 1.046 s |

- The isolated compiler-policy reductions are **6.4%** for full builds and
  **20.7%** for linking. Both policy images included the subsequently removed
  read trial, which the measured RAM phases do not use. These are bounded TCG
  trials, not hardware results or attribution of every elapsed-time difference
  to one source change. The retained-source scorecard below uses a refreshed
  original control to limit host drift.
- New x64 configurations default to `-O3`; other architectures retain `-Os`.
  Existing caches keep their explicit values. The active build now selects
  `PEDIGREE_OPTIMIZE_SIZE=FALSE`. Kernel text grows from 857,053 to 2,031,565
  bytes, and the uncompressed initrd from 6,123,520 to 7,649,280 bytes.
  See [build profiles](kernel-build-profiles.md) for reproduction and the
  footprint tradeoff. The existing CMake flag-propagation test passes for the
  new default, explicit `-Os`/`-O3`/`-O0`, and unaffected host-tool flags.
- FAT, ISO9660, local binding, LTO, and PGO were not selected. The measured
  development workload uses ext2; compiler policy is kept separate from the
  source-level instruction and command-count improvements above.
- Reports: `/private/tmp/perf-0922-{final-os,final-os-ram,o3,o3-ram}`; frozen
  binaries, configuration, and manifests are under `final-os-*` and `o3-*`
  in the Stage 1 artifact directory.

### Accumulated correctness checks

- The retained kernel/initrd build passes. All 1,241 native tests pass, with
  one pre-existing disabled test; the 17 AHCI sanitizer and benchmark-runner
  tests pass, as does the focused CMake optimization/default test. Ruff finds
  the same 22 pre-existing findings in the three checked Python files, with
  none introduced by this patch. `git diff --check` passes.
- The accumulated four-vCPU image passes the Linux/native syscall ABI contract,
  vector-I/O contract, and four million syscalls across four concurrent
  workers. The vector contract includes partial faults, EOF, shared offsets,
  concurrent descriptor replacement, cache/mapping lifetime, eventfd,
  timerfd, signalfd, and pipe record atomicity.
- The old ABI benchmark assumed Linux `gettid()` returned the process ID.
  That assertion failed identically on the original control and final source;
  the current kernel uses a separate global task identity. The benchmark now
  checks that the global ID is stable and differs after fork, while the native
  local ID remains one. The corrected contract passes on both kernels. This
  changes the benchmark expectation, not kernel identity behavior.
- Contract reports: `/private/tmp/perf-0922-abi-contract-original` and
  `/private/tmp/perf-0922-final-contract-{abi,vector,parallel}`. Temporary
  copies of the existing launch harness retain child output for diagnostics;
  their timings are not performance evidence.
- After removing read coalescing, a fresh 38-phase RAM matrix, 16-phase
  disk-backed compiler/memory run, and four-vCPU vector-I/O contract pass.
  The four-vCPU full compiler/memory run also passed before removing that
  experiment; kernel CPU code is unchanged. The resulting compiler binary
  remains 1,980,136 bytes with FNV-1a `41f5414073fc9aac`.
- The final image passes all eight small-file durability phases and shutdown.
  A fresh four-vCPU boot verifies 8,192 persisted bytes and the rename,
  repeats the workload, and powers off. Offline `e2fsck -fn` passes and
  reports a clean filesystem. The earlier four-vCPU fresh-boot check also
  verified the unchanged 1 MiB write path.
- Final artifacts: `retained-*` under the Stage 1 artifact directory and
  `/private/tmp/perf-0922-retained-*`; offline filesystem check:
  `final-fsck.log`. T420 throughput and drive-cache behavior remain optional
  follow-up. No commit has been made.

### Retained-source scorecard

The original `-Os` control was rerun immediately after the retained-source
qualification to limit host drift. Both one-vCPU RAM matrices pass all 38
phases, verify identical input identities, and issue zero measured disk
requests. These are host-wall medians of three rounds after warmup:

| Workload | Refreshed original control | Retained source, `-O3` | Reduction |
| --- | ---: | ---: | ---: |
| Tiny build | 0.255 s | 0.218 s | 14.6% |
| Preprocess | 1.342 s | 1.166 s | 13.2% |
| Code generation | 15.109 s | 14.170 s | 6.2% |
| Link | 1.230 s | 1.010 s | 17.9% |
| Full build | 18.437 s | 17.041 s | 7.6% |
| Full build with `-pipe` | 18.992 s | 17.569 s | 7.5% |

The short CPU controls are 173-176 ms before and 174-180 ms after. Earlier
comparisons against the morning control suggested larger cumulative gains;
use this refreshed pair for the final elapsed-time claim. Disk-backed cold
compile remains variable and has no demonstrated material improvement.

The separate source profile establishes 9.1% fewer kernel instructions during
compile, including 74.8% less file-cache hash-table work. Final small-file
fsync and rename/directory sync take 48.252 and 40.681 ms in one trace-enabled
sample; both use two flushes versus six and seven in the original control.
Clean file/directory sync issues no writes or flushes, and orderly shutdown
uses two flushes versus eleven. Counts are the stronger storage evidence.

Reports: `/private/tmp/perf-0922-ram-control-refresh`,
`/private/tmp/perf-0922-retained-ram`, and
`/private/tmp/perf-0922-retained-small`. These measurements precede the Stage 9
read-coalescing follow-up; its separate results are recorded above. Hardware
gains and cache-footprint effects remain optional T420 follow-up.

### RCU core and descriptor pilot

The RCU core has no allocating reader path or shared reader lock. Readers
mask interrupts and cannot migrate or switch threads; nested IRQ/NMI readers
share the CPU's atomic nesting/generation state. Only resident, nonblocking
lookups belong inside the guard. A caller retains its normal lease before
leaving. Pointer publication and reader admission use sequentially consistent
atomics; the outermost reader exit releases its generation.

Four focused native tests pass, including 80,000 concurrent reads against
2,000 publications and reclaims. The existing concurrency-smoke module gains
one bounded four-vCPU case: a nested reader on CPU 2 delays reclamation on
CPU 0, then exits; full-queue backpressure and explicit drain reclaim all 34
objects. The guest stops after that case passes. Artifacts are
`rcu-native-tests.log`, `rcu-native-bench.json`, and `rcu-core-smoke` under the
Stage 1 artifact directory, plus `/private/tmp/perf-0922-rcu-core-smoke`.

Native ARM64 protocol medians are 3.17 ns for one reader versus 1.27 ns for an
uncontended shared spinlock. Across four host threads, aggregate throughput is
1.17 billion reads/s versus 43.4 million for the shared spinlock. Idle or
already-completed grace scans take roughly 0.5 ns for one state and 1.1-1.4 ns
for four. These exclude kernel IRQ masking and scheduling, and do not predict
uncontended kernel latency; the guest descriptor workload is the acceptance
measurement.

Descriptor lookup now traverses one of 16 sorted RCU-linked buckets and pins
its existing `DescriptorLease`. Mutations still hold `m_FdLock`; install or
replacement allocates one entry, close allocates none, and removed links stay
intact until a synchronous grace period completes. Private retiring owners
keep blocking descriptor teardown outside that lock. Bulk fork/exec updates
publish replacement chains and drain old readers once. This adds a membership
index, not another descriptor ownership or callback-lifetime protocol.

The final matched QEMU pairs use the same payload and root image, normal
non-smoke configuration, and one guest per side and CPU count. Each guest
runs three rounds: 100,000 `F_GETFD` calls per worker, 16,384 `dup`/`close`
pairs at 32 and 256 live aliases, and a highest-descriptor lookup at 256
aliases. Longer phases avoid the visible timer quantization in the initial
shorter samples. The table reports median guest elapsed milliseconds:

| Workload | 1 CPU before | 1 CPU RCU | 4 CPUs before | 4 CPUs RCU |
| --- | ---: | ---: | ---: | ---: |
| Lookup, one worker | 166.442 | 125.275 | 191.365 | 132.157 |
| Lookup, four workers | 687.186 | 535.455 | 353.136 | 222.096 |
| Lookup at 256 aliases, one worker | 176.400 | 139.696 | 193.296 | 145.517 |
| `dup`/`close`, 32 aliases | 190.997 | 208.597 | 212.338 | 213.008 |
| `dup`/`close`, 256 aliases | 215.109 | 233.517 | 221.461 | 221.146 |

One-vCPU single-worker throughput rises from 600,810 to 798,244 calls/s
(**32.9%**). Four-vCPU, four-worker throughput rises from 1,132,708 to
1,801,023 calls/s (**59.0%**). The tradeoff is **8.6-9.2%** more one-vCPU
mutation time; four-vCPU mutation time is essentially unchanged. These are
`fcntl` syscall workloads, not isolated instruction costs or a whole-kernel
speedup. Retain the pilot for read-heavy work; compare compile/link before
extending the migration.

Normal kernel text grows by 1,152 bytes and POSIX module text by 2,571 bytes
against the pre-RCU control. The existing four-vCPU vector-I/O contract passes,
including concurrent `dup2` generation replacement, shared offsets, pipe EOF,
and event/timer/signal descriptor dispatch. Basic fork/exec markers also pass.
No full compiler matrix or unrelated integration suite was repeated.

Final artifacts under the Stage 1 directory: `rcu-fd-control5`,
`rcu-fd-linked`, `rcu-vector`, `rcu-fd.c`, and `rcu-final-summary.json`.
Guest reports: `/private/tmp/perf-0922-rcu-final-{b1,a1,b4,a4}` and
`/private/tmp/perf-0922-rcu-vector`. The normal build has
`PEDIGREE_CONCURRENCY_SMOKE_TESTS=FALSE`. Changes remain uncommitted.

### Descriptor compiler qualification

The existing compiler matrix now supports `--mode quick --storage ramfs`:
CPU controls around one tiny build, preprocess, link, and full build. It
keeps input identity and zero-disk-I/O checks while reducing a development
comparison from 38 phases to six. This is a single sample without warmup,
not a replacement for repeated measurements when claiming a small gain.

The pre-RCU control and descriptor candidate both pass with identical RAM-root
and input identities and no measured disk requests. Host-wall seconds:

| Workload | Pre-RCU | Descriptor RCU |
| --- | ---: | ---: |
| Tiny build | 0.539 | 0.523 |
| Preprocess | 1.291 | 1.235 |
| Link | 1.031 | 1.037 |
| Full build | 18.810 | 18.860 |

Full build and link are essentially unchanged in this pair. The descriptor
syscall throughput gain does not establish a whole-build gain. Both guests
complete in about 40 seconds including RAM-root setup. Reports:
`/private/tmp/perf-0922-rcu-quick-b2` and `/private/tmp/perf-0922-rcu-quick-a1`;
shared fixture `rcu-quick-root2`, frozen `rcu-quick-control2` and
`rcu-quick-candidate2` under the Stage 1 artifact directory.

### VFS tracked-file ownership

The existing compiler profile records 17,977 established-file retains and
17,929 releases, but only 69 final releases. The old tracker serialized these
operations and updated a tree on every reference change. Its replacement uses
256 RCU-linked buckets and atomic owner counts. Ordinary retain/release needs
no allocation or global mutex. Final release closes retain admission under
the writer lock, unlinks the record, and waits for readers before destruction.
File destruction remains outside the lock so it can release parent ownership.
Existing namespace locks, `ChildLease`, and mount-view ownership are preserved.

Control and candidate include the retained descriptor optimization and use
identical configuration, payload, and backing image. One short QEMU boot per
version and CPU count runs three rounds, then races path lookup against
unlink/recreate. Median guest throughput:

| Workload | CPUs | Before (operations/s) | VFS RCU (operations/s) | Gain |
| --- | ---: | ---: | ---: | ---: |
| Path lookup, one worker | 1 | 18,055 | 20,391 | 12.9% |
| Path lookup, four workers | 1 | 17,658 | 21,951 | 24.3% |
| Unlink/create/close cycle | 1 | 3,794 | 4,144 | 9.2% |
| Path lookup, one worker | 4 | 14,139 | 16,328 | 15.5% |
| Path lookup, four workers | 4 | 17,591 | 22,617 | 28.6% |
| Unlink/create/close cycle | 4 | 3,278 | 4,025 | 22.8% |

These are short TCG workload measurements, not hardware or whole-kernel gains.
All four guests pass the concurrent unlink/recreate contract and basic
launch/fork/exec checks. The 60 focused native VFS, directory, mount-view, and
RCU tests pass, including two new ownership races. Normal kernel/initrd and
native builds pass. Native utilities use a shared-lock RCU platform adapter;
the QEMU checks exercise the kernel's per-CPU implementation.

One additional quick RAM-root compiler run passes with matching input identities
and zero measured disk I/O. Descriptor-only to VFS candidate host-wall seconds:
tiny 0.523 to 0.556, preprocess 1.235 to 1.237, link 1.037 to 1.058, and full
build 18.860 to 19.007. These single samples establish no compiler speedup.
The quick-mode runner's 18 protocol tests also pass.

Reports: `/private/tmp/perf-0922-vfs-rcu-{b1,a1,b4,a4}` and
`/private/tmp/perf-0922-vfs-rcu-quick`. Frozen artifacts: `vfs-rcu-control`,
`vfs-rcu-candidate`, and `vfs-rcu-quick` under the Stage 1 directory. The guest
kernel/initrd hashes match the normal build, with concurrency smoke tests off.

The remaining launch check uses the same frozen `gcc --version` payload and
one guest per version, with three launches plus fork/exec probes. Both pass.
Cold launch is 126.443 to 126.065 ms in the guest; warm samples are 28.507/19.450
to 26.650/19.641 ms. Timer quantization is visible in these short phases, so no
launch speedup is claimed. Reports: `/private/tmp/perf-0922-close-launch-pre-vfs`
and `/private/tmp/perf-0922-close-launch-vfs`.

### Remaining VM and registry assessment

The compiler profile contains 213,415 mapping searches and 1,673,922 tree-node
visits, averaging 7.84 visits per search. `MappingList` accounts for 0.81% of
kernel instruction dispatches; all `MemoryMapManager` work accounts for 4.13%.
A last-hit cache avoids repeated searches within a mapping and is invalidated
by every membership mutation. The existing `OperationGuard` still serializes
mapping lifetime, range reservations, and page population. An RCU conversion
would also need durable mapping leases and is not justified by this profile.

All four native mapping-index tests pass, including cached-hit invalidation
after split rollback, removal, and replacement. The four-vCPU anonymous-memory
contract passes concurrent fault/protection/unmap/reuse in one address space,
surviving-range data checks, fork/CoW, and child exit. Report:
`/private/tmp/perf-0922-close-anonymous`. Together with VFS and RCU checks, the
focused native run passes 64 tests.

Timer dispatch is 0.345% of kernel instructions, event work 0.145%, and retained
process lookup 0.304%; thread lookup is negligible in this workload. Event
dequeue mutates the queue and is not an RCU membership read. IRQ and page-fault
dispatch account for 1.478% and 1.805%, respectively, but already admit readers
without a global lock. The earlier page-fault retirement-scan reduction is
retained. No further registry migration is selected.

### Thread-owned accounting

CPU-time publication updates the running Thread and requests timer reporting
only when interested. Process queries aggregate live Thread counters and
retired totals under a dedicated accounting lock; thread removal transfers its
totals exactly once. This removes per-process per-CPU allocation and duplicate
process-counter writes from the publication path. Topology mutation takes the
accounting lock inside the process lock; queries take only the accounting lock,
avoiding inversion with timer, signal, and parent/child-state locks.

The four-vCPU accounting contract passes live aggregation, worker retirement,
sleep exclusion, concurrent timer queries, cross-thread CPU timers, fresh fork
accounting, and exec retention. One/four-vCPU `getrusage`/`wait4` checks also pass.
Reports: `/private/tmp/perf-0922-close-accounting` and
`/private/tmp/perf-0922-close3-a{1,4}`. This is a reduction in publication work;
no isolated elapsed-time gain is attributed to accounting.

### Initial syscall-handler retirement experiment

At checklist completion, normal syscall dispatch used an atomic handler load
without taking a lock and retained its module-unload lifetime limitation. A trial replaced
the separate synthetic-dispatch tracking with one admission protocol for normal
and synthetic calls. Short RCU sections plus a shared callback counter regressed
cheap calls, so a second trial used resident per-CPU admission counters and
immutable handler/entry publication. Both allowed callbacks to block or migrate
and drained them before reclaiming their registration.

The final integrated trial still regressed the four-worker cheap-query workload: the
median for one million calls rose from 181.455 to 193.098 ms (**6.4% more**).
This sample does not justify retaining the extra admission machinery. The
syscall-dispatch changes and per-CPU barrier facility were removed. Safe normal
syscall-handler unloading was deferred to the follow-up below. Rejected artifacts: `close-candidate*` under the Stage 1
directory; reports `/private/tmp/perf-0922-close4-b4`,
`/private/tmp/perf-0922-close5-a4`, and `/private/tmp/perf-0922-close-quick3`.

Two simpler changes from the investigation are retained.
`OperationBarrier` uses an atomic closed/count word for ordinary admission and
release, retaining the wait-queue lock for the final closed release and drain.
Thread-owned cleanup-list publication uses IRQ-masked atomic loads/stores
instead of compare-and-swap. LIFO validation remains, and the cleanup sequence
keeps its atomic increment because debug/fault exceptions may advance it.

### Final accounting and synchronization comparison

The control already includes descriptor and VFS RCU. The candidate adds the
accounting, barrier, Thread cleanup-list, and mapping-cache changes above; it
keeps the original lock-free syscall dispatch. Identical payloads run three
rounds per guest, with one/four-vCPU controls refreshed after experimentation.
Median guest syscall throughput:

| Workload | CPUs | Before (calls/s) | Retained (calls/s) | Gain |
| --- | ---: | ---: | ---: | ---: |
| `getuid`, one worker | 1 | 1,766,834 | 2,054,485 | 16.3% |
| `getuid`, four workers | 1 | 1,885,490 | 2,168,699 | 15.0% |
| `getuid`, one worker | 4 | 1,771,567 | 2,027,411 | 14.4% |
| `getuid`, four workers | 4 | 5,511,008 | 6,141,903 | 11.5% |

The one-vCPU anonymous-touch sample changes from 191.125 to 186.982 ms and
fragmented mapping from 91.782 to 84.635 ms. Four-vCPU samples change from
554.424 to 578.895 ms and 90.545 to 91.801 ms. These short samples establish no
consistent VM elapsed-time gain; the cache is retained for the direct removal
of repeated tree searches. Syscall gains are workload results, not an attribution
to each individual change or a whole-kernel speedup.

Both CPU counts pass the anonymous-memory, `getrusage`/`wait4`, and launch/fork/exec
contracts in the same short guest. Reports:
`/private/tmp/perf-0922-close-retained-{b1,a1,a4}` and
`/private/tmp/perf-0922-close4-b4`. Frozen candidate: `close-retained`; timings:
`close-retained-summary.json` under the Stage 1 artifact directory.

The final six-phase RAM-root compiler qualification passes with matching input
identities and zero measured disk I/O. Host-wall seconds versus the preceding
VFS candidate: tiny 0.556 to 0.542, preprocess 1.237 to 1.233, link 1.058 to
1.017, full build 19.007 to 19.517. This single sample establishes no consistent
whole-build speedup. Report: `/private/tmp/perf-0922-close-retained-quick`.
The final four-vCPU Linux/native syscall ABI contract also passes:
`/private/tmp/perf-0922-close-retained-abi`.

The final four-vCPU entry contract passes all four workers, including TLS/errno,
nonzero GS preservation, and 32 signal deliveries per worker. The diagnostic
guest passes RCU publication/reclamation, pinned-module unload rejection,
reciprocal callback removal, producer/consumer teardown, pre-start cancellation,
and terminal stack unwind. The barrier check now verifies rejection after close
and that releasing one of two admissions cannot complete the drain. Reports:
`/private/tmp/perf-0922-close-entry` and `/private/tmp/perf-0922-close-smoke`.

The normal kernel/initrd rebuild passes with concurrency smoke tests off and
sampled accounting enabled. Kernel, initrd archive, all 50 archived payloads,
and generated configuration exactly match the final timed fixture. Verification:
`close-final-verification.json` under the Stage 1 artifact directory.
`git diff --check` passes. All stages are closed; changes remain uncommitted.


## Post-checklist: reads and syscall-handler unloading

- [x] Reassess remaining RCU consumers against the integrated profile. Descriptor
  and VFS membership remain the strongest targets; the VM/registry assessment
  above still does not justify another conversion.
- [x] Measure sustained sequential reads with a 128 MiB fixture and 128 KiB
  `pread` requests, separating cold and cached rates.
- [x] Increase the regular-file read window from 64 to 128 KiB, retaining 16 KiB
  AHCI commands and the small-buffer allocation fallback.
- [x] Complete safe normal syscall-handler retirement and measure its cost.

Two untraced one-vCPU comparisons measure cold throughput at 94.34/95.55 MiB/s
before and 101.08/104.42 MiB/s after (**7.1%/9.3% higher**). Cached samples are
237.85–247.09 before and 242.91–252.86 MiB/s after; these overlapping ranges do
not establish a repeatable cached-read gain. All 128 MiB are checked after each
read timer. The benchmark now accepts up to 256 MiB and reports `mib_per_s`
directly; all 15 benchmark protocol/parser tests pass.

A traced pair confirms that maximum outstanding NCQ commands doubles from four
to eight. Both versions read exactly 128 MiB in 8,193 commands (8,191 at 16 KiB,
one at 4 KiB, one at 12 KiB); cached repeats issue no disk reads. Traced cold
rates are 95.81 and 96.86 MiB/s, so trace timing is not used for the headline
gain. This is a read-window/overlap improvement, not a command-count reduction
or a measurement of physical SSD bandwidth.

Frozen fixtures: `sustained-{control,candidate}` under the Stage 1 directory.
Reports: `/private/tmp/perf-0922-sustained-{b1,a1,b2,a2,bt,at}`. The second
untraced pair and trace pair use the same backing chain; the first baseline
boots the equivalent flattened root image.


### Safe syscall-handler retirement

Normal x64, hosted, and synthetic dispatch now share immutable handler/entry
publication and per-Thread admission. Unregistration closes admission and scans
scheduler-protected Thread membership until old calls finish. Callbacks may
block or migrate; ordinary entry/exit takes no shared registry lock. Busy
self/reciprocal removal fails without losing registration ownership, while an
idle peer can be unloaded from another callback. A brief Closing state prevents
failed idle-removal attempts from rejecting otherwise valid calls.

One cleanup record combines termination deferral with exceptional retirement.
The Thread-owned lease chain permits nested exec to retire an ancestor without
losing the active inner return action or retaining an abandoned stack pointer.
The production dispatcher and the focused lifecycle test use the same path.
This replaces the old separately locked synthetic-call tracking; it is not an
RCU section held across a blocking callback.

Fresh controls and the corrected candidate each run three rounds per guest.
Median cheap-call time (lower is better):

| Workload | CPUs | Before (ms) | Safe retirement (ms) | Added time |
| --- | ---: | ---: | ---: | ---: |
| 250,000 `getuid`, one worker | 1 | 120.808 | 132.320 | 9.5% |
| 1,000,000 `getuid`, four workers | 1 | 466.301 | 532.184 | 14.1% |
| 250,000 `getuid`, one worker | 4 | 130.765 | 141.374 | 8.1% |
| 1,000,000 `getuid`, four workers | 4 | 175.537 | 181.472 | 3.4% |

This is an explicit correctness/performance tradeoff against unsafe raw
publication, not another speedup. The admission protocol is retained to make
normal handler unloading safe. Reports:
`/private/tmp/perf-0922-unload-{b1,b4}` and
`/private/tmp/perf-0922-unload2-{a1,a4}`; frozen candidate `unload2-candidate` and
`unload-summary.json` under the Stage 1 directory.

The normal one/four-vCPU workload passes anonymous memory, process accounting,
and launch/fork/exec checks. A combined four-vCPU guest passes Linux/native ABI,
TLS/GS/errno with 32 signal deliveries on each of four workers, and vector I/O.
A real userspace `pedigree-c` call unloads `native`, confirms it is absent, and
safely issues a call to its now-empty service on both one and four CPUs;
self-unloading `pedigree-c` remains rejected. Reports:
`/private/tmp/perf-0922-unload-contract` and
`/private/tmp/perf-0922-unload-module{,1}`.

Both SMP diagnostic runs pass the new blocked-callback drain, late rejection,
self/idle-peer removal, handler/entry reuse, abandoned-stack cleanup, and nested
exec action tests, followed by RCU and reciprocal syscall removal. The broader
run then fails the unchanged NetworkFilter callback-lifetime test; the frozen
pre-change kernel reproduces the same failure. Its fixed-yield startup handshake
can miss the remover before advancing phase, so this is recorded separately,
not counted as a passing broader suite. Reports:
`/private/tmp/perf-0922-unload-smoke{4,4b,-control}`. A one-vCPU diagnostic attempt
cannot reach this module because the earlier USB reciprocal test requires two
different CPUs; normal one-vCPU execution and real module removal pass above.


The four-vCPU durability/shutdown guest also passes, including orderly poweroff
after POSIX admission closes and callbacks drain:
`/private/tmp/perf-0922-unload-shutdown2`. This checks the shutdown path, not a new
fresh-guest persistence qualification.

The final combined kernel reads the 128 MiB fixture at **100.34 MiB/s cold** and
234.36/238.32 MiB/s cached, with full byte verification. An earlier combined run
measured 65.66 MiB/s cold and a later frozen read-window-only control measured
66.12 MiB/s. Those slow samples report 0.533/0.498 s of aggregate QEMU backend
read time versus 0.129 s in the final run; both versions show the variation.
These samples therefore do not establish an unloading-induced storage
regression or a physical bandwidth ceiling. Reports:
`/private/tmp/perf-0922-unload-{sustained,read-control,read-final}`.

The normal build is restored with concurrency smoke tests disabled. Kernel,
initrd, and generated configuration match the timed `unload2-candidate` fixture
exactly (`unload-final-verification.json`). `git diff --check` passes. Ruff reports
the same six pre-existing runner findings (EXE001, I001, BLE001) before and after
the throughput metric; no new finding is introduced. All follow-up checklist items above
are complete, with the unrelated broader-smoke failure recorded explicitly.
Changes remain uncommitted.

### Linux read control

A stock Alpine Linux 6.12.110-0-lts kernel runs the identical static read
benchmark against the same ext2 filesystem images and 128 MiB pattern file.
QEMU 11.1.1, q35/AHCI, one SandyBridge vCPU, 4 GiB RAM, multithreaded TCG,
firmware, disk backing chains, and default host caching match. Linux boots
through QEMU's kernel/initramfs handoff using the same UEFI firmware; its minimal
init mounts the ext2 root read-only. Kernel and module downloads come from
[Alpine's official distribution](https://dl-cdn.alpinelinux.org/alpine/v3.22/releases/x86_64/netboot/).

Each OS/order has one fresh guest, one cold pass, and two cached passes.
Sequential requests are 128 KiB; random requests are a deterministic permutation
of all 4 KiB blocks, each visited once. The destination is prefaulted, all bytes
are verified outside the read timer, and QEMU tracing is disabled. Cold means
guest-cache cold; the host's backing-file cache is not flushed.

| Workload | Pedigree cold (MiB/s) | Linux cold (MiB/s) | Pedigree cached (MiB/s) | Linux cached (MiB/s) |
| --- | ---: | ---: | ---: | ---: |
| Sequential, 128 KiB requests | 101.83 | 526.45 | 238.06 / 242.52 | 2,219.64 / 2,245.73 |
| Random, 4 KiB requests | 21.92 | 38.29 | 159.36 / 163.44 | 291.15 / 300.03 |

Every cached phase issues zero backend disk reads on both kernels. Linux is
5.17x faster cold and 9.29x faster cached for sequential reads in this pair;
random ratios are 1.75x and 1.83x. The cold sequential phase uses 8,193 backend
read requests on Pedigree versus 1,058 on Linux; random uses 32,768 versus
32,801. Linux also reads 132 KiB of filesystem metadata within each cold phase.
These are short controls, not physical-disk bandwidth or tail-latency results.

Source and frozen-kernel disassembly identify two full copies in Pedigree's
buffered read path: cache to bounce buffer, then bounce buffer to user memory.
Both use `rep movsb` for large copies. A temporary copy-only control in the
Linux guest copies 128 MiB, checking contents outside the timer. After one
complete warmup pass, its second pass reaches 540.47 MiB/s with `rep movsb`,
3,592.76 MiB/s with `rep movsq`, and 3,659.95 MiB/s with the benchmark's musl
`memcpy`. Only this warmed pass is used for the comparison.

Two copies at the warmed byte-copy rate imply roughly 270 MiB/s of delivered
data before other work, close to the measured 238–243 MiB/s. This is strong
evidence for investigating copy cost under TCG, not a kernel A/B attribution
or a hardware prediction. The same Linux guest repeats the sequential workload
at 528.66 MiB/s cold and 2,299.39/2,306.43 MiB/s cached. Ramfs throughput was
not measured in this control. The next read experiment should compare copy
implementations and reducing bounce copies while preserving partial-fault and
mapping-lifetime semantics, before expanding RCU or transport concurrency.

Reports: `/private/tmp/perf-0922-linux-{seq2,ped-seq,random,ped-random,copy}`.
Download provenance, benchmark identity, checked metrics, copy source, and the
temporary adapter for the existing runner are under
`/private/tmp/pedigree-linux-control`; `summary.json` records their identities.
No production kernel code changes for this comparison.

### Further allocation work

The scheduler's ready queues, affinity queue, and deferred-reap queue already
embed links in stable objects. Ready-queue enqueue/unlink needs no allocation;
RoundRobin accounts for only 0.121% of the earlier kernel instruction profile.
Replacing these containers is not a promising next optimization.

Mapping membership currently allocates separate list and tree nodes. Combining
their links in one membership record could remove an allocation and indirection
without changing O(log n) lookup. The record must belong to the registry:
replacement plans can temporarily place one mapping object in both published
and staged registries. A single hook embedded in that object would be wrong.

Anonymous-page tree work accounts for 3.420% of the earlier instruction profile.
Its page value already lives in the tree node, so an intrusive rename alone
saves nothing. Sparse chunks of page metadata could amortize first-fault
allocations, but must preserve sparse mappings, fork/CoW, swap, and partial
unmap. This is a stronger but broader experiment than the mapping-membership
change. Thread-start staging queues offer a smaller opportunity to embed a
reusable hook in their existing startup record. None of these assessments
establishes a measured speedup or removes the need for lifetime synchronization.
