# Storage performance checklist

This checklist covers ext2/AHCI latency first, then read throughput, FAT, and
ISO9660. QEMU is the development and completion environment. A measured QEMU
gain with the relevant persistence checks is sufficient to finish the
checklist. ThinkPad T420 testing is an optional manual follow-up.

**2026-09-22: ext2/AHCI durability and read-coalescing work complete in QEMU.**
Conditional follow-on work remains explicitly deferred below.

Sustained sequential throughput is a post-checklist follow-up. The current
cold 8 MiB QEMU sample reaches 79.68 MiB/s after coalescing; larger-stream
throughput and the remaining limiting path have not been established.

The starting point is the existing batched writeback described in
[Batched filesystem sync](sync-writeback-spike.md): dirty ext2 pages can already
reach AHCI in NCQ waves, but metadata helpers, clean syncs, and shutdown can
still issue several serialized device-cache flushes.

## Rules

- [x] Use the existing I/O latency, directory-sync, and QEMU storage tools before
  adding instrumentation.
- [x] Compare identical one-vCPU QEMU fixtures for latency. Add four vCPUs when
  changing shared writeback, interrupt, or completion state.
- [x] Record payload command count, bytes, flush count, and elapsed time when the
  QEMU trace provides them.
- [x] Use a disposable disk and verify `PEDIGREE_CRIPPLE_HDD=FALSE` for
  persistence tests.
- [x] Preserve ordering, retry, and clean-marker behaviour. A false durability
  result is a regression even if it is faster.
- [x] Prefer one focused failure test over a large synthetic matrix. Use a fresh
  guest or offline filesystem check when it establishes something the focused
  test cannot.
- [x] Do not add test-only production branches, hooks, modes, or exported state.
- [x] Stop after a measured bottleneck is removed; do not automatically complete
  the lower-priority sections.
- [x] Do not leave an agent or task waiting for physical hardware. Record the
  optional T420 follow-up and complete the QEMU-qualified work.

## Core QEMU workloads

- [x] Overwrite a small ext2 file and call `fsync()`.
- [x] Create and write a small ext2 file and call `fsync()`.
- [x] Write a temporary file, rename it, and sync the parent directory.
- [x] Repeat an immediate clean file and directory sync.
- [x] Run global `sync()` after dirty and clean phases.
- [x] Perform a clean orderly shutdown and verify the filesystem on the next
  boot.

Add large sequential, random, FAT, or ISO9660 rows only while working on those
paths.

## 1. Establish the command baseline

- [x] Run the core workloads on a frozen one-vCPU fixture.
- [x] Use existing QEMU block/ATA traces to count payload writes and flushes.
- [x] Correlate benchmark phases with trace timestamps or retained serial
  markers.
- [x] Record the four-vCPU result only if the current change will affect
  concurrency.
- [x] Add subsystem diagnostics only if the external trace cannot distinguish
  the candidate mechanisms and the diagnostics have ongoing operational value.

**Done when:** the small-file, clean-sync, global-sync, and shutdown costs can be
explained by command counts closely enough to test the next change.

## 2. Separate submission from durability

**Goal:** let a filesystem submit all pages in one ordering phase, then issue one
checked device barrier.

- [x] Add an explicit payload-submission operation beside the existing durable
  operation.
- [x] Forward both contracts through partitions and SCSI/AHCI layers.
- [x] Keep a conservative durable fallback for backends that cannot separate
  the operations.
- [x] Preserve retry state after partial payload or final barrier failure.
- [x] Extend the narrowest existing cache/SCSI tests for the new contract.

**Done when:** no payload-only API can be mistaken for durability and ext2 can
express several writes followed by one barrier.

## 3. Batch ext2 metadata by ordering phase

**Goal:** make small-file `fsync()` depend on ext2 ordering phases, not the
number of metadata pages touched.

- [x] Write down the required order for data, allocation/mapping metadata, inode
  publication, and namespace metadata.
- [x] Collect a bounded, duplicate-free dirty page set for each required phase.
- [x] Submit the pages, then issue the phase barrier.
- [x] Skip clean resident metadata pages.
- [x] Preserve quota and xattr ordering where those features participate.
- [x] Cover an existing file, a newly allocated file, and one injected phase
  failure using existing tests where possible.
- [x] Verify the persisted result in a fresh guest.

**Done when:** the QEMU trace shows bounded barriers, clean file sync performs no
payload writes, and failed work remains retryable.

## 4. Preserve batching across lower-cache aliases

Do this only if the new-file trace shows that lower SCSI-cache aliases force the
batch path back to per-page writes.

- [ ] Merge or partition direct pages and aliased pages without bypassing cache
  coherence.
- [ ] Keep page lifetime and partial-failure ownership explicit.
- [ ] Cover the alias created by new-block zeroing; add other cases only if the
  same mechanism handles them differently.
- [ ] Re-run existing-file and new-file workloads.

**Done when:** a newly allocated multi-page file retains bounded submission and
all cache views observe the new data.

## 5. Avoid redundant barriers

The existing AHCI submission gate excludes later writers until a flush
completes. A whole-port pending flag implements the obligations below without
a generation counter; traces showed redundant clean barriers.

- [x] Mark device writes pending for every write that may reach volatile
  device storage.
- [x] Clear pending state only after a successful barrier.
- [x] Exclude later submissions through flush completion so a concurrent later
  write remains pending.
- [x] Share pending state at the whole-device level across partitions.
- [x] Cover clean repetition, an intervening direct write, a concurrent write,
  and a failed barrier.

**Done when:** clean repeated syncs issue no second hardware flush, while every
intervening write still forces one.

## 6. Finish ext2 namespace and shutdown

- [x] Make directory sync operate on dirty directory blocks and required
  namespace metadata rather than every resident block.
- [x] Preserve create, rename, replacement, unlink, and orphan ordering.
- [x] Keep logical shutdown phases, but let a phase skip the hardware barrier
  when the device generation is already durable.
- [x] Keep the final ext2 clean marker last and restore unchecked state after a
  failed marker write.
- [x] Verify rename plus directory sync and clean/dirty shutdown in fresh guests.

**Done when:** directory-sync work follows actual mutation and clean shutdown
contains no flush without an intervening write.

## 7. Consider background writeback batching

Do this only if a many-small-files workload shows one barrier per inode or a
visible read-latency burst.

- [ ] Group only caches on the same device and compatible ordering phase.
- [ ] Keep inode/cache lifetime through payload completion and the shared
  barrier.
- [ ] Preserve newer dirty generations and failed-page ownership.
- [ ] Ensure foreground `fsync()` can wait for or join background work without a
  deadlock or false durability result.

**Done when:** barrier count follows writeback waves rather than inode count and
foreground durability remains synchronous.

## 8. Improve AHCI payload throughput

**Done for reads:** adjacent whole pages share bounded 16 KiB commands. Writes
remain separate after their coalescing trial regressed latency.

Start only when payload command setup or copies dominate after barrier work.

- [x] Measure command size/count, NCQ depth, bounce copies, completion latency,
  and polling fallback.
- [x] Coalesce adjacent compatible pages up to current AHCI and ATA limits.
- [x] Preserve per-input status after a coalesced command.
- [x] Cover noncontiguous input and one partial completion failure.
- [x] Attempt direct PRDT mappings only if bounce copying remains material;
  retain the bounce fallback.
- [x] Revisit timeout waits or MSI only when completion overhead is isolated and
  the existing interrupt contract remains intact.

**Done when:** the measured workload uses fewer or larger commands without
regressing random/noncontiguous I/O or failure cleanup.

## 9. Improve reads only when measured

- [x] Establish cold/warm sequential, random, and executable-launch results.
- [x] Add bounded sequential read-ahead only if command granularity limits the
  sequential case.
- [ ] Cancel speculation under random access or memory pressure.
- [x] Batch ISO9660 extent reads if its base per-page implementation is visible
  in an actual workload.
- [x] Cache optical geometry only when repeated READ TOC traffic is observed.
- [x] Verify EOF, failed reads, cache eviction, and no unexpected dirty pages.

**Done when:** command count and latency improve in the target read workload and
small/random reads remain within normal variance.

## 10. Apply the model to FAT

FAT follows ext2 because it is not the primary T420 development path.

- [ ] Document the crash order for allocation, mirrored FAT/FSInfo updates,
  zeroed/data contents, file metadata, and namespace publication.
- [ ] Batch each required phase behind one checked barrier.
- [ ] Remove a linked-file close barrier only if dirty state remains safely owned
  for explicit `fsync`, global sync, unmount, or terminal unlinked retirement.
- [ ] Narrow the filesystem-wide mutation lock only if waits are measured and
  rollback/lifetime state remains protected.
- [ ] Cover the FAT variant and namespace operation actually used by the target
  workload; broaden coverage only for shared code with distinct on-disk rules.
- [ ] Verify the result after remount or reboot.

**Done when:** FAT barriers follow documented dependency phases rather than
helper calls, and data plus namespace survive remount.

## Checklist completion

- [x] Re-run the affected core QEMU workload on the retained source.
- [x] Show lower latency or a meaningful reduction in payload commands, flushes,
  copies, or completion work.
- [x] Pass the focused storage tests and fresh-guest persistence check relevant
  to the changed path.
- [x] Record remaining hardware questions as optional follow-up.

The storage checklist is complete at this point. A T420 result is not required.

## Post-checklist: optional T420 confirmation

When the user has time and prepared media, run the QEMU-qualified source on the
T420 using the existing native `g++` baseline and whichever small-file,
shutdown, launch, or sequential-read workload is useful. Verify persistent data
after reboot when applicable. Record drive/controller, link, cache, NCQ,
interrupt, and CPU information only to the level needed to explain differences.

This is a separate user-assisted qualification task. It must not keep an agent
run or the underlying storage work open.

## Result note

For a retained slice, record:

- starting commit and relevant dirty files;
- workload and baseline;
- before/after latency and command counts;
- focused tests and guest/persistence check;
- artifact path if logs need to be retained;
- commit and remaining uncertainty.

Stage 2 of [the kernel roadmap](kernel-performance-roadmap.md#stage-2-ext2-durability-2026-09-22)
records the 2026-09-22 ext2/AHCI result, failure tests, command counts, and fresh
four-vCPU persistence plus clean-marker verification. Submission-only APIs
already existed; they were reused. Lower-cache alias batching (section 4) was
not selected: the measured small-file case no longer pays per-page barriers.
Background batching (section 7) and FAT (section 10) were not selected by the
integrated profile. Read-ahead was not added, so its cancellation item remains
unchecked.

The coalescing trials are recorded in
[Stage 9](kernel-performance-roadmap.md#stage-9-ahci-read-trials-2026-09-22).
The retained 16 KiB read grouping reduces sequential command count by 75% and
cold elapsed time by 17.1% in a matched short QEMU comparison. Random reads
remain within normal variance. Focused failure tests and one SMP read smoke
pass. Earlier rare random-read stalls remain unexplained; the short follow-up
does not establish their cause or claim a fix. Larger read commands and write
coalescing remain rejected after latency regressions.
Read-ahead, zero-copy DMA, completion redesign, background inode batching,
and secondary-filesystem changes were not selected. Checked conditional
assessment steps do not claim implementations of those alternatives.
Final accumulated guest checks are recorded in the roadmap.
