# NVMe storage

The `nvme` module supports PCI NVMe 1.1 and newer controllers exposing the NVM
command set, a 4 KiB minimum memory page size, and legacy INTx. The controller
must be in D0 with MSI/MSI-X disabled. PCI BAR0 may be 32 or 64 bits. Active
namespace discovery follows namespace lists, including sparse IDs and list
continuations. Namespaces using 512-byte or 4096-byte logical blocks are exposed
as ordinary disks. Metadata, protection information, shared namespaces, extended
LBA formats, and other I/O command sets are rejected.

One admin queue and one I/O queue use up to 16 entries each, bounded by CAP.MQES,
with at most 15 commands outstanding. Distinct filesystem cache fills can run
concurrently; fills overlapping the same cache page remain serialized by the
SCSI cache admission layer. Writes and flushes retain the SCSI request worker's
ordering. Transport writes also share a lock with flushes, so a successful flush
covers all writes that completed before its submission.

Transfers use persistent, coherent DMA buffers below 4 GiB. Aligned PRP1, direct
PRP2 for two pages, and PRP lists for larger requests cover up to 64 KiB, further
bounded by the controller's MDTS. Completion processing validates queue IDs,
command IDs, submission heads, phase changes and status. Threaded INTx completes
requests; a bounded polling fallback handles lost interrupts. Read/write and
admin commands time out after 30 seconds; flushes allow 120 seconds.

A transport timeout or invalid completion takes both queues offline. DMA pages
remain allocated until clearing CC.EN is acknowledged by CSTS.RDY clearing. If
that cannot be established, the kernel stops rather than reusing memory still
visible to the device. Ordinary command errors fail that request without treating
an error completion as a controller timeout. Module retirement closes disk
admission and drains caches and callers before removing interrupt handlers and
DMA storage. Orderly teardown sends a normal shutdown notification and waits up
to 30 seconds for completion before disabling the controller.

The initial implementation does not include MSI-X, per-CPU queues, zero-copy
DMA, request merging, discard, namespace hotplug, controller recovery after a
transport failure, power management, or SMART/log-page interfaces. Concurrent
read fills exercise hardware queuing, but the page cache, copies, and serialized
write worker limit throughput. Real SSD performance needs hardware measurement;
QEMU establishes behavior only.

## Disposable namespace smoke test

Configure `PEDIGREE_NVME_SMOKE_TESTS=ON`, `PEDIGREE_CRIPPLE_HDD=OFF`, and
`PEDIGREE_WITH_INIT=OFF` in a dedicated test build. Use a fresh UEFI image and disposable
disks. [Modern storage validation](modern-storage.md) provides the harness. The smoke test excludes the physical root disk and selects 32 MiB NVMe
namespaces. It requires an exact 4096-byte fixture header before making writes:

| Offset | Encoding | Value |
| --- | --- | --- |
| 0 | ASCII without terminator | `PEDIGREE-NVME-SMOKE-v1` |
| 32 | byte | 1 |
| 40 | little-endian uint64 | 33554432 |
| 48 | little-endian uint32 | logical block size, 512 or 4096 |
| all other header bytes | byte | 0 |

Fill bytes after the header with
`((offset * 37) ^ (offset >> 8) ^ (offset >> 16) ^ 0x5a) & 255`.
Use multiple namespaces with noncontiguous IDs, such as 1 and 7, to test
discovery. The test reads through the PRP list, checks rejected ranges and a
1024-byte offset within a native block, and starts 20 workers with both shared
and distinct cache-page reads. It requires more than one device command in
flight and at least one interrupt completion. It writes and flushes these ranges
using the same pattern with seed `0xa5`:

| Byte offset | Length |
| --- | --- |
| 8388608 | controller maximum transfer, normally 65536 |
| 16777216 | 4096 |
| 33554432 minus logical block size | logical block size |

It rereads raw transfers and retires the written cache page before its reread.
The final marker is `NVME-SMOKE: PASS complete namespaces=N`. Verify those ranges
in the host disk image after QEMU exits to establish media persistence; guest
rereads alone do not prove host backing-file durability. Preserve all failure
logs. This suite covers valid I/O and range rejection; injected controller
timeouts, malformed hardware completions, and physical-device reset behavior
need separate qualification.

## Specification references

Register, queue, command, namespace and PRP definitions follow the
[NVM Express 1.4c specification](https://www.nvmexpress.org/wp-content/uploads/NVM-Express-1_4c-2021.06.28-Ratified.pdf),
particularly sections 3, 4, 5.3–5.4, 5.15, 6.8, 6.9, 6.15 and 7.
Current specifications are published by
[NVM Express](https://nvmexpress.org/specifications/).
