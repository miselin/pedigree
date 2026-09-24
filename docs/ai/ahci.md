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

## QEMU smoke test

Use the UEFI [modern storage harness](modern-storage.md). Configure a separate
x86-64 target build with the usual Pedigree toolchain and host-tool settings,
then enable both storage smoke modules. The harness supplies marked disposable
scratch disks and snapshots the boot/root image.

```sh
cmake -S . -B build \
  -DPEDIGREE_AHCI_SMOKE_TESTS=ON -DPEDIGREE_NVME_SMOKE_TESTS=ON \
  -DPEDIGREE_CRIPPLE_HDD=OFF -DPEDIGREE_WITH_INIT=OFF
cmake --build build --target uefi-image --parallel 6
uv run scripts/test_qemu_storage.py --image build/pedigree-uefi.img \
  --root ahci --ahci-sector-size 512 --cpus 1 --run-dir /tmp/ahci-up
uv run scripts/test_qemu_storage.py --image build/pedigree-uefi.img \
  --root ahci --ahci-sector-size 512 --cpus 4 --run-dir /tmp/ahci-smp
uv run -m unittest discover -s tests -p test_qemu_storage.py
```

Each run directory must be new. The harness uses Q35 AHCI, checks patterned
reads, invalid ranges, concurrent commands, writes and uncached rereads, and
requires positive interrupt-completion counts. After stopping QEMU it compares
the disposable scratch contents and checks traced guest flush commands.
QEMU's own exit-time flushing is not evidence of a guest flush.

Logs, traces, scratch images and `report.json` remain in the run directory.
Restore the original write-protection, init and smoke-module settings after
testing, then rebuild the normal image. Never replace an image in use by a guest.

The UEFI harness does not inject the scratch read error used in the historical
checkpoint below, nor does it qualify timeout or unresponsive-DMA-engine paths.
Emulation cannot establish T420 firmware handoff, link behavior or hardware errata.

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
