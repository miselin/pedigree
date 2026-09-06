# musl syscall implementation

The amd64 expansion backlog starts at `0867c8927`: 108 untranslated syscall
numbers referenced by the bundled musl source plus five mapped operations with
no successful valid implementation. The [inventory](musl-syscall-implementation.csv)
tracks all 113. A mapping alone does not close an item; its scope and public musl
contract evidence must be recorded.

Current checkpoint: 91 of 113 backlog entries implemented; 22 remain.

Work proceeds by families, starting with IPC, then timers and signal integration,
VM and descriptor-backed objects, file operations, and process/resource features.
Simple queries and aliases follow the larger ownership and blocking contracts.
Linux administration and isolation remain explicit work items; an error-only
placeholder or unconditional success is not implementation evidence.

Each completed pass receives a local commit. Queue, timer, and process-interface
state belongs in POSIX. Shared kernel/VFS changes should express a reusable
lifetime or behavior contract that cannot be implemented correctly inside the
subsystem. Public-wrapper guest tests use fresh headless images, disposable
disks, per-suite exit statuses, and one/four-CPU runs for concurrent behavior.

## File synchronization, clock adjustment and module removal

fdatasync uses existing file and Ext2 metadata writeback. sync_file_range flushes
cached pages overlapping the requested byte range; every nonzero valid flag
combination completes synchronously. Zero length extends through EOF. Failed
pages remain available for retry while independent successful pages progress.
No filesystem-wide or device-wide durability fence is implied by this range
operation. readahead and POSIX_FADV_WILLNEED populate the real cache, preserve
OFD positions and bound each prefetch to 2 MiB. Linux also treats zero-count
readahead as WILLNEED through EOF. Other recognized advice remains explicitly
unsupported. Global sync and syncfs still require filesystem-wide writeback.

adjtimex and the realtime clock_adjtime route support unprivileged queries,
privileged signed ADJ_SETOFFSET, and persistent MICRO/NANO output units. Clock
steps reuse existing serialization, vDSO publication and realtime waiter/timerfd
notifications without changing monotonic time. Replies report STA_UNSYNC and
nonnegative TIME_ERROR; successful adjustment does not assert synchronization.
PLL/FLL, frequency tuning, gradual slew, leap/TAI handling and other discipline
modes remain unsupported. Invalid offsets are rejected before mutation; an
output-copy fault may follow a committed change, matching Linux's copyout order.

Privileged delete_module removes active, unloadable modules through the existing
single-owner cleanup path. Completed records do not hide later live instances;
lookup distinguishes missing, busy, pinned, depended-on and shutdown outcomes.
Execution admission is serialized with unloading before dependency checks or
relocation. Forced unload is unsupported. init_module still requires a fallible,
validated loader transaction and owned image storage before accepting user data.

Verification: 37 routing/ABI checks, 47 focused native filesystem tests, nine
cross source compiles, ten hosted compile-only sources, a full image build, and
actual Darwin hosted core execution passed. The latter exercises the production
execution/unload admission helper. All 158 affected image source/object pairs
were current, and 797 strong POSIX imports had available exports. Fresh serial
one/four-CPU guests passed all 34 suites in 244.2/212.1 seconds. New public
contracts ran against RAM-backed and Ext2 files, clock steps and privilege gates,
and actual module exit/destructor markers. Disk writes were enabled only for
disposable test disks; shared build settings were restored. These file tests do
not claim cold-boot persistence or a new device-wide flush guarantee. Evidence:
/private/tmp/pedigree-mount-expansion-20260906/completion-verification.json.

The initial focused compile exposed a local descriptor-helper name collision;
renaming that helper resolved it. The guest runner's initial preflight rejected
the new digit-bearing suite name; its parser now admits digits. Both failures
were preserved, and no guest had launched before the preflight correction.

## Mount lifetime prerequisite

VFS now provides copyable filesystem pins for long-lived path ownership. A live
pin rejects ordinary unregistration without changing the mount table or closing
access admission. Registry shutdown closes new admission and drains pins outside
publication locks; existing pins remain valid and copyable until released.
Identity tokens remain independent and cannot revive a retired registration.

This is a prerequisite for attachment-aware paths and pivot_root, with no new
syscall counted. Existing Process and descriptor paths have not yet adopted the
pin. All 35 native VFS tests passed, including concurrent admission/removal and
shutdown/copy behavior. Affected cross compiles and the actual Darwin hosted
kernel's idle-close admission contract passed. Evidence is under
/private/tmp/pedigree-mount-expansion-20260906/.

## Cooperative tracing

ptrace now supplies a successful amd64 Linux TRACEME path for a consenting,
single-threaded child. The exact creating task owns GETREGS, GETREGSET,
GETSIGINFO, CONT and DETACH; another thread in the parent can consume shared
wait reports but cannot control the tracee. Task tokens survive exec and close
before retirement, avoiding PID reuse and permanent Process/Thread pins.
Credentials and dumpability are checked at enrollment and inspection. Native
callbacks, multithreaded enrollment, ATTACH/SEIZE, memory/register mutation,
stepping and other optional trace modes remain explicitly unsupported.

The complete 216-byte amd64 register image includes selectors, FS/GS bases and
original syscall RAX. Entry metadata belongs to the saved machine frame; the
x64 return tails that consume these frames restore it. Exec exposes and enters
one canonical frame.
GETREGSET supports NT_PRSTATUS, aligned partial/oversized lengths and Linux's
ordered data-then-iov_len stores. Copyout and invalid-request failures preserve
the immutable stop. GETSIGINFO supplies 128 bytes and distinguishes signal,
group and exec stops. Group stops report EINVAL for GETSIGINFO; CONT releases
them, while SIGCONT alone preserves their inspectable tracing stop.

Signal selection occurs before disposition consumption. CONT can suppress,
preserve or replace the signal; blocked replacements return to ordinary pending
selection. Replacement allocation and queue admission precede resume commitment,
so ENOMEM/EAGAIN preserve the original stop. Stack abandonment releases selected
deliveries, parked-state flags and prepared replacements. SIGKILL bypasses the
parked gate. Stop publication supplies CLD_TRAPPED wait reports and SIGCHLD,
respecting SA_NOCLDSTOP. WNOWAIT retains reports; consuming one does not resume
execution. Tracer loss detaches, and successful tracer or tracee exec preserves
the surviving relation.

Verification passed 37 routing/ABI checks, 23 cross source compiles, 20
hosted compile-only sources, the full image build and actual Darwin hosted core
runtime. Fresh headless one- and four-CPU guests passed all 30 public suites in
225.0 and 187.0 seconds; all 316 affected image source/object pairs were
current. Disk writes were enabled only on disposable disks, and build settings
were restored. Results and saved image hashes are in
/private/tmp/pedigree-tracing-expansion-20260906/verification.json.
Public families cover registers, inspection, signals, ownership, lifecycle and
errors. A separate static ELF records its first resumed instruction before any
interpreter or CRT, independently checking the exec
snapshot. Hardware metadata round-trip checks cover GS-base restoration;
public FS-base comparison uses ARCH_GET_FS. Deterministic allocation-failure
injection and a negative SA_NOCLDSTOP observation were not exercised. Real queue
exhaustion and stop preservation are covered.

The initial shared-kernel build rejected direct relocations to the new assembly
helpers. Protected visibility binds those internal calls while retaining module
exports. Initial guests then exposed a test fixture's use of a private kernel
IRQ guard; the fixture now saves/restores IRQ state explicitly. Failed images
and logs are preserved. A preflight confirms all strong POSIX imports have
available image exports, with runtime still responsible for load-order proof.

## Child wait and non-consuming observation

waitid supports P_ALL, P_PID and P_PGID, exited/stopped/continued events,
WNOHANG and WNOWAIT. Reports include numeric real UID, cause and exit/signal
status. Group-zero selection snapshots the caller group at entry. Known clone
and PIDFD forms remain explicitly unsupported. Cooperative tracing reports use
the same selection engine.

waitid and wait4 share child selection, interruption handling, reaping and final
CPU accounting. WNOWAIT retains the Process through off-stack completion without
consuming its event or accounting it to the parent. A concurrent consumer can
claim the single reaper; destruction waits for retained observers after removing
the child from new lookup. Stop/continue peeks preserve a pending transition
until consumption or replacement. A fully exited child cannot keep waits for
only stop/continue events eligible.

Process-group membership now survives through zombie observation. Previously,
exit removed it before wait could select the zombie by group. The existing
PosixProcess destructor removes membership after lookup removal and observer
drainage, keeping both numeric selection and group-member pointers valid.

The raw waitid ABI accepts optional siginfo and rusage. It writes only the six
defined siginfo result fields, preserving padding and unrelated fields; no-result
and error paths clear those fields. wait4 writes status before rusage; waitid
writes rusage before siginfo. Copyout failures can consume events, while WNOWAIT
retains them. Existing cancellation/restart handling is shared, and waitid joins
the restartable syscall set. The Linux rusage prefix is 144 bytes; musl's reserved
tail remains untouched.

Verification passed 37 routing/ABI checks, six cross source compiles, seven
hosted compile-only sources, the full image build and actual Darwin hosted core
runtime. The native fixture covers transition selection and a retained terminal
observer alongside the sole reaper. Its exact parked-reaper check uses a hosted
hook and is explicitly skipped in guests. Fresh headless one- and four-CPU guests
passed all 29 public suites in 214.8 and 175.7 seconds, respectively; all 187
affected image source/object pairs were current. Disk writes were enabled only
on disposable disks. The six new public families cover selectors, event classes,
lifetime/accounting, ordered copyout, admission errors and interruption.
Artifacts, saved image hashes and results are recorded in
/private/tmp/pedigree-wait-expansion-20260906/verification.json.

Failed candidates are preserved. A selector fixture initially assumed a real
bootstrap process group where getpgrp instead fabricated a PID; it now creates
an explicit group, and the bootstrap inconsistency remains a session follow-up.
The next run exposed the repaired zombie group-lifetime bug. An existing
scheduling fixture also assumed immediate numeric task removal after musl's
pthread_join; bounded retirement observation now precedes its stale-ID checks.

One four-CPU run stalled in the existing affinity kernel fixture before the new
wait fixture. Later runs of both the same saved image and the final image passed;
phase diagnostics and bounded source review did not establish a cause. This
remains an unresolved scheduling follow-up, not a claimed repair. The separate
parked VM split/unmap fault is unchanged.

## Scheduling and CPU placement

Ten new Linux syscall routes supply scheduling queries, ordinary-policy
validation, CPU affinity and getcpu. Static priority zero under SCHED_OTHER
remains the actual scheduler policy; valid unsupported policies return
EOPNOTSUPP, malformed parameters EINVAL. Priority-bound queries describe the
Linux policy ranges. sched_rr_get_interval reports the active scheduler timer's
nominal quantum, without promising an observed scheduling latency. Four direct
bundled musl policy wrappers still return ENOSYS locally; public pthread
scheduling interfaces use the implemented raw routes.

Affinity addresses Linux task IDs and inherits from the actual creator through
pthread creation and fork. Exec retains the surviving task's mask. Masks use
logical online CPU indices, independently of firmware/APIC identities, with
capacity for 1024 CPUs. Foreign changes require effective root or a matching
real/effective target UID. Queries release target leases before user copy.
getcpu returns the actual logical CPU and node zero; the former constant-zero
vDSO now reaches the same query.

A source-CPU coordinator commits affinity policy only while the target is
off-stack. A blocked syscall can acknowledge the new policy while retaining its
existing kernel continuation on its original CPU. Physical movement occurs at
an audited clean return gate before the next user instruction. Gate waits
recheck pending signal/stop work, and the final transition remains
IRQ-disabled. This avoids moving arbitrary kernel continuations that hold
CPU-local references. Queued work retains independent Thread/Process pins
through cancellation and wake publication.

Legacy native Input callbacks pin the task to their owner CPU until
irreversible exec/exit retirement; removing the public callback stops producers
but retains the image's pin. Excluding that CPU returns EOPNOTSUPP, while
containing masks remain usable. Generic Pedigree user signal callbacks and
explicit affinity use mutually exclusive process domains because nested
callbacks can retain lower kernel continuations. Modern Linux signal frames
remain supported. PCI ATA workers receive fixed BSP placement at construction
so their queue wake binding remains stable.

Verification passed 37 routing/ABI checks, affected cross and hosted source
compiles, the full cross image build and the actual Darwin hosted core runtime.
All 28 public suites passed in fresh headless guests on one CPU (211.5 seconds)
and four CPUs (168.3 seconds). The new suite covers six scheduling families,
including exact task permission checks, registers/TLS across movement, blocked
waits, signal/cancellation, ordinary and nonleader exec, concurrent setters,
retired task IDs, and native Input pin retention/reset. The core fixture forces
held wake publication, policy acknowledgement before movement, clean-gate
movement, counted callback pins and terminal cancellation. All 496 affected
image source/object pairs were current. A further four-CPU run passed all 28
suites in 166.4 seconds with APIC IDs 0, 1, 2 and 4; the installed QEMU
topology was independently confirmed through its monitor. Disk writes were
enabled on disposable disks. Logs, saved images, hashes and results are recorded in
/private/tmp/pedigree-scheduler-expansion-20260906/verification.json.

Earlier failed runs are preserved and explained: nested use of the new status
publisher under a plain Thread lock caused a bootstrap deadlock; explicit
locked operations repaired it. A four-CPU run then found the constant-zero
getcpu vDSO. Subsequent placement runs exposed stale Thread capture during
arbitrary kernel-continuation migration. A diagnostic reproduced the mismatch
at construction, before deferred-scope registration; restricting physical
migration to audited return gates repaired that path. The diagnostic remains
test-only and still fails on an identity mismatch.

Signal/stop arrival specifically during an architectural gate wait, nonzero
bootstrap topology slots, generation exhaustion, and actual hardware Input
callback delivery were source-reviewed or remain unavailable, rather than
claimed as deterministic runtime coverage. One-CPU migration/exclusion cases
are explicitly skipped. No CPU hotplug or real-time scheduling is added. The
parked VM split/unmap fault remains unchanged.

## UTS namespaces and task membership

`unshare(CLONE_NEWUTS)`, `setns` and `setdomainname` provide independent UTS
namespaces. The existing `uname` and `sethostname` paths now read and update
the caller's namespace. Names accept 0–64 bytes, preserve embedded NUL bytes
through the supplied length, and zero the remaining ABI field. Linux uname
exports all six 65-byte fields. Namespace administration uses effective UID
zero; filesystem IDs do not grant that authority.

Bindings are task-local and inherit from the actual creator through fork and
pthread creation. Fork-shaped clone accepts CLONE_NEWUTS and prepares the child
namespace before publication. Exec retains the survivor's membership, including
nonleader exec. Other namespace types and sharing combinations remain
unsupported; unshare(0) succeeds without changing state. This pass does not
claim mount, PID, IPC, network, user or cgroup isolation.

`/proc/self/ns/uts`, `/proc/thread-self/ns/uts`, `/proc/<pid>/ns/uts` and
`/proc/<pid>/task/<tid>/ns/uts` provide dynamic links to namespace descriptors.
Process views select the current leader; exact task views retire with their
original binding and cannot resolve a reused task identity. Following or
reading another process's link checks caller filesystem UID/GID against all
target real/effective/saved IDs and target dumpability. Same-process access is
exempt; there is no capability override. An open descriptor pins the namespace
independently of its source task, and survives dup, fork, SCM_RIGHTS transfer
and non-CLOEXEC exec. Ordinary data access, writable opens, resizing and
mapping are rejected. O_PATH and namespace ioctls are not added.

Namespace state stays inside POSIX. The generic VFS change lets dynamic
symlinks return retained targets without parsing their displayed text as paths.
Procfs cache invalidation preserves stale directory lifetime. Namespace
creation is bounded to 256 live namespaces including the initial namespace,
reports ENOSPC at capacity, and never reuses its namespace identities. Failed
preparation leaves membership intact; dropping the last reference releases
capacity.

Creation/exit review also repaired a shared lifecycle contract: process
termination drains already-admitted thread creators before sealing publication.
An unstarted terminal thread cannot receive a late namespace binding after its
one-shot subsystem exit hook. The kernel fixture covers rejected publication,
preparation rollback, actual task retirement and context closure; the hosted
core fixture covers the creation-drain rendezvous. Checked shared allocation
and explicit unique-ownership transfer support prepared publication without
changing the existing pointer constructors.

Verification passed 62 selected native utility/VFS/credential tests (three new
pointer-ownership cases), 37 routing/ABI checks, affected cross and hosted
compiles, and the Darwin hosted core runtime including process-creation
termination drain. All 27 suites passed in fresh headless guests on one CPU
(214.9 seconds) and four CPUs (171.3 seconds), including five public namespace
families, capacity failure/retry, and the namespace lifecycle kernel fixture.
The existing memory, IPC, signal, descriptor and filesystem suites also passed.
All 356 affected image source/object pairs were current. Images, serial logs,
hashes, review notes and results are recorded in /private/tmp/pedigree-uts-
expansion-20260906/verification.json. No guest failures occurred in this pass.
Identifier exhaustion/reuse and an already-admitted setns racing fd replacement
were not forced; source identity/lease invariants were reviewed. The parked VM
split/unmap issue remains unchanged.

## Process memory and numeric credentials

`process_vm_readv` and `process_vm_writev` copy scatter/gather streams through
public musl wrappers. Each transfer imports bounded vector arrays, retains the
selected process and checks its image generation and authorization for each
page fragment. Reads and writes preserve completed prefixes when a later
fragment fails. PID/TID resolution releases thread leases before waiting for VM
operations. An exec image change stops the old transfer; a fresh call can use
the replacement image.

The shared VM layer supplies explicit-target ordinary-RAM page access without
switching the caller address space. Managed anonymous/file mappings honor
current permissions, CoW, lazy allocation and full-page EOF boundaries;
registered raw stack/heap and runtime mappings have explicit admission. Page
access marks the target leaf accessed/dirty, and shared file writes participate
in normal writeback. Direct physical mappings are rejected. Other architectures
and hosted execution currently return EOPNOTSUPP for this backend.

The POSIX credential record now validates complete UID/GID transitions,
preserves saved IDs across fork and publishes coherent snapshots. Numeric IDs
through UINT32_MAX-1 do not require account-database entries.
`setfsuid`/`setfsgid` maintain per-thread filesystem IDs, inherited at thread
creation and reset by exec or applicable ordinary ID setters. VFS traversal,
creation, chmod/chown and xattr access consume filesystem IDs; access() uses
real IDs. Supplemental groups are bounded at 32. Ordinary six-ID/group state
remains process-shared; musl's repeated thread setter protocol is supported,
but raw ordinary ID syscalls do not implement Linux per-task credential
separation.

Foreign memory copies require matching caller real UID/GID against all target
real/effective/saved IDs and target dumpability. There is no capability
override. PR_GET_DUMPABLE/PR_SET_DUMPABLE supply this policy state; they do not
implement core files. Effective/FS identity changes clear dumpability, and exec
resets it using real/effective IDs and executable readability. The legacy login
entry requires effective root; nonroot privilege elevation still needs a
separately trusted authentication/set-ID design. Early boot kernel
filesystem/configuration access is explicit and independent of the account
database.

Ext2 loads and writes high owner bits for Linux/Hurd creator layouts.
Unsupported creator formats are rejected before metadata access or mutation.
Combined chown publication preserves fields omitted with the sentinel. Public
tests cover high IDs on RamFs and Ext2; native tests cover the disk encoding
and unsupported creator admission. No new cold-boot owner-persistence claim is
made.

Verification passed 204 selected native tests (11 new), 37 routing/ABI checks,
32 affected cross compiles, 29 hosted compile-only checks, and the Darwin
hosted core runtime. The final fresh-image headless guests passed all 26 suites
on one CPU (159.0 seconds) and four CPUs (135.0 seconds), including five new
public process-memory/credential families and two kernel ownership/generation
fixtures. Disk writes were enabled on disposable disks. Logs, failed runs,
immutable images, source hashes and results are recorded in
/private/tmp/pedigree-process-memory-expansion-20260906/verification.json.
Initial boot failures exposed missing explicit bootstrap identity. The first
new backend fixture also undercounted a raw CoW alias's physical references,
causing premature free and subsequent corruption despite passing its immediate
assertions. Its two live references are now explicitly enrolled. Failed image-
generation and later init runs are preserved; they are not attributed to the
parked VM fault. The image-generation fixture uses independently mapped remote
pages so its replacement does not require splitting. A later public filesystem
test caught and repaired an EACCES-to-ENOENT overwrite during faccessat and
openat path traversal. Existing ProcFs owner metadata and separate-snapshot
IPC/signal permission decisions remain documented follow-ups in the credential
consumer review. The parked split/unmap fault is unchanged.

## File handles and fanotify

`name_to_handle_at` and `open_by_handle_at` export and reopen linked Ext2 regular
files. The opaque handle includes filesystem UUID, inode number and persistent
inode generation. Hardlink and rename aliases keep the same identity. Final
unlink prevents new decode while existing open descriptions remain usable;
reallocation advances the generation before publishing the inode. Exhausted
generations are not reused. Decode checks allocation and identity while holding
an inode admission reference, then creates a separately tracked file wrapper.

VFS now provides retained handle results, mount-operation leases and inert mount
identities. Mount retirement closes new admission, waits for active operations,
then retires observers outside publication locks. Long-lived marks retain only
identity and subscriptions. These contracts protect the new export paths; they
do not redesign every existing open-file/unmount lifetime. Private unregistered
filesystems, RamFs, directories and symlink handles remain unsupported. Decode
uses the current effective-root policy; Linux capabilities are not emulated.

`fanotify_init` supports notification groups with mandatory FAN_REPORT_FID,
CLOEXEC and NONBLOCK. Inode marks support MODIFY, ATTRIB, OPEN, CLOSE_WRITE and
CLOSE_NOWRITE on exportable Ext2 regular files. Alias wrappers publish through a
shared inode event source. Each record snapshots its handle and producer PID;
reading it needs neither a live producer nor a live inode. Retired inode or
mount sources release mark quota immediately and prevent further delivery.

The POSIX-owned queue holds 256 ordinary records and one overflow record,
coalesces adjacent records for the same producer and target, and integrates with
read/readv, poll, epoll, ioctl, dup, fork, exec and descriptor transfer. Short
record buffers preserve the head; copyout failure drops the selected record,
including a later EFAULT overriding a previously copied prefix. The last file
owner closes the group and wakes readers even if an epoll watch still retains
its state. Limits are 64 groups, 256 marks per group and 4096 global marks.
Existing readiness fanout may allocate; preallocated event storage is not an
end-to-end allocation-free guarantee.

Default event-FD mode, directory and mount marks, ACCESS events, permission
mediation and unprivileged group creation require further implementations.
Unsupported requests fail explicitly. Ext2 handle persistence does not promise
journaled or power-loss-atomic inode allocation.

Verification passed 193 selected native cases (20 new handle, mount and queue
cases), 37 routing/ABI checks, 20 affected cross and hosted compiles, and the
actual Darwin core runtime. All 125 affected image source/object pairs were
current. Fresh headless one- and four-CPU guests passed all 25 suites in 167.8
and 132.3 seconds, including six new public families and the prior kernel
memory-lock/remap fixtures.

Independent write-enabled disks passed writer and cold-boot reader stages on
both CPU configurations. Saved linked handles reopened exact payloads and
hardlink identities; deleted handles remained stale. All four read-only e2fsck
checks were clean. Host inspection independently matched handle UUID, inode
number and generation against disk metadata. These are fsync/close and cold-boot
checks, without a power-loss claim. The persistence image and final guest image
have identical kernel and all 45 module hashes; only the overflow test fixture
changed between them.

Initial guests exposed an unsupported-private-filesystem error mismatch,
corrected from ESTALE to EOPNOTSUPP. A subsequent overflow fixture exceeded its
unchanged deadline while creating hundreds of dirty files. The final fixture
uses 32 targets and nine distinct producer processes, preserving the 256-record,
one-overflow and recovery checks with less unrelated disk I/O. Failed runs,
review findings, image identities and full verification are retained under
`/private/tmp/pedigree-fanotify-handles-expansion-20260906/verification.json`.
Shared build settings were restored after saving the verification images. The
previously parked split/unmap fault remains outside this pass.

## Extended attributes

All twelve get/set/list/remove xattr syscalls now have successful public musl
paths. Nonempty `user.*` names are supported on regular files and directories
in RamFs, memfd and Ext2. Path, final-symlink and descriptor variants retain the
target throughout each operation. Current credentials control value access and
mutation, including supplementary groups and sticky-directory ownership. An
O_RDONLY descriptor can change metadata when the caller has write permission;
O_PATH descriptors are rejected. Other namespaces require their own policy
implementations and remain unsupported.

Names are limited to 255 bytes and values to 65536 bytes. Size queries, empty
values, binary data, CREATE/REPLACE and short buffers follow the musl-visible
contract. Inputs are copied before mutation; reads return a complete snapshot
before guarded user copyout. Failed input copies preserve the old value. Each
successful mutation updates ctime and publishes one attribute event without
changing mtime or atime. Attributes follow file identity through hardlinks,
rename, unlink with open descriptors, fork and descriptor transfer.

RamFs and memfd use an inode-owned store with 128 entries and 256 KiB of charged
storage per file, sharing a 16 MiB quota. Replacement charges net growth and
destruction releases the charge. Prepared temporary values are bounded per
operation and are not included in that shared quota. Memfd content seals do not
seal metadata.

Ext2 stores standard version-two external attribute blocks through `i_file_acl`.
It validates the on-disk layout, preserves opaque supported-format namespaces,
and copies shared blocks before mutation. EA sectors participate in inode block
accounting, fast-symlink classification, truncation and final inode retirement.
Mutation prepares storage and bounded writeback dependencies before publication.
Payload or metadata writeback failure retains dirty state and dependencies for
retry. Unsupported filesystem features are rejected explicitly. Storage is one
filesystem block per inode; multi-block EA values, EA inodes, ACL enforcement and
crash-atomic transactions are outside this implementation.

Verification passed 170 selected native cases (22 new xattr cases), 37 routing
and ABI checks, 15 affected hosted compiles, and the actual Darwin core runtime.
Native cases cover quota and conditional-operation races, complete snapshots,
malformed EA layouts, shared-block replacement, allocation/read failure
rollback, orphan retirement, and dirty dependency retention across failed sync
and retry. Fresh headless one- and four-CPU guests passed all 24 suites in 131.8
and 120.2 seconds, including seven public xattr families and the prior kernel
memory-lock/remap fixtures. All 560 affected image source/object pairs were
current.

Separate write-enabled, non-snapshot disks passed writer and cold-boot reader
stages on both CPU configurations. Each stage completed with zero status, then
QEMU exited; this is an fsync/close and cold-boot test, not a guest shutdown or
power-loss test. All four read-only e2fsck checks were clean. Host debugfs
independently decoded binary, empty and replaced attributes, shared hardlink
identity, and EA-only block accounting. Saved disks, image hashes and logs are
recorded in
`/private/tmp/pedigree-xattr-expansion-20260905/verification.json`.

The first two guests stopped before userspace because kernel placement new was
hidden from VFS module relocation. Exporting the existing placement new/delete
pairs repaired the linkage; the failed images/logs remain saved. The successful
build retained the same backend tests. Shared build settings were restored
after recording the separate verification images. The previously parked
split/unmap fault remains outside this pass.

## File-page offset remapping

`remap_file_pages` replaces an existing shared regular-file range at the same
virtual address using an absolute file-page offset. Compatible adjacent mapping
fragments from the same open description are accepted; independently opened
descriptions remain distinct even when they name one inode. A scalar origin ID
survives clone/split/remap without retaining an open description or extending
its advisory-lock lifetime. Current protections and write-upgrade ceilings are
preserved, and fresh mapping policy is checked against memfd seals.

Replacement reuses the prepared fixed-map transaction. Old pages and cache
loans are retired at their old file offsets, while untouched fragments preserve
their physical mappings and offsets. Default population is best effort;
MAP_NONBLOCK leaves an unlocked replacement lazy. Inherited locks become eager
unless the caller has FUTURE ONFAULT policy. Lock admission checks the full old
charge plus replacement length before publication, then commits the net charge.
File truncation continues to invalidate aliases according to backing offsets.

Address and length are independently rounded down. `prot` must be zero; flag
bits other than MAP_NONBLOCK are ignored. Invalid private/anonymous mappings,
holes and incompatible fragments fail without replacement. Internal mappings
without open provenance, raw allocations, direct mappings and SysV attachments
remain unsupported. Replacement is bounded to 65536 pages and the existing
4096-object transaction limit. There is no new nonlinear PTE format or physical
page-move operation.

The public contract families cover offsets, admission, lifetime, locks and
resize. The default-off `PEDIGREE_REMAP_FILE_PAGES_TESTS` fixture inspects actual
PTEs and offset-specific cache loans for lazy/default/locked population, failed
policy and metadata-capacity admission, unaffected slices, and real compactor
retention through unlock. Capacity rejection is not allocation-failure injection.

Verification passed 148 selected native tests, 37 routing/ABI checks, and nine
affected hosted compiles. The actual Darwin core runtime completed cleanly.
Fresh headless one- and four-CPU guests passed all 23 suites (104.6 and 98.2
seconds), including five new public families, four mapping cases, and the prior
four kernel memory-lock cases. All 289 affected image source/object pairs were
current. Logs and image identities are under
`/private/tmp/pedigree-remap-file-pages-expansion-20260905/verification.json`.

The initial guests correctly returned EIO from Ext2 MS_SYNC because the test
build disabled disk writes. Their logs are retained. The successful verification
used a separate write-enabled ISO with independent disposable snapshot disks;
this proves synchronous writeback success, without a reboot-persistence claim.
The shared build's write-disable setting and default-off fixtures were restored.
The separately parked split/unmap fault is unchanged.

## Memory locking and locked-memory limits

This pass adds `mlock`, `munlock`, `mlockall`, `munlockall`, `mlock2`, and
successful `setrlimit` mutation for RLIMIT_MEMLOCK (six backlog numbers).
`prlimit64` also reads and changes the caller's stored limit. The default soft
and hard limits are 16 MiB; an unprivileged caller cannot raise the hard limit.
Overlapping locks charge virtual pages once. Lowering the limit preserves
existing locks, while later admission checks the current limit. Limit copyout
follows mutation, so a failed old-limit copyout does not roll back an update.
Other resource mutations and cross-process limit access remain explicit work.

Eager locking populates accessible pages and resolves writable private CoW;
ONFAULT charges the full eligible range without forcing residency. File-backed
locks inhibit the actual mapping compactor and retain cache loans. Partial
unlock, unmap, protection splits, remap and prepared file shrink preserve exact
ownership and charge. Locked discard is rejected; MS_INVALIDATE reports EBUSY.
Direct eager population failure retains the established lock metadata and any
partial residency. CURRENT and mapping-prefault population are best effort.

The maintained amd64 ELF path inventories both managed mappings and raw heap
and stack owners. FUTURE quota admission precedes destructive fixed replacement
or new raw allocation. Input callbacks preallocate their fallback stack before
registration and retire it once after callback draining. Internal signal/event
runtime mappings are reserved and excluded from ordinary lock accounting;
user-provided musl TLS remains ordinary memory. Fork inherits resource limits
but clears child locks and FUTURE policy; successful exec clears both while
retaining limits. POSIX owns policy and credentials, VFS owns managed mapping
transactions, and core exposes a generic raw-owner admission interface.

Current scope limits:

- Complete CURRENT/FUTURE inventory is advertised only by the maintained amd64
  image path. Hosted and the optional native ELF loader report unsupported when
  their raw allocation inventory is incomplete.
- Linux brk continues to decline shrink. Generic native heap shrink retires its
  own surviving raw pieces and preserves managed replacements. Fixed mremap
  into raw-owned reservations remains unsupported.
- Plans are bounded to 4096 managed objects/raw extents and 65536 affected
  managed pages. Population reports typed backing/allocation failures; general
  allocation-failure injection is not supplied by this pass.
- SysV SHM_LOCK retains its separate implementation; per-real-UID shared-memory
  lock accounting still needs its own resource policy. It is not charged as a
  process's ordinary locked virtual mappings.

The `memory-lock-contract-test` application covers limits, ranges, managed and
raw mappings, fork/exec and threaded lifetime. The default-off
`PEDIGREE_MEMORY_LOCK_TESTS` fixture additionally checks physical-page/cache-loan
retention across the real file compactor, eager private copying, raw heap quota
rollback, and callback-stack denial/retirement/retry. Callback dispatch itself
and incomplete architectures are not claimed as runtime coverage.

Verification passed 148 selected native tests, 37 routing/ABI checks, and 16
affected hosted compiles. The actual Darwin core runtime completed its lifecycle
and clean shutdown. Fresh headless one- and four-CPU guests passed all 22 suites
(241.0 and 280.7 seconds), including five new public families and four
kernel ownership cases. All 413 affected image source/object pairs were current.
Disk writes remained disabled. Logs and image identities are under
`/private/tmp/pedigree-memory-lock-expansion-20260905/verification.json`.

Two initial boot failures are explained and retained: an unexported embedded account vtable, and
the shared tree iterator treating two end iterators as unequal. The latter is
repaired at the container boundary with symmetric empty/exhausted comparisons
and native regression tests. The separately parked split/unmap fault is unchanged.

## File shrink residency prerequisite

File shrink now prepares backing reads, cache retirement, and mapping-loan
accounting before changing the file. A failed preparation preserves the old
size, data, and mappings. Successful shrink retains resident pages below the
rounded-up new EOF, zeros the retained partial page, and retires only the full
suffix. Shared backing identities include hardlink aliases and mappings in other
processes. Truncated dirty pages are discarded without requiring new writeback.
This supplies the residency contract needed by file-backed memory locking; no
new syscall numbers are added by this prerequisite.

RamFs and Ext2 implement the prepared operation. Foreign physical loans reject
shrink with EBUSY before mutation. Other backends must supply the preparation
contract before supporting shrink. Journals are bounded to 4096 matching mapping
objects and 65536 tracked pages. Existing allocator teardown assumptions and
asynchronous disk-write scheduling remain; this is not a durable filesystem
transaction.

Verification passed 124 native tests, including cache cancellation and Ext2
read-failure preservation, and 37 routing/ABI checks. Eight affected hosted units
compiled. Actual threaded Darwin execution passed the prepublication drain,
cancellation, and rejected-last-writeback cases, then completed clean shutdown.
That last case covers a repaired request-lease leak in callback cancellation.
The older optional VM ownership suite remains disabled and was only compiled.

All 21 integration suites passed on fresh headless one- and four-CPU guests
(161.9 and 188.1 seconds); 106 affected source/object pairs were current. The new
guest contract checks memfd, RamFs, Ext2 hardlink aliases, private CoW, and seals.
Residency checks prove retention across shrink, not yet compactor inhibition.
An earlier four-CPU run failed the existing AF_UNIX interruption fixture with
child code 55 under concurrent compiler load. Its cause remains undetermined;
the passing final rerun is not an explanation. Disk writes stayed disabled and
the separately parked VM fault was not reopened. Logs, image identities, and
initial failures are preserved under
`/private/tmp/pedigree-file-resize-expansion-20260905/verification.json`.

## Pipe transfers

The pipe pass adds `splice`, `tee`, and copied `vmsplice`. Distinct pipes can
transfer bytes atomically as a pair; tee preserves the source. Splice also admits
regular-file input into a pipe, and pipe input into regular files or connected
stream sockets. Vmsplice copies between user vectors and a pipe in either
direction. Each successful batch is bounded by the pipe's 4 KiB capacity;
positive short results are permitted. No zero-copy or page-pinning guarantee is
introduced.

Known MOVE and MORE hints are accepted with copied transfers. GIFT is also
accepted: user-to-pipe vmsplice requires page-aligned nonempty vector bases and
lengths, and retains no caller pages. Other directions do not use that hint.
Unknown flags fail with EINVAL. The GIFT alignment restriction follows the
documented valid-input contract; Linux's current implementation also accepts
some unaligned inputs.

Pipe storage uses a VFS-local ring. Read reservations preserve an unconsumed
prefix, write reservations preserve space, and neither keeps a mutex across
guarded user copy or backend I/O. Cancel and partial completion restore readiness;
close and explicit reset cannot invalidate bytes already accepted by a sink.
Two-pipe waits observe both endpoints without retaining one reservation while
waiting for the other. Generic Buffer users remain independent of this change.

Unread FIFO data now survives a writer closing and reopening while a reader
remains. Data resets when the final open description leaves, matching the FIFO
lifetime. Successful direct transfers publish the destination's ordinary VFS
modify event after buffer locks are released.

The first guest run found that FIFO creation beneath a mount point inserted into
the covered directory, making the new path invisible. Creation now resolves the
mounted parent with a retained reference, checks parent access/read-only state,
and applies umask and effective ownership before publication. The failed one-
and four-CPU runs are retained with the explained repair.

`pipe-transfer-contract-test` covers splice, tee, vmsplice, lifetime, readiness,
and concurrency. All 20 integration suites passed on fresh headless one- and
four-CPU guests, with zero statuses and final markers. Native verification passed
69 buffer, readiness, VFS, metadata, and lock tests, including 13 new tests of the
actual ring engine. The 37 routing/ABI checks passed, eleven affected hosted units
compiled, and all 20 audited source/object pairs were current for the final images.

Guest streams use Unix sockets; TCP/IPv6 admission is compile-checked. A terminal
exit test proves cleanup of an admitted read reservation; the pair-wait exit test
has only public entry gating, so it cannot prove internal enrollment before exit.
Vmsplice faults commit successful iovec fragments, which differs from Linux's
pipe-buffer fault granularity. Full MAX_RW_COUNT movement and scratch-allocation
failure injection were not exercised. Disk writes remained disabled, and the
separately parked VM failure was not reopened. Logs, image identities, and limits
are recorded in
`/private/tmp/pedigree-pipe-transfer-expansion-20260905/verification.json`.

## File transfers

The transfer pass adds `sendfile` and `copy_file_range` using at most 64 KiB of
scratch storage. Both accept ordinary regular-file input and output; sendfile
also admits connected Unix, IPv4, and IPv6 stream output. Copy_file_range requires
the same filesystem instance. Pipes, datagrams, devices, and synthetic endpoints
remain outside this slice. Transfers copy bytes; they do not promise zero-copy
or preservation of sparse extents.

Explicit offsets leave their open description's position unchanged. Implicit
positions are serialized across dup aliases, and only accepted output advances
input. Copy_file_range rejects overlapping ranges of the same backing file.
Sendfile retains separate local positions even when both descriptions alias, so
its final shared position advances once. Backend size limits use Ext2's existing
size limit as their source of truth. Output writes
retain ordinary file seal checks.

Late offset-copy faults follow each syscall's completion order: EFAULT can be
returned after bytes have reached the destination. This does not roll back data
or already committed implicit positions. Positive progress otherwise takes
precedence over a later I/O error. Socket waits retain the original open
description across numeric close/reuse; SIGPIPE delivery occurs after transfer
locks are released.

Numeric close no longer takes an unrelated mqueue position lock for file-backed
descriptions. This allows a controller to close or replace a blocked transfer's
descriptor and then drain its original socket; the retained description keeps
the operation alive.

`transfer-contract-test` covers file copying, offsets, stream output, lifetime,
and concurrent operations. All 19 integration suites passed on fresh headless
one- and four-CPU guests, with individual zero statuses and final markers.
Native verification passed 38 lock-engine and VFS metadata tests; 37 routing/ABI
checks passed, and five affected hosted units compiled. All 14 audited affected
source/object pairs were current for the final images.

Stream guest coverage uses Unix sockets; TCP/IPv6 share the admitted backend but
were not executed here. Huge transfers and Ext2's size boundary have arithmetic
checks rather than guest data movement. Ordinary file copies have no deterministic
blocking backend for an EINTR fixture. A caught signal may allow further immediately
writable socket chunks before the next wait or completion. No sparse, snapshot,
or overlapping-sendfile guarantees are added. Disk writes remained disabled;
the separately parked VM failure was not reopened. Image identities, logs, and
limits are recorded in
`/private/tmp/pedigree-transfer-expansion-20260905/verification.json`.

## Anonymous memory files and seals

The memfd pass adds `memfd_create` and `fcntl(F_GET_SEALS/F_ADD_SEALS)`.
Files are anonymous, seekable, initially empty O_RDWR descriptions with mode
0777, effective creator credentials, and zero links. Empty labels and labels up
to 249 bytes are accepted; repeated labels create independent files. CLOEXEC
and ALLOW_SEALING are supported. Other creation flags, including huge pages,
return EINVAL.

All five seals in the bundled musl are supported: SEAL, SHRINK, GROW, WRITE,
and FUTURE_WRITE. Seal state follows the backing file through dup, fork, exec,
descriptor passing, and mappings that outlive their last descriptor. Failed
seal additions publish none of the requested bits. Write and resize restrictions
are checked before changing data or retiring mapping loans.

WRITE admission rejects shared mappings that retain write capability, including
unfaulted, read-only, PROT_NONE, and child mappings. FUTURE_WRITE preserves that
capability in existing mappings; new shared read-only mappings cannot later gain
write access. Private writable mappings retain copy-on-write behavior. Splitting,
moving, discarding resident pages, and changing current protections preserve
the admitted capability; final unmap or teardown releases it. A policy-denied
MAP_FIXED leaves its victim intact.

The new backing and seal state remain POSIX-local. Generic VFS admission hooks
coordinate with the existing VM operation gate. There is no general writable
page-pin API in the current memfd call graph; future vmsplice/GUP/DMA borrowers
must participate in seal admission. Existing SharedPointer control-block and
container allocation failures are not fully recoverable. A read-only memfd OFD
cannot yet be obtained through `/proc/self/fd` reopening, so that access-mode
test remains unavailable.

`memfd-contract-test` covers creation, seals, mappings, lifetime, and synchronized
write/mmap races. All 18 integration suites passed on fresh headless one- and
four-CPU guests, including per-suite zero exit statuses and final markers.
Native verification passed 126 String, lock-engine, and VFS metadata tests;
37 routing/ABI checks passed, and four affected hosted units compiled.

The first guest run exposed a shared String bug: appending an empty String copied
a terminator from null storage. Empty append is now a no-op, with four native
regressions. Both initial failures are retained alongside the passing runs.
Image identities, logs, checks, and limits are recorded in
`/private/tmp/pedigree-memfd-expansion-20260905/verification.json`. Disk writes
remained disabled; the separately parked VM failure was not reopened.

## Advisory file locks

The file-lock pass adds `flock` and the classic/OFD record-lock commands of
`fcntl`: GETLK, SETLK, and SETLKW. Locks are advisory; ordinary I/O remains
permitted. Classic process locks and OFD record locks conflict with one another;
local `flock` uses a separate namespace.

Process record locks are shared by threads, absent in a fork child, retained by
exec, and removed when their process closes any descriptor for the inode. OFD
and flock locks follow the shared open description through dup, fork, and
SCM_RIGHTS, ending at its final lifetime release. A blocked classic acquisition
rechecks the numeric descriptor's open-description identity before granting a
lock, so concurrent close/reuse cannot leave a grant on the retired file.

Ranges support shared/exclusive modes, partial unlock, conversion, signed
SEEK_SET/CUR/END normalization, and zero length through future EOF. Capacity
failure preserves the original range set. Blocking classic requests detect
cycles across process owners and inodes; OFD and flock waits do not promise
deadlock detection. Waits are interruptible, and F_SETLKW follows musl's
cancellation-point path.

The registry is POSIX-local, with at most 4,096 granted intervals and 256 blocked
requests. Exhaustion returns ENOLCK. Its first admitted descriptor class is
ordinary regular files; unsupported classes fail explicitly. Waiters use
broadcast/recheck, without a FIFO fairness guarantee.

RamFs file and directory creation now applies the supplied mode and effective
creator UID/GID before namespace publication. Constructor defaults for synthetic
kernel nodes remain separate. Special set-ID inheritance is existing metadata
work outside this slice.

`file-lock-contract-test` covers flock, records, lifetime, blocking, and creation
families. Native verification passed eight shared-engine cases, including 1,000
transactions compared with an independent byte model, plus 30 VFS/file-metadata
regressions. All 17 integration suites passed on fresh headless one- and
four-CPU guests, with individual zero exit statuses and final markers. The
37 routing/ABI checks passed, and six affected hosted translation units compiled.

The first guest fixtures assumed an unavailable `close_range` route; that
prerequisite was removed, while close, dup2, CLOEXEC, fork, exec, and descriptor
passing remain covered. The creation check then caught a Unix-to-VFS permission
encoding error in the new RamFs helper; its conversion is corrected. Failure logs
are retained. Numeric-close waiter enrollment uses a bounded timing check because
public wrappers do not expose kernel enrollment. Global capacity exhaustion has
native reduced-capacity coverage.

Artifacts and exact image/check identities are under
`/private/tmp/pedigree-file-lock-expansion-20260905/verification.json`. Disk writes
remained disabled; the hardlink/rename tests do not claim persistence coverage.

## Remapping, residency and page discard

The VM pass adds `mremap`, `mincore`, and `madvise(MADV_DONTNEED)`.
Remapping supports shrinking, in-place growth, MAYMOVE relocation, and FIXED
replacement for anonymous memory and ordinary shared/private file mappings.
Source subranges preserve their surrounding mappings, backing offsets, protection
limits, and copy-on-write behavior. Whole contiguous System V attachments can
move or shrink without changing attachment counts.

Preparation allocates replacement metadata, address reservations, page tables,
and a retirement journal before mutation. Commit checks the reservation epoch
and transfers the current PTEs under the address-space mutation lock; detached
references are retired afterwards. Process reservation access now uses serialized
operations instead of exposing its allocators. Fallible container operations and
checked nullable allocation support let preparation report exhaustion without
partially changing mappings.

`mincore` reports residency without faulting pages in, includes cached file pages
and present PROT_NONE mappings, and masks file information for callers who neither
own nor may write the file. Copyout uses bounded kernel snapshots. DONTNEED drops
private changes while retaining the mapping and permissions; shared file and SHM
contents remain in their backing storage.

The initial contract has explicit limits:

- Remapping operates within one managed source object. Zero-size shared mapping
  duplication, DONTUNMAP, direct physical mappings, and unmanaged initial
  address-space mappings are unsupported. SHM growth and fragmented or partial
  source attachments are unsupported.
- Preparation admits at most 65,536 aggregate pages across the new extent and
  complete affected source/victim objects, 4,096 existing objects, and 32
  reservation retries. Capacity exhaustion returns ENOMEM.
- DONTNEED is the only accepted advice. Other advice values return EINVAL.
- Production PTE transfer is implemented for amd64. Hosted builds provide compile
  coverage; other architectures do not gain a remap implementation from this pass.
- Ordinary allocator free-list growth retains the pre-existing fatal allocation
  failure limitation. The remap transaction prepares its release metadata before
  commit. Existing fork/TLS snapshot coherence and the separately parked VM fault
  are outside this pass.

`vm-contract-test` contains bounded remap, residency, and discard families.
The default-off `PEDIGREE_VM_REMAP_TESTS` fixture exercises preparation allocation
failure, stale reservations, a CoW change after preparation, protection transfer,
and exact displaced-page retirement. A standalone native fixture injects actual
allocation failure into the shared metadata containers.

Native verification passed 119 affected utility cases and 38 routing/ABI/fallible
allocation checks. Six affected hosted translation units compiled; the scheduler
regression unit passed C++ syntax checking only because its existing ELF assembly
is not accepted by the Darwin assembler. The remap kernel fixture passed with one
and four CPUs; it is disabled in the ordinary image configuration.

Initial failures are retained under `/private/tmp/pedigree-vm-expansion-20260905/`.
The new CoW fixture undercounted its retained physical-page references and was
corrected. The residency fixture now establishes file mode explicitly because
RamFs creation ignored the supplied mode at that checkpoint; the file-lock pass
above repairs creation metadata.
An existing clock fixture could consume its deadline while creating workers; it
now gates startup and checks the observation window, with failure diagnostics.
The original clock failure lacked timestamps sufficient to prove its exact cause.
Final fresh headless one- and four-CPU ordinary images passed all three VM
families and all 16 integration suites, including per-suite zero exit statuses
and the final marker. `verification.json` in that artifact directory records the
image identity, logs, and checks. Disk writes remained disabled.

## Signal and timer descriptors

This pass adds `signalfd`, `signalfd4`, `timerfd_create`, `timerfd_settime`, and
`timerfd_gettime`. The guest application is `event-descriptor-contract-test`,
with optional `signalfd`, `timerfd`, and `clock` family arguments.

Signal descriptors consume the caller's pending signals without changing its
signal mask. Standard signals coalesce; realtime signals retain ordered values.
They share masks across dup, fork, and SCM_RIGHTS. Reads report complete 128-byte
records; failed scalar or vectored copies leave the failing record pending.
SIGCHLD exit records include trusted child identity and status, and timer signals
carry timer identity, payload, and overrun. Legacy child stop/continue delivery
still lacks typed status metadata; fault-specific siginfo fields are not populated.

Poll observes its calling thread. Epoll retains registration with the process's
signal context, samples the waiting thread's private queue, and survives the
registering thread's exit. An inherited epoll registration does not observe the
child's new signal context; the child must create its own registration.

Timer descriptors support CLOCK_REALTIME and CLOCK_MONOTONIC, relative/absolute
deadlines, periodic expiration counts, and realtime CANCEL_ON_SET. Realtime
changes wake absolute timers immediately; relative deadlines remain monotonic.
Expiration counts and settings belong to the shared open description and survive
fork, descriptor passing, and exec unless the descriptor has CLOEXEC. A failed
readv copy preserves the counter. Disarming retains the configured interval.
There are at most 256 live timer descriptions (ENFILE on exhaustion); alarm and
CPU clock variants are unsupported.

Both descriptor types support ordinary fd flags, FIONBIO, anonymous-inode fstat
metadata, noop lseek, and `/proc/self/fd` readlink names. Closing the final
descriptor wakes a blocked reader with EBADF. This explicit local close contract
differs from Linux's ability to retain an in-flight read after the numeric fd is
closed.

Verification: 37 native routing/ABI checks passed, and the sleep/clock and
thread/signal hosted regression units compiled against the final headers. Fresh
headless one-CPU and four-CPU guests passed all three new families plus 14 existing
suites, with individual exit statuses and final markers. Artifacts are under
`/private/tmp/pedigree-eventfd-expansion-20260905/`; `verification.json` records the
ISO identity and final logs. Initial guest failures exposed missing EBADF and
partial-read errno clearing; those failures and their repairs are retained.
The descriptor-passing fixtures use Unix streams because datagram socketpair
remains an existing networking limitation.

## IPC pass

Implemented and guest-verified: System V shared memory, semaphore sets, message
queues, and POSIX message queues (18 numbers; 95 backlog items remain). Coverage includes creation/lookup permissions,
fork/exit lifetime, removal while referenced or blocked, bounded resources,
interruptible waits, ABI marshalling, and message selection/priority semantics.
POSIX queue notification includes musl's SIGEV_THREAD cookie transport.

The guest application is `ipc-contract-test`; optional arguments select
`messages`, `semaphores`, `mqueues`, or `shared-memory`. Each family runs in a
bounded child and reports its own result. The integration run also exercises the
existing signal/exec, user-copy, futex, descriptor-passing, readiness, and resource
contracts.

The implementation uses finite resources and reports exhaustion. Defaults and
limits are exposed through the corresponding IPC control queries where the ABI
provides them:

- SysV messages: 128 queues; 8192-byte messages; 16384-byte default queue storage,
  configurable up to 65536 bytes with privilege for increases.
- SysV semaphores: 128 sets, 256 semaphores per set, 128 operations per atomic
  vector, and 4096 undo records. Waiters use broadcast/recheck; strict FIFO
  handoff is not promised.
- SysV shared memory: 128 segments, 16 MiB per segment, 64 MiB total, and 128
  explicit attachments per process. HugeTLB is unsupported. SHM_LOCK pins pages;
  per-user RLIMIT_MEMLOCK accounting remains part of the resource/VM expansion.
- POSIX queues: 64 live queues, up to 128 messages of 8192 bytes per queue. The
  AF_NETLINK endpoint implements the notification cookie transport required by
  musl; general routing netlink remains outside this IPC interface.

The IPC checkpoint originally limited signals to 1–34. The signal/timer pass
below extends delivery through 64, including POSIX queue notifications.

Verification: the native routing/ABI checks passed 42 tests. Fresh, display-free
QEMU guests with one and four CPUs passed all four IPC families plus 12 existing
regression suites, including per-suite zero exit statuses and the final marker.
Logs and the disposable-image harness are under
`/private/tmp/pedigree-musl-expansion-20260905/` (`ipc-runtime-*`). This is guest
behavioral evidence; no persistence claim is made with disk writes disabled.

Concurrent fork/exit exposed an unrelated procfs shadow-tree race. Removing the
unused PID index lets the existing VFS directory synchronization own publication;
inode allocation is now atomic. The original failure and the subsequent passing
run are retained alongside the IPC notification cleanup regression evidence.

The existing intermittent VM split/unmap investigation remains recorded
separately. Reusing the established shared-file mapping path does not establish
that the earlier fault is resolved.

## Signals, timers and realtime clock

This pass adds `rt_sigpending`, `rt_sigtimedwait`, `rt_sigqueueinfo`, the five
POSIX timer calls, and `clock_settime` (nine numbers). Signal queues retain
standard-signal coalescing and realtime FIFO order, support synchronous waiters
in sibling threads, and preserve a delivery after failed result copyout.
Blocked ignored signals remain available to synchronous consumers. Signal
disposition changes rebind every pending instance without losing its metadata.

Timers support realtime and monotonic clocks, relative and absolute deadlines,
periodic overruns, SIGEV_NONE, SIGEV_SIGNAL and thread-directed delivery,
including musl's SIGEV_THREAD helper. Rearm, delete, target exit and successful
exec invalidate the appropriate queued notification generation. Timers are not
inherited across fork; IDs reject stale and foreign-process references.

Setting realtime updates kernel and vDSO time and wakes absolute realtime sleeps
and POSIX queue waits. Relative waits and monotonic time remain unaffected.
The timer service reevaluates absolute realtime deadlines on its next tick.
The final signed-nanosecond second is converted without premature saturation.

Current bounds and deliberate restrictions:

- Pending queued signals and timer reservations share a fixed limit of 16 per
  real UID. Mutable RLIMIT_SIGPENDING remains in the resource pass. Realtime
  `kill`/`tkill`/`tgkill` report EAGAIN on exhaustion; Linux can instead retain a
  signal without allocating its queued metadata.
- User `rt_sigqueueinfo` accepts SI_QUEUE; kernel timer and queue-notification
  metadata is constructed internally. Existing process-directed rejection of
  musl-private signals 32–34 remains; private thread-directed protocols work.
- Timers have 256 global slots and 64 per-process slots, further constrained by
  signal reservations. CPU-time and alarm/boottime clock variants are unsupported.
- Only privileged realtime clock setting is supported; hardware RTC persistence
  is not implied.

`signal-timer-contract-test` runs bounded `signals`, `timers` and `clock`
families. Native timer arithmetic has six passing cases; 37 routing/source ABI
checks pass. Two affected hosted regression translation units compile. Fresh
one- and four-CPU guests passed all 14 integration suites after the once-only
subsystem cleanup repair, including each suite's zero status and final marker.
Logs are `signal-timer-exit-fixed-*` in the artifact directory below; image
identity and results are recorded in `verification.json`. The backlog now has
27 implemented numbers and 86 remaining. Disk writes remained disabled.

The first one-CPU integration run passed all new families, then failed during
IPC mqueue process exit with a retained spinlock. Review found that final
shutdown repeated blocking subsystem cleanup under the process spinlock;
thread cleanup now has one admission per lifetime. A diagnostic retry did not
reproduce the fault, so its exact retained-lock identity was not captured.
The original failure and diagnostic run remain under
`/private/tmp/pedigree-signal-timer-expansion-20260905/`; the lock-checker message
now includes the retained lock and acquisition caller for future attribution.
