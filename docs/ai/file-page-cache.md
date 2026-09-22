# File page ownership and writeback

Regular-file data has one authoritative native-page cache. Ext2 inode aliases
share an inode-owned `File::CacheState`; FAT and ISO use their File-owned state.
`read`, `write`, and shared mappings refer to those same pages. Private writable
mappings still use copy-on-write pages and never dirty the backing file.

`File::readPage` fills a caller-owned page, including holes and the partial EOF
tail. Ext2 and ISO can read file extents through `Disk::readInto` without first
allocating a second block-cache page. Whole-page ordinary writes can initialise
their destination without reading the old payload. Partial writes retain the
read/modify/write path.

## Dirty state

Canonical file caches select `Cache::DirtyTracking::Explicit` before inserting
pages. Ordinary writes and changes to resident EOF padding call `markDirty`.
The cache records mutation and completed-write generations separately from
actual writeback errors. A callback can complete an older generation while a
concurrent writer leaves the page dirty for its next writeback.

Clean explicit pages are neither hashed nor included in periodic writeback
scans. The candidate index contains dirty pages, pages being published, and
pages exposed to external writers. A clean sync avoids scheduling a worker.

Before a file page is exposed through a writable shared mapping,
`File::markPageExternallyWritable` enables checksum detection for that page.
The marker remains until eviction; unmapping does not attempt to reconstruct
alias counts or reset the baseline. Read-only and private mappings do not enable
this fallback. Permission changes and kernel copies into shared user mappings
follow the same exposure rule.

Checksums remain the default for legacy raw-pointer cache producers, including
disk metadata. Those producers must not select explicit tracking until every
mutation and writable mapping exposure is accounted for.

## Transport and durability

`Disk::readInto` and `Disk::writeFrom` borrow caller-provided buffers for the
duration of a synchronous transfer. `writeFrom` completes the transfer;
`syncData` provides the device durability barrier. The default adapter uses
legacy bounded views and already-durable writes. AHCI implements the direct
buffer path and keeps its existing DMA bounce buffers.

A direct transfer must preserve existing disk-cache aliases. The SCSI/AHCI
adapter serialises overlapping ranges. Absent, native-sector-aligned data goes
directly to the supplied buffer. An existing cached page is read or merged in
place, and partial native sectors use read/modify/write. It never waits for
long-lived metadata loans to disappear. When joining an active cache callback,
it drops range admission and rechecks cache identity after reacquiring it.
Disks with any partition cache origin that is not native-page-aligned retain
the legacy buffered path for all producer transfers. This avoids bypassing a
shifted cache alias; the legacy overlapping-origin policy itself is unchanged.

Synchronous mapping sync submits bounded batches of resident shared pages.
Ext2 transfers each batch and performs one final barrier. Failed transfers or
barriers leave every submitted upper page retryable. Clean pages can be omitted
from a batch; cache users must not assume the callback count equals the request
count or that a cache callback alone supplies a hardware barrier.

## Metadata and remaining adapters

Ext2 loads only the inode-table block containing the requested inode. Sparse
table slots are retryable, and loaded inode addresses remain pinned and stable.
Namespace durability and destruction visit loaded slots only. Indirect block
accounting and metadata ordering are unchanged.

FAT now shares the common page owner and dirty policy, but its existing cluster
assembly and disk-cache adapters remain. Newly allocated ext2 blocks also keep
their existing zero-initialisation path: an experiment writing zeros directly
removed a cache layer but added immediate I/O and regressed the write benchmark.
ISO rejects ordinary writes and writable shared mappings.

RAM filesystem pages are the backing storage itself; reclaiming them as clean
file-cache pages would lose data. Raw device views retain their existing cache
and alias policy. Neither is silently converted into a second regular-file
cache. This change does not replace the block request scheduler or add speculative
read-ahead.
