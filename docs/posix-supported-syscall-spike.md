# Supported POSIX syscall repair spike

The preceding ABI and I/O repair sprint is committed locally as `cac7c6d6e`.
This follow-on spike repairs existing syscall paths; it adds no Linux syscall
mappings.

## Changes

| Area | Repaired behavior |
| --- | --- |
| User copies | Guarded copies materialize demand pages and check access under the VM operation lock. Socket messages, address results, socket options, directory records, metadata, console requests, execution arguments, groups, and process-query results use bounded kernel snapshots. Blocking socket operations use staged payloads. |
| Namespace mutation | VFS rename reserves both names, validates the operation, commits the backend change, then publishes the new namespace. RamFs and Ext2 implement the transaction. Ext2 replacement preserves open references to the replaced inode and updates moved directories' parent links. |
| Truncation | Fallible resize reports backend failures. RamFs and Ext2 preserve data before the new EOF and zero discarded tails and subsequent growth. Resize revokes mapped cache pages before changing the backing file, preserves private pages on backend failure, and discards whole private pages beyond the new EOF after success. |
| Ext2 aliases and writes | Hardlink aliases share metadata, block maps, mutation locks, and a native-page cache, including mapped writes and `fsync`. Metadata reports actual link and allocated-block counts. Writes into existing sparse holes allocate the required direct or indirect blocks and preserve holes and size on allocation failure. Write syscalls preserve fresh backend errors and partial progress. |
| VM | `PROT_NONE` retains backing, protection changes preserve copy-on-write behavior, invalid ranges and holes fail before partial mutation, shared mappings respect descriptor permissions, and cache-page loans survive fork without duplicate physical-page ownership. Shared writable mappings prepare backing storage before publishing writable pages. Access to whole file pages beyond EOF delivers `SIGBUS`; guarded kernel copies fail with `EFAULT`. |
| Free address ranges | RangeList coalesces adjacent address ranges even when unrelated entries separate them in storage, allowing allocations across completely freed spans. Copies preserve allocation preference, and self-assignment preserves ranges. File-map resize visits resident records rather than scanning every virtual page. |
| Futex and thread identity | Linux task IDs are globally distinct while internal thread indices remain stable. Futex wait comparison and queue publication share a synchronization boundary; requeue transfers queue ownership. Shared file mappings use a stable backing identity. Robust cleanup and clear-child-TID materialize accessible demand and private COW pages in the owner context before unmapping. Successful exec releases robust ownership at commit; failed exec preserves registrations. |
| Thread exit | The final Linux thread exit reserves process termination under the process lock, including concurrent exit and clone publication, so the parent can reap the process and observe its exit status. |
| TLS and socket setup | Linux TLS setters preserve the caller's memory and permit a zero FS base. Unix socket binding follows a mounted parent directory consistently with connection lookup. Bound pathname sockets survive final close until unlink, and socket rename/replacement preserves open endpoint references on RamFs and Ext2. Explicit datagram destinations override the default peer for that send; queue admission respects nonblocking mode and propagates errors. |
| Descriptor metadata | Internal empty-path normalization supports descriptor-based metadata operations, including `fchmod`, without dereferencing an empty String's null storage. |

The new guest contract suites are `--usercopy-contracts`, `--fs-mutation-contracts`,
`--vm-contracts`, and `--futex-contracts` in `testsuite`.

## Verification

- The final native cross-build of the kernel, userspace suite, ISO, and HDD
  succeeded after forcing recompilation of the changed VM interface's consumers.
  [Build log](/private/tmp/pedigree-posix-audit-20260905/pass2-final-build.log).
- All **ten guest suites exited zero with one and four virtual CPUs**: syscall
  contracts, VM, user copies, futex, filesystem mutation, SCM_RIGHTS datagrams
  and streams, Unix stream interruption, epoll/PTY, and resource limits. Runs
  took 37.9 and 37.2 seconds respectively. RamFs symlink coverage is explicitly
  skipped because that backend remains unsupported; Ext2 symlink checks pass.
  [One-CPU log](/private/tmp/pedigree-posix-audit-20260905/spike-pass2-final-1cpu.serial.log),
  [four-CPU log](/private/tmp/pedigree-posix-audit-20260905/spike-pass2-final-4cpu.serial.log).
- **106 native tests passed**, covering VFS, Ext2, file/directory contracts,
  wait diagnostics, RingBuffer, Tree, and RangeList. The rename test pauses the backend while
  lookups wait, then verifies identities after successful and failed commits.
  Another test checks release/reload of inactive Ext2 block maps without changing
  futex identity. New cases cover alias cache ownership and writeback, metadata,
  sparse allocation and failure preservation, resident-tree iteration, and
  free-range coalescing/copy behavior.
  [Native test log](/private/tmp/pedigree-posix-audit-20260905/pass2-final-native-tests.log).
- **42 routing and signal checks passed**. The Linux syscall mapping header is
  unchanged. [Routing log](/private/tmp/pedigree-posix-audit-20260905/pass2-final-routing.log).
- Five hosted regression translation units compile: static syscall, VM
  permission, futex/robust, system user-copy, and file-contract fixtures. They were **not linked
  or executed**; the Linux hosted runtime is unavailable locally. Their checks
  therefore provide no additional runtime coverage claim.
- Changed-line formatting and `git diff --check` passed. Guest execution used a
  disposable HDD clone, a freshly built ISO, and QEMU snapshot mode. These are
  bounded checks, without sanitizer, endurance, or crash-durability evidence.
- Verification ran in the shared working tree, which also contained earlier
  serial-console and musl private-signal changes. Those changes are excluded from
  this checkpoint; the results are not an isolated-checkout verification claim.

## Failures investigated during the second pass

The second pass extends the same guest suites with mapped shrink, shared and
private EOF behavior, five robust-memory cases, pathname socket lifetime and
replacement, and Ext2 hardlink metadata/cache checks. All these cases pass in
the final runs above.

The first second-pass guest run exposed an empty-path `fchmod` kernel fault,
which is repaired and covered by the hardlink metadata case. It also failed a
combined sparse mapping release/zero-fill assertion. Splitting that assertion
and rebuilding did not reproduce the VM failure in the diagnostic or either
final run; these green runs do not identify its cause.
[Initial guest log](/private/tmp/pedigree-posix-audit-20260905/spike-pass2-initial-4cpu.serial.log),
[diagnostic guest log](/private/tmp/pedigree-posix-audit-20260905/spike-pass2-diagnostic-4cpu.serial.log).

The follow-up found an independent deterministic RangeList defect: freeing
`[0,64)`, `[640,704)`, `[128,192)`, then `[64,128)` left the fully free
`[0,192)` span unavailable to allocation. Coalescing now considers all later
fragments while preserving unrelated ranges' allocation order. Native
regressions cover this case, late bridges, copy policy, and self-assignment.
This repair is not proof of the earlier guest failure's cause.

## Third pass: checked writeback and metadata

This pass starts at `48756ea35`, is checkpointed as `c59181482`, and adds no syscall mappings. Earlier private-signal
changes remain in the shared working tree and are outside this pass.

- File, mapping, cache, and disk sync paths return checked results. Ext2 backend
  failures reach `fsync` and `msync` as `EIO`; unmapped `msync` ranges remain
  `ENOMEM` and fail before submitting any pages. Multiple pages are attempted
  even after a failure, and retries preserve successful partial progress.
- Failed cache writes remain dirty despite unchanged checksums. Eviction retains
  their bytes, background retries are bounded per scan, and the shared Ext2 inode
  state keeps failed fill pages after the last linked alias closes. Reopening can
  retry without using an object whose destructor has already run.
- Full Ext2 regular-file sync checks data, indirect mappings, inode metadata, and the relevant
  allocation bitmaps, descriptors, and superblock counters. SCSI sync uses the
  caller's pinned page directly. Both ATA controller paths now dispatch checked
  device-cache flushes. Ordinary writes retain asynchronous data submission.
- Successful and partial ordinary writes update mtime and ctime; zero progress
  preserves them. Hardlinks observe the same metadata. RamFs reports allocated
  pages in 512-byte units, including sparse growth and truncation. Existing
  `utime`, `utimes`, and `futimesat` paths use seconds consistently and validate
  microseconds and the currently supported unsigned 32-bit timestamp range.
- The original VM failure has better diagnostics and 24 guarded sparse split /
  removal cases per guest suite. Hosted cases separately inspect mapping objects,
  reservations, PTEs, page loans, and zero contents. No further VM ownership defect
  was established, so the original intermittent failure remains unresolved.

The first integration attempt exposed `PEDIGREE_CRIPPLE_HDD=TRUE` in the shared
build. This disables actual disk writes; checked writeback correctly returned
failure where the earlier void path reported success. Persistence validation
therefore uses a separate ISO built with that flag false and a disposable HDD
under QEMU snapshot mode. The shared build's original setting is restored after
validation. Earlier guest passes are cache behavior evidence, not disk-persistence
proof.

A write-enabled one-CPU run then exposed a one-second RequestQueue watchdog
panic while the lower disk queue was progressing. Device operations already have
longer bounded deadlines, so an interval without a completed outer request is a
warning, not proof of deadlock. Queue sampling remains active; backend deadlines
still determine I/O failure. Ordinary write submission was also restored to avoid
scheduling a device flush for every write. The failed runs are preserved in the
pass artifact directory.

Final verification:

- **126 native tests passed**, including injected data-read, constituent-write,
  inode/bitmap writeback, host `msync`, partial-progress, retry, close/reopen,
  timestamps, and allocation-accounting cases.
  [Native log](/private/tmp/pedigree-posix-pass3-20260905/native-final-tests.log).
- **42 routing/signal checks passed**; syscall translations are unchanged.
  [Routing log](/private/tmp/pedigree-posix-pass3-20260905/routing-tests.log).
- **All ten guest suites exited zero on one and four CPUs**, with disk writes
  enabled, explicit end markers, and snapshot isolation. Runs took **50.9s and
  42.8s**. Both include all 24 new sparse VM cases and Ext2/RamFs metadata checks.
  The one-CPU run logged two intervals without CacheManager completion, then
  resumed and finished; this confirms those samples were not terminal deadlocks.
  RamFs symlink tests remain explicitly skipped.
  [One CPU](/private/tmp/pedigree-posix-pass3-20260905/spike-final-1cpu.serial.log),
  [four CPUs](/private/tmp/pedigree-posix-pass3-20260905/spike-final-4cpu.serial.log).
- **Nine hosted regression files compile**, including public `fsync`/`msync`
  failure propagation and SCSI failed-flush/pin ownership. They were **not linked
  or executed**: the local Linux hosted runtime remains unavailable.
  [Compile log](/private/tmp/pedigree-posix-pass3-20260905/hosted-final-compile.log).
- Twelve repetitions of the original VM suite also passed on four CPUs before
  these edits. Neither those repetitions nor the expanded cases establish the
  cause of the earlier intermittent failure.
  [Baseline log](/private/tmp/pedigree-posix-pass3-20260905/spike-baseline-vm-4cpu.serial.log).
- Changed-line formatting and `git diff --check` passed. Both the write-enabled
  test build and the restored write-inhibited shared build succeeded. Unrelated
  baseline edits were preserved; this remains shared-working-tree verification.
  No sanitizer, physical-device failure, endurance, or power-loss testing was run.

Full commands, preserved failures, and the owned patch are in the
[pass handoff](/private/tmp/pedigree-posix-pass3-20260905/handoff.md).

## Fourth pass: directory persistence, FAT, and VM execution

This pass starts at `c59181482` and keeps the existing syscall set. Private-signal
changes remain outside the checkpoint.

- Ext2 directory `fsync` checks directory data and namespace metadata, including
  child inodes and allocation changes in other groups. Parents retain new child
  directory data and moved `..` block dependencies across aliases, close/reopen,
  and failed writes. Dependencies are block identities resolved through fresh
  pinned reads. The metadata sweep conservatively includes loaded filesystem
  metadata; it does not add journaling or crash-atomic rename.
- FAT cache writeback now checks complete data and metadata submission, retaining
  dirty producer pages on failure. Retry uses absolute metadata values; failed
  mirrored FAT writes, allocation reservations, and whole-chain reclamation keep
  retryable state. Growth zeroes newly visible bytes before publishing size.
  Metadata snapshots count actual allocated clusters, including empty allocated
  files, using the correct FAT16/FAT32 entry widths. Symlink creation preserves
  its existing behavior through checked writes before publishing the name.
- The shared sparse mapping ownership and public `msync` fixtures run during
  POSIX initialization when `PEDIGREE_VM_OWNERSHIP_SMOKE_TESTS=TRUE`. The test build
  requires a root disk and leaves the existing disk-free concurrency smoke
  module unchanged. The fixtures check four sparse
  masks, mapping/PTE removal, page loans, reservation reuse, every replacement
  byte, `ENOMEM` preflight, `EIO`, continued page attempts, and successful retry.

Native verification passes **151 tests**, including 11 new Ext2 directory sync
cases and 14 FAT cases. The fake disks separate cached and persisted bytes and
inject failures, including retry after partial progress and failed reclamation.
The **42 routing/signal checks** also pass. Ten hosted regression translation
units compile; this alone does not establish hosted execution.
[Native log](/private/tmp/pedigree-posix-pass4-20260905/native-final-tests.log),
[routing log](/private/tmp/pedigree-posix-pass4-20260905/routing.log),
[hosted compilation](/private/tmp/pedigree-posix-pass4-20260905/hosted-final-compile.log).

All ten ordinary guest suites pass on one and four CPUs (**49.5s / 40.6s**).
The new `--fs-persistence-write <directory>` and
`--fs-persistence-read <directory>` modes also pass across two boots of the same
disposable disk on each CPU configuration. The reader verifies every payload
byte, file size/link/block metadata, retired names, and the moved directory's
stored `..` inode. QEMU is terminated after the writer's completion marker;
there is no guest shutdown or global sync between phases. The runner requires
write on the first boot and read on the second, so losing the completion marker
cannot silently restart the test. Read-only `e2fsck -fn` passes on the baseline
and both resulting disks. This is reboot-persistence evidence, not a claim of
physical power-loss or crash-atomic rename behavior.

Guest runs use fresh copied ISOs with `PEDIGREE_CRIPPLE_HDD=FALSE`, serial output,
no display/network, per-suite status/end markers, and 180-second deadlines.
Ordinary suites use snapshots; each persistence pair uses its own retained
qcow2 overlay. FAT fault/retry coverage is native execution with fake disks,
without an additional FAT guest-persistence claim.

The focused VM fixtures execute successfully on **one and four CPUs**
(**21.1s / 22.4s**), including all four ownership cases and the public `msync`
failure/retry checks. The final marker occurs after worker joins and process
teardown. Older hosted-only clone/resize/refcount fixtures remain compile-only;
the original intermittent split/unmap failure remains unresolved.
[One CPU](/private/tmp/pedigree-posix-pass4-20260905/vm-1cpu.serial.log),
[four CPUs](/private/tmp/pedigree-posix-pass4-20260905/vm-4cpu.serial.log).

Early VM test-module attempts failed relocation before executing fixtures.
`posix_msync` is intentionally hidden, so the final test source is linked inside
POSIX under the optional flag; no production symbol visibility changed. Failed
logs are retained. The normal shared build is restored with runtime disk writes
disabled and both smoke flags false. Unrelated baseline edits are preserved;
verification used the shared working tree. Exact commands, persistence logs,
filesystem checks, and limitations are in the
[pass handoff](/private/tmp/pedigree-posix-pass4-20260905/handoff.md).

## Remaining boundaries

These repairs do not establish general musl conformance. The following existing
capability gaps remain material before making that claim:

- Resize can still return `EBUSY` for physical-page loans outside the managed
  file-mapping paths. The mapped-shrink work is validated on amd64; the hosted
  implementation is compile-checked, and other architectures lack this repair.
- Ext2 growth still allocates intervening blocks. Repairing writes into existing
  sparse holes does not establish sparse growth or allocation-policy parity.
  RamFs counts allocated pages and FAT counts allocated clusters; other backends
  still need a separate block-accounting audit. FAT12 lacks dedicated native
  coverage; malformed chains cannot report an attribute error through the
  current interface and instead warn and report zero blocks.
- Ext2 retains a small identity state and registry entry for each touched linked
  inode until unlink or unmount. Inactive block-map capacity is released after
  successful cache drain; failed writeback retains it for retry. A fully bounded
  identity registry needs explicit futex-waiter lifetime ownership.
- Robust cleanup remains bounded. Inaccessible or corrupt lists are skipped,
  and foreign-context teardown retains nofault-only access. Owner-context COW,
  demand paging, protected-memory preservation, and successful exec now have
  explicit guest coverage; this is not exhaustive robust-mutex conformance.
- Rename provides runtime namespace atomicity, not crash consistency. Ext2 has
  no journal. Unsupported filesystem backends return an error; this spike does
  not add RamFs symlinks or FAT mutation capabilities.
- The VM permission work is validated on amd64; other non-hosted architectures
  reject unsupported `PROT_NONE`. Checked writeback now covers Ext2 through
  SCSI/ATA, FAT, and buildutility DiskImage. Ext2 directory sync is checked;
  other disks without checked sync return failure. Terminal backend destruction
  reports unwritten pages but cannot recover them afterwards. Other directory
  backends still need checked sync paths. Legacy `brk` and stack regions are
  not newly represented as VM mapping objects.
- Existing stubs and unsupported futex operations remain deferred, as do the
  broader signal restart, terminal timing, and threaded-exec conformance gaps.

The next phase should close or explicitly scope these boundaries before treating
all currently mapped syscalls as complete. New syscall entry points can then be
added against the same public-wrapper contract suites.
