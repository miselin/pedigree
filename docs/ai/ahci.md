# AHCI storage

The `ahci` module supports PCI SATA controllers reporting class/subclass/programming
interface `01:06:01`. It is built for x86-64 PCI targets, including the Intel
controller family in the ThinkPad T420. The controller must be configured for
AHCI in firmware; IDE and RAID modes are not supported by this module.

The driver supports directly attached SATA disks with DMA, 48-bit LBA addressing,
logical sectors of 512, 1024, 2048 or 4096 bytes, and checked cache flushes. MBR
partition offsets now use the disk's logical sector size; GPT discovery supports
both 512-byte and 4 KiB logical sectors.

## Implemented behavior

- BIOS/OS ownership handoff, HBA reset, port initialization and IDENTIFY.
- READ/WRITE FPDMA QUEUED when both controller and disk advertise NCQ. Queue
  depth is the smaller of the controller's command slots and the disk's limit,
  up to 32. Other disks retain READ/WRITE DMA EXT.
- Independent persistent DMA buffers and command tables for each slot, below
  4 GiB. NCQ tags match hardware slots; completion follows PxSACT. PRDBC byte
  counts are checked only for non-NCQ commands, as required by AHCI 5.4.1.
- Distinct filesystem cache fills run concurrently. Overlapping fills are
  serialized until data is published, preventing readers seeing an incomplete
  cache page. Writes retain the SCSI worker's ordering.
- Threaded shared INTx completion with a timed polling fallback. A transport
  error or timeout takes the entire affected port offline and fails its active
  commands. DMA must stop before memory can be released.
- FLUSH CACHE EXT or FLUSH CACHE drains queued commands and blocks later
  submissions until completion. Ordinary commands allow 30 seconds; flushes
  allow 120 seconds. Shutdown includes a final checked device flush.
- Cache ownership and unload admission follow `ScsiDisk` and `ScsiController`.
  `PEDIGREE_CRIPPLE_HDD` disables filesystem writes and flushes.

ATAPI, port multipliers, hotplug, paging I/O, suspend/resume, MSI/MSI-X, and
recovery of an offline port remain unsupported. PCI must start in D0 with
MSI/MSI-X disabled. DMA assumes coherent x86 memory. Reads still copy through
persistent buffers; writes remain serialized, so queue support alone is not a
claim of SSD-class throughput.

Discard/TRIM is deferred until filesystem allocation changes and cache
retirement can be ordered together. Issuing it directly from block release can
race delayed writes or reallocation.

See [modern storage validation](modern-storage.md) for the current UEFI harness.
The older ISO commands below document the historical checkpoint and require a
checkout that still supports legacy ISO packaging.

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
