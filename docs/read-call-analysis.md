# Read request sizes during startup

Measured September 9, 2026, against the completed page-cache/storage changes.
The measured startup and version-command launches did not issue regular-file
reads larger than 4 KiB. Supporting asynchronous submission only for larger
`File::read()` calls would therefore leave these workloads on the single-page
path. Startup needs a bounded fault-clustering or read-ahead policy to create
multiple useful pending page fills.

## Measured calls

| Workload | Regular-file `File::read()` calls | Calls larger than 4 KiB | Mmap backing-read calls | Mmap share of EOF-clipped requested bytes |
|---|---:|---:|---:|---:|
| Cold `git --version` | 188 | 0 | 179 | 99.76% |
| Cold `nano --version` | 129 | 0 | 121 | 99.47% |
| Init invocation to first username prompt, run 1 | 420 | 0 | 313 | 97.97% |
| Init invocation to first username prompt, run 2 | 420 | 0 | 313 | 97.97% |

The two complete startup histograms matched. All recorded mmap backing-read
requests were exactly 4,096 bytes. These counts include backing reads that find
an already resident file page, such as private-copy faults; they are not counts
of physical disk requests or every page fault.

Git made three ordinary regular-file `read()` syscalls requesting 3,008 bytes in
total, each no larger than 1 KiB. Nano made two, totaling 1,920 bytes. Normal
startup made 39, totaling 29,537 bytes; none exceeded 4 KiB. No regular-file
`pread`, `readv`, or `preadv` calls occurred in those measured intervals.

Cold Git performed 180 canonical page fills and 195 AHCI reads; Nano performed
124 fills and 138 AHCI reads. The additional transport work includes filesystem
metadata. Warm Git repeated the same small ordinary reads and seven mmap backing
reads, but performed zero canonical fills and zero AHCI reads. Caller activity
and storage misses must remain separate measurements.

## Where the sizes originate

Normal execution uses `PosixSubsystem::prepareExecutable/loadElf`. Its eager
file reads inspect the shebang, ELF header, program-header table and interpreter
path. The measured main executables have six program headers, totaling 336 bytes.
PT_LOAD segments use file mappings with `MemoryLockMode::None`; executable and
library contents are subsequently demand-paged. The optional kernel
`DynamicLinker::loadProgram` is a different route and is not the main/interpreter
loader for these programs.

`MemoryMappedFile::getBackingPage` requests at most one native page. Its private
copy path also bounds backing reads to one page. Thus the kernel's mapping layer
supplies most of the observed small requests; increasing application stdio buffer
sizes would not change that part of startup.

Scalar regular-file `read` and `pread` already allow up to 64 KiB per backend
call. Larger requests are divided into those pieces. Allocation failure or
user-buffer validation can reduce a piece to the 4,097-byte fallback. Vector
reads currently divide each iovec into pieces no larger than 4,097 bytes
(`PIPE_BUF_MAX + 1`), without combining neighbouring iovecs.

Once a request reaches `File::read`, pages are filled synchronously one at a
time. SCSI further splits buffer transfers at disk-cache page boundaries, and
AHCI rejects a producer transfer larger than one native page. Both caller demand
and lower-layer submission need to support a batch before it can overlap reads.

Source entry points:

- [Scalar and vector read adapters](../src/modules/subsys/posix/file-syscalls.cc):
  `allocateScalarReadBounce`, `posix_read`, `posix_pread64`, `posix_readv`,
  `positionalReadVector`.
- [Executable loader](../src/modules/subsys/posix/PosixSubsystem.cc): `loadElf`,
  `prepareExecutable`.
- [File reads and fills](../src/modules/system/vfs/File.cc): `read`, `readIntoCache`.
- [Mapped backing reads](../src/modules/system/vfs/MemoryMappedFile.cc):
  `getBackingPage` and private-copy handling in `trap`.
- [SCSI splitting](../src/modules/drivers/common/scsi/ScsiDisk.cc): `transferBufferRange`.
- [AHCI size admission](../src/modules/drivers/common/ahci/AhciDisk.cc): `transferBuffer`.

## Positive control

A separate static program successfully read and checked the same 256 KiB fixture
range through each of `read`, `pread`, `readv`, and `preadv`. Each vector call
contained two 128 KiB iovecs. The counters recorded all four 256 KiB syscall
requests, confirming that the measurement did not discard larger sizes.

The scalar calls produced eight 64 KiB backend reads in total: four per syscall.
The vector calls produced 124 pieces of 4,097 bytes and four tails of 4,065 bytes.
All returned bytes matched the fixture. The control still reached at most NCQ
depth one, confirming the existing serialization even when demand is larger.

This establishes a real opportunity for bulk-read batching, but the startup
results do not exercise that opportunity. The vector adapter's splitting is a
separate bulk-I/O issue; it did not account for these startup delays.

## Method and limits

Temporary relaxed atomic counters were added to an isolated checkout. Separate
histograms recorded original regular-file syscall lengths, `File::read` lengths
with and without copying, EOF-clipped lengths, mmap backing reads, and canonical
page fills. Ordinary bytewise devices, pipes and sockets were excluded from
regular-file syscall counts. Requested bytes are not returned bytes, unique file
bytes, or physical disk traffic.

Launch phases used the gated static harness in `scripts/benchmarks`. QMP paused
the guest outside measured work to read the counter arrays. Normal startup used
a kernel checkpoint immediately before invoking init, with an endpoint at the
first username prompt. This includes startup services until that endpoint and
excludes earlier kernel/module boot work. QEMU had four Sandy Bridge vCPUs,
4 GiB RAM and no NIC; kernel serial logging was disabled to avoid UART record
interleaving. Disk writes remained enabled on disposable overlays.

The source baseline was `94b99d465` in the proven-console checkout. Its completed
storage changes are integrated in main at `11e611773`; the launch harness is
`c30184d39`. This is the same proven-console image family used for the preceding
benchmarks, not the separate graphical-console work. Instrumentation changes
execution cost, so these runs establish call shapes, not speedups. They do not
cover Git repository operations, editing a large file in Nano, or every workload
on physical hardware.

Five guests passed, both startup histograms matched, the large-read control
passed, histogram bounds and stage accounting checked, and launch transport
counts matched QMP. The diagnostic source changes were then removed and the
ordinary initrd rebuilt. No production performance code changed in this pass.
The initial diagnostic compile failed on a protected predicate; using the public
`supportsRegularFileOperations()` predicate corrected it before all guest runs.

The retained artifact directory is `/private/tmp/pedigree-read-call-profile`:
`comparison.json`, per-run reports and logs, `diagnostic.patch`, exact source
baselines, `profile_support.py`, launch/boot runners, the large-read control,
`profile-initrd.tar`, provenance and disposable images. Kernel and original
release images were not replaced.

## Next experiment

Measure adjacent fault-offset reuse, then try a small bounded cluster of file
pages within the mapping and EOF. Submit those fills through an asynchronous
request window, retain page ownership until completion, and let the demanded
page's completion satisfy the fault. Track useful prefetched pages, extra bytes,
cold launch time and warm regressions. Increasing backend concurrency alone
cannot create neighbouring demand that the mapping layer never submits.
