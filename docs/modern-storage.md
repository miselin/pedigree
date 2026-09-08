# Modern storage validation

Build a UEFI test image with both destructive smoke modules enabled. Writes must
be enabled and userspace init disabled. The harness supplies marked disposable
scratch disks and uses a snapshot for every boot/root disk.

```sh
cmake -S . -B build -DPEDIGREE_CRIPPLE_HDD=OFF -DPEDIGREE_WITH_INIT=OFF \
  -DPEDIGREE_AHCI_SMOKE_TESTS=ON -DPEDIGREE_NVME_SMOKE_TESTS=ON
cmake --build build --target uefi-image --parallel 6
uv run scripts/test_qemu_storage.py --image build/pedigree-uefi.img \
  --root ahci --ahci-sector-size 512 --cpus 4 --run-dir /tmp/storage-ahci-smp
uv run scripts/test_qemu_storage.py --image build/pedigree-uefi.img \
  --root nvme --cpus 4 --run-dir /tmp/storage-nvme-smp
```

Repeat each profile with `--cpus 1` at the storage milestone. Use a new run
directory each time. `--ovmf` selects an alternative OVMF code image. QEMU uses
Q35 with its AHCI controller and a separate NVMe controller. Every profile
includes NVMe namespace 7 with 512-byte blocks and namespace 23 with 4 KiB
blocks. The AHCI profile boots its root through AHCI and also tests a marked
AHCI scratch disk. QEMU 11.0.3 IDE/AHCI does not provide native 4Kn
IDENTIFY geometry; AHCI 4Kn needs physical-device qualification. NVMe supplies
the guest 4Kn coverage. The NVMe profile retains a separate ESP boot disk and wraps
the root filesystem in GPT on a 4Kn namespace with ID 1.

The storage profile disables the PS/2 controller. The AHCI scratch disk is
limited to 100 read requests per second to make simultaneous NCQ commands
observable even when QEMU serves the fixture from its host cache. This is a
correctness fixture, not a throughput benchmark.

The guest tests patterned reads, shared-page and distinct-page concurrent reads,
command overlap, range rejection, interrupt completion, writes, checked flushes,
and uncached rereads. NVMe tests also exercise a PRP list. The harness requires
completion markers, guest-issued flush traces for each scratch device, and
compares the backing-image contents after stopping QEMU. Logs, traces, fixture
images and `report.json` remain in the run directory, including on failure.

After testing, restore the original CMake values for write protection, init and
smoke modules, and rebuild the normal image. Never replace an image used by a
running guest. Smoke images are for these disposable fixtures only.

## Partition support

MBR and EBR offsets use logical sectors. Extended chains have bounded traversal,
cycle detection and disk/container range checks. Protective MBR entries are
never published as filesystem partitions.

GPT discovery validates header and entry-array CRCs, locations, usable bounds,
entry strides, partition ranges, overlaps and unique partition GUIDs before
publishing children. A damaged primary can fall back to the independently
validated backup on disks with a protective MBR. No on-disk repair is performed.
Tables exceeding 4,096 entries or 1 MiB are unsupported. Apple partition maps
remain restricted to 512-byte logical sectors. Partition cache handling retains
its existing limitation for incomplete final cache pages.

GPT layout follows [UEFI chapter 5](https://uefi.org/specs/UEFI/2.10/05_GUID_Partition_Table_Format.html).

## PCI and interrupt prerequisites

PCI enumeration combines 64-bit BAR pairs and probes them with decoding and
bus mastering disabled. It preserves the command register without writing
status-register bits that clear on write. This allows NVMe BARs above 4 GiB to
be mapped correctly.

PCI IRQ registration configures its PIC ELCR line as level triggered while
masked, verifies the write, and preserves other lines. The original trigger is
restored when the last handler retires. OVMF can leave ELCR entirely in edge
mode; a completion serviced while masked then leaves a stale edge that can
quarantine a shared IRQ line. Polling also records acknowledged interrupt causes
for a pending threaded callback, without counting them as interrupt completions.

## Qualification boundaries

### Validation checkpoint (2026-09-08)

QEMU 11.0.3 with OVMF and Q35 passed the AHCI-root and native-4Kn NVMe/GPT-root
profiles with one and four CPUs. Each profile also passed both sparse NVMe
scratch namespaces. Checks included real interrupt-completion deltas, up to
12 simultaneous scratch AHCI commands and 15 NVMe commands, guest-issued flush
traces, and complete comparison of every 32 MiB scratch image after shutdown.
The storage fixture/evidence tests passed (21 tests), as did the focused native
PIC, PCI BAR, GPT and disk-page tests (58 tests).

The milestone exposed and repaired 64-bit BAR enumeration, low-memory DMA
exhaustion, polled-completion IRQ accounting, and firmware leaving PCI lines
edge triggered. A PS/2 mouse command wait also blocked one boot before the
storage tests; bounded probing and retaining the absent-device module's POSIX
exports allowed that profile to finish. Earlier failed runs were retained for
diagnosis rather than discarded after successful retries.

### Remaining hardware qualification

QEMU establishes the emulated I/O and persistence paths. Physical firmware
handoff, PCI interrupt routing, power-loss durability, link errata, controller
reset failures and real SSD throughput still need hardware qualification.
MSI-X, multiple CPU-local queues, zero-copy DMA, request merging, discard,
hotplug and power management are follow-up work.
