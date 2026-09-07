# AHCI storage

The `ahci` module supports PCI SATA controllers reporting class/subclass/programming
interface `01:06:01`. It is built for x86-64 PCI targets, including the Intel
controller family in the ThinkPad T420. The controller must be configured for
AHCI in firmware; IDE and RAID modes are not supported by this module.

This initial implementation supports directly attached SATA disks with DMA,
48-bit LBA addressing, 512-byte logical sectors, and cache-flush support. It uses
Pedigree's existing disk cache, request queue and partition/filesystem layers.
Partition discovery waits for both ATA and AHCI probing, allowing the root disk
to be attached through either transport.

## Implemented behavior

- BIOS/OS ownership handoff, HBA reset, port initialization and IDENTIFY.
- READ DMA EXT and WRITE DMA EXT through persistent DMA buffers below 4 GiB.
- Threaded shared INTx completion, with a timed polling fallback.
- Checked byte counts, device errors and command deadlines; a failed command
  takes its port offline. The driver stops DMA before releasing its buffers.
  An engine that cannot be stopped causes a kernel panic rather than unsafe
  reuse of memory still owned by hardware.
- FLUSH CACHE EXT or FLUSH CACHE for checked disk synchronization, including a
  final device flush during controller shutdown. Ordinary commands have a
  30-second deadline; flush commands have a 120-second deadline.
- Cache publication, pin ownership and unload admission follow `ScsiDisk` and
  `ScsiController`. `PEDIGREE_CRIPPLE_HDD` still disables writes and flushes.

The first slice issues one command at a time. The inherited controller worker
also serializes requests across its ports. It does not yet implement NCQ, TRIM,
scatter/gather into caller pages, ATAPI, port multipliers, 4Kn, hotplug, paging I/O,
suspend/resume, MSI/MSI-X, or recovery/retry of an offline port. It requires the
PCI function to start in D0 with MSI/MSI-X disabled. DMA assumes coherent x86
memory; there is no IOMMU or noncoherent architecture support.

AHCI provides the interface needed for NCQ and ATA Data Set Management/TRIM, but
those features and any performance improvement need separate implementation and
measurement. Queue depth and command-slot ownership should precede NCQ; a disk
discard API and filesystem allocation integration should precede TRIM.

## QEMU smoke test

Use an isolated build and a disposable copy of a bootable Pedigree root image.
The optional `ahci-smoke` module writes only after matching an entire marked
4096-byte fixture header, its exact disk size and controller/port relationship
to the mounted root disk.

First configure an isolated x86-64 target build with the usual Pedigree
toolchain and host-tool settings. In that configured build, enable the test:

```sh
cmake -S . -B build \
  -DPEDIGREE_AHCI_SMOKE_TESTS=ON \
  -DPEDIGREE_CRIPPLE_HDD=OFF \
  -DPEDIGREE_WITH_INIT=OFF
cmake --build build --target livecd --parallel 6
uv run scripts/test_qemu_ahci.py \
  --iso build/pedigree.iso --root-image /path/to/disposable-root.img \
  --cpus 1 --run-dir /tmp/ahci-up
uv run scripts/test_qemu_ahci.py \
  --iso build/pedigree.iso --root-image /path/to/disposable-root.img \
  --cpus 4 --run-dir /tmp/ahci-smp
uv run scripts/test_qemu_ahci.py \
  --iso build/pedigree.iso --root-image /path/to/disposable-root.img \
  --cpus 4 --run-dir /tmp/ahci-error --inject-read-error
uv run -m unittest discover -s tests -p test_qemu_ahci.py
```

The run directory must be new. The harness uses QEMU `ich9-ahci`, snapshots the
root image and creates a persistent, disposable 32 MiB scratch disk. No network
device is attached. The IDE CD-ROM only supplies the boot image; both disks use
AHCI.

The guest checks the actual root filesystem's backing disk, patterned reads,
invalid-range rejection, boundary writes, checked synchronization, cache
retirement and uncached rereads. Positive interrupt-completion counts are
required. After stopping its own QEMU process, the harness checks every byte of
the scratch image, including untouched neighbors, and requires traced guest ATA
flush commands on the scratch port. QEMU's own exit-time flushing alone is not
treated as evidence of a guest flush.

`--inject-read-error` additionally uses QEMU's `blkdebug` to inject a single EIO
at 24 MiB on the scratch disk after the persistence checks. The guest must
reject that read and subsequent uncached I/O promptly, release its cache pins,
and complete a fresh hardware read on the root port. The harness requires a
traced task-file error on the scratch port as well as the normal byte checks.

Each run preserves `report.json`, the QEMU command, serial output, device traces
and scratch image. The harness deadline bounds the VM run; it does not exercise
the driver's timeout or unresponsive-DMA-engine paths. Emulation cannot establish
T420 firmware handoff, link behavior or hardware errata coverage.

### Validation checkpoint (2026-09-07)

QEMU 11.0.3 (`pc`, `ich9-ahci`) passed with both one and four CPUs:

- AHCI root mounting, patterned reads, invalid ranges, boundary writes, checked
  flushes, uncached rereads and full 32 MiB host comparison.
- Injected scratch read EIO, prompt rejection of further uncached I/O, and a
  successful direct read on the root port after the error.
- Positive threaded interrupt-completion counts and traced ATA FLUSH CACHE EXT.

An IDE-only root boot also passed with AHCI absent. All 17 Python fixture and
evidence-parser tests passed. The first VM run exposed eager polling consuming
all completions before the interrupt thread; waiting for the interrupt first
fixed it. An initial error test attempted to retire the mounted ext2
superblock's persistent cache pin; the corrected test uses direct root-port I/O.

Physical hardware, watchdog expiration, failed-engine shutdown, module unload,
and power-loss durability have not been validated. The byte comparison and
guest flush trace establish the emulated persistence path, not drive firmware
behavior during power loss.

## Sources and license

The implementation is ISC licensed and was written from the public interfaces
and Pedigree's existing permissively licensed storage code. It belongs in the
main repository; it does not require the encumbered-driver repository.

- [Intel AHCI 1.3.1 specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/serial-ata-ahci-spec-rev1-3-1.pdf),
  sections 3, 4 and 10: registers, DMA structures, initialization and ownership.
- [Serial ATA 2.5 specification](https://sata-io.org/system/files/specifications/SerialATA_Revision_2_5_Gold.pdf),
  section 10.3.4: Register Host-to-Device FIS.
- [ATA/ATAPI-5 working draft](https://www.seagate.com/support/disc/manuals/ata/d1153r17.pdf),
  IDENTIFY layout and command-set validity conventions. LBA48 and extended
  command definitions follow Pedigree's existing `AtaDisk` implementation.

No Linux or other GPL driver implementation was incorporated.
