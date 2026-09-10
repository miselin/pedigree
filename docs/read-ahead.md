# Batched reads and fault read-ahead

File cache misses can now submit several page reads before waiting for I/O.
The executable loaders keep their file mappings; the mapping fault supplies
bounded neighbouring demand that the previous single-page path lacked.

## Policy and ownership

- A mapped read fault starts with four pages. Reaching the end of the preceding
  populated window doubles it, up to 32 pages. A jump restarts at four pages.
  Mapping bounds and EOF limit every window. Private-copy faults remain local.
- `File::read()` batches misses within the requested range, up to 32 pages at a
  time. Cache hits retain the existing single-lookup path.
- Regular-file `readv` and `preadv` use the same 64 KiB bounce-buffer policy as
  scalar reads. They preserve iovec boundaries and partial-copy/offset semantics.
  Pipes retain their existing policy; allocation or valid-prefix fallback can
  still select the smaller 4,097-byte buffer.

`File::populateRange()` reserves and pins canonical file pages through transport.
Ext2 resolves their block mappings first, leaves holes and EOF padding zero, and
submits bounded `Disk::ReadBuffer` arrays through Partition and SCSI to AHCI.
Each page becomes visible only after all its constituent reads succeed. Failed
speculation does not invalidate an already completed demanded page.

SCSI retains disk/controller leases and cache admission until every transfer is
drained. Resident disk-cache aliases are copied under a pin, preserving dirty
data coherence. Non-batched transports retain the sequential fallback.

AHCI admits as many commands as available slots allow before reaping the wave.
An initially full queue waits for existing owners; a batch already owning tags
reaps them before trying to admit more. Flush admission uses the same gate, and
failure drains every owned tag after stopping DMA. The caller waits for the
whole submitted wave. This is concurrent transport with synchronous completion,
not a background read-ahead worker. Neighbouring pages are cached; only the
demanded page receives a mapping during that fault.

## Completion waits during faults

Early four-page runs exposed an intermittent AHCI stall. A captured Git thread
was asleep in `reapCommand()` with event deferral depth three and its timeout
event already pending. The hardware requests had completed. Mapped-fault event
deferral prevented the timeout callback from waking the semaphore waiter.

AHCI now checks completion before sleeping. Deferred-event callers poll and
yield without allocating timeout events, including queue admission and engine
shutdown waits. Existing issue and flush deadlines remain in effect. Native
tests model missing interrupts and a completion racing interrupt acknowledgement;
the exact initiating interrupt race in the captured guest is not established.

## QEMU comparison

September 9, 2026: QEMU 11.0.3, TCG, four Sandy Bridge vCPUs, 4 GiB RAM, AHCI,
no NIC, writes enabled, identical quiet serial settings and disposable disks.
These are guest-cold comparisons, not cold host-cache or physical SSD timings.
An unrelated background guest remained running on the host. Builds were excluded
from the measured intervals.

| Workload | Baseline | Candidate | Disk bytes, baseline → candidate | Maximum NCQ, baseline → candidate |
|---|---:|---:|---:|---:|
| Git first output | 367.6 ms | 288.4 ms median | 798,720 → 1,216,512 | 1 → 4 |
| 8 MiB, 64 KiB `pread` calls | 1,164.5 ms | 545.2 ms | 8 MiB → 8 MiB | 1 → 13 |
| 8 MiB mapping, sequential first touches | 1,337.3 ms | 568.2 ms | 8 MiB → 8 MiB | 1 → 13 |
| 8 MiB mapping, permuted first touches | 2,016.4 ms | 699.1 ms | 8 MiB → 8 MiB | 1 → 4 |

Git candidate cold samples were 288.4, 295.2 and 280.0 ms. The baseline and
each other table row have one cold sample; treat their deltas as experimental
results, not statistical confidence bounds. Warm bulk reads were 186.6/186.8 ms
before and 185.0/176.7 ms after, with zero disk reads in both cases. Warm mapped
touch loops fell below the guest clock's reported resolution. All fixture bytes
were checked outside the timed interval.

The mapping benchmark visits every page eventually. Its unchanged byte count
does not establish low cache pollution for sparse accesses to much larger files.
Git demonstrates that tradeoff: the selected policy reads about 52% more bytes
while reducing first-output latency by about 22%. An initial fixed 16-page
window read 2.8 times the baseline bytes without improving Git launch time.
An initial bulk implementation also added warm cache lookups; removing those
eliminated its observed warm-read regression.

The queue depth limit remains 32. QEMU completed some commands while later ones
were being issued, so measured hardware overlap peaked at 13. One-CPU Git and
bulk checks also passed. Later normal-init regression boots observed init to
username prompt at 1.03/1.15 seconds on one CPU and 1.54/1.61 seconds on four.
Those coarse host observations include serial logging and have no matching
baseline in this pass; they are not an attributed speedup or a T420 prediction.

## Validation

The normal `cmake --build build` completed with disk writes enabled. All 107
focused native cache/File/Ext2/Disk/Partition tests passed, as did the two AHCI
ASan/UBSan fixtures and 14 benchmark tests. The AHCI fixtures cover saturated
queues, partial admission, failures, deferred-event completion and DMA-stop
requirements. They use simulated MMIO/admission, not a physical controller.
Darwin hosted core/SCSI tests passed through clean shutdown.

All 14 final QEMU guests passed: nine launch/read experiments plus a large
scalar/vector control and mapped-write/contract suites on one and four CPUs.
Offline `e2fsck -fn` and exact-byte verification passed for all four persistence
overlays. The contracts include private-copy protection, regular and mapped
user buffers, and filesystem mutation persistence.

The new hosted vector fixture counts backend calls and covers short/error
semantics, but its static POSIX runtime lane was unavailable on this macOS host.
It was syntax-checked only. Guest vector validation establishes correct returned
bytes; the 64 KiB splitting policy is additionally verified from the source.
Physical hardware and simultaneous hardware flush/queue saturation remain
outside this pass.

The baseline proven-console source was `94b99d465`; the main checkout started
this pass at `749d78c27`. Runtime images use that isolated console configuration
with the new storage sources. The normal main build retains its graphics
configuration. Exact source patches, frozen binaries, images, reports, failed
run captures and validation logs are under `/private/tmp/pedigree-read-ahead`.
