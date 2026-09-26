# Firmware handoff contracts

Pedigree establishes the state needed by its supported hardware paths and rejects
unsupported modes before using their registers. Existing firmware assignments
are retained. New PCI MMIO allocations use only unused space inside configured
host and bridge windows; they do not move working BARs or rebalance bridges.

The UEFI loader refreshes the memory map and retries `ExitBootServices` up to four
times when the map key is stale. It validates descriptor geometry and address
arithmetic on each attempt, and passes only the successful map to the kernel.
After an exit attempt it uses only the map/exit services; terminal failure halts
locally because console protocols and returning to firmware are no longer safe.
See [UEFI 7.4.6](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html#efi-boot-services-exitbootservices).

The local APIC checks IA32_APIC_BASE on each processor and uses the MMIO xAPIC or
MSR x2APIC backend matching the inherited mode. Invalid modes and APIC IDs above
the current eight-bit topology limit are rejected. The x86 backend has 64 PCI
message vectors. MSI-X groups can request a destination CPU for each entry;
completion workers are created on demand and pinned to the same CPU. Legacy
single-vector requests retain the bootstrap destination. A retired vector can
be reused on its original CPU only by a handler that explicitly tolerates
spurious delivery and verifies its own device's pending work. Source shutdown
and callback retirement must succeed first; masking does not drain posted MSI
writes. Other handlers keep retired vectors reserved. ARM retains its existing
16 message slots and default interrupt destination.

NVMe negotiates up to eight I/O queue pairs, bounded by online CPUs, controller
grants, MSI-X entries, available vectors and BAR doorbells. Submissions choose
a queue by CPU and each queue has independent completion processing. The driver
limits aggregate I/O slots to 32 to bound memory and DMA mapping costs, and
falls back to fewer queues or one shared vector. Writes and flushes retain their
ordering lock; multiqueue currently enables concurrent reads, not parallel writes.

With uACPI and a usable I/O APIC, PCI INTx evaluates AML `_PRT` tables at root
and bridge scopes after `_PIC(APIC)`, swizzling only through bridges without a
table. Link descriptors are resolved by SourceIndex. Existing shared level IRQ
and ExtendedIRQ assignments are retained; disabled links with one simple IRQ
resource can use `_PRS`/`_SRS`, followed by `_STA`/`_CRS` readback. Dependent
alternatives and general multi-resource link reconfiguration are unsupported.
Low GSIs reserve their MADT-mapped ISA inputs against PIC users and remain masked
on the PIC. Routing failures after APIC selection cannot use stale legacy routes.
Without AML routing, supported static routes and the PIC fallback remain available.
Other ISA IRQs and the current SCI adapter remain on the PIC.

The RTC uses the century index declared by a validated FADT, accepting only CMOS
RAM indices accessible through its seven-bit selector. An absent index uses a
fixed 1970–2069 interpretation and never reads or writes a guessed century byte.
Writes outside that window require a declared century register. BCD/binary and
12/24-hour formats are decoded and preserved, and invalid calendar values are
rejected before publication or writes. Bootstrap selects the clock divider and
periodic rate, clears inherited update inhibition, interrupt enables and automatic
DST adjustment, and verifies those controls. Calendar snapshots inhibit updates
under the CMOS lock after a final UIP check protected from preemption. The early
software clock has a valid 1970-01-01 fallback until RTC acquisition completes.
See [ACPI RTC fields](https://uefi.org/specs/ACPI/6.5/04_ACPI_Hardware_Specification.html).

PCI mechanism-1 accesses validate the complete byte range and serialize each
CF8/CFC transaction with an IRQ-saving lock. Legacy dword-index callers cannot
wrap past the 256-byte configuration space. Command updates use the 16-bit
register, preserve unrelated bits, and require readback; they never rewrite PCI
Status. ECAM supplies extended configuration access and bounded capability-chain
discovery. BAR sizing temporarily masks only address decoding, preserving
firmware bus mastering until driver takeover. It restores and verifies BARs and decoding
before creating mappings or logging. This follows the separation of decoding and
bus mastering in [Linux PCI enumeration](https://raw.githubusercontent.com/torvalds/linux/master/drivers/pci/probe.c).

AHCI, NVMe, EHCI, UHCI and xHCI inspect the live function before acquisition. The
supported profile requires D0, a valid bounded capability chain and a matching
retained BAR mapping. AHCI, NVMe and xHCI can use MSI/MSI-X without a legacy INTx
pin; EHCI and UHCI retain their legacy pin/line requirements. MSI/MSI-X controls
are disabled with width-correct writes and readback. Drivers establish their
required decoder, bus-master and INTx settings at the ownership/DMA boundaries and
verify that BARs and interrupt metadata survived takeover. Non-D0 controllers are
rejected; a D3-to-D0 transition can reset configuration and needs a separate power
and resource-restoration policy. See [PCI power management](https://docs.kernel.org/power/pci.html).

These checks do not take general ownership of platform power management. PCI
Interrupt Line remains firmware-provided fallback metadata. Storage readiness
requires an actual command completion through the registered IRQ handler before
publishing disk endpoints; a polled completion alone fails that check.
Shared-line delivery is operational evidence, not proof of which device
electrically asserted the line. The global PCI lock serializes kernel callers;
it cannot serialize firmware SMM execution.

## PCIe ownership and resources

The `external/uacpi` submodule loads and initializes the AML namespace. PCI root
discovery evaluates `_SEG`, `_BBN` and `_CRS`, accepting segment-zero bus ranges and fixed,
positive-decode producer windows. Native services obtain their requested `_OSC`
control bits through query and commit; discovering a capability alone does not
grant ownership. SCI/GPE callbacks and deferred AML work are supported, but the
current interrupt adapter accepts only ISA-range GSIs.

The PCI resource inventory reserves existing BARs and bridge memory windows in
PCI bus address space. New allocations must fit a matching host window and every
upstream forwarding window, respect address width/alignment, and avoid endpoint
and sibling-bridge reservations. An ancestor bridge window is a forwarding
boundary. Active inherited SR-IOV apertures, malformed capabilities or unknown
resource extents prevent allocation. x86 rejects translated host windows while
its MMIO translation remains identity-only. ARM uses device-tree host windows
and their CPU translations when no ACPI namespace is available.

Native x86 AER requires `_OSC` ownership and supported root-port message routing.
Correctable reports are acknowledged; uncorrectable reports stop bus mastering
on the requester, or the branch when individual containment is insufficient.
Failure to contain is fatal. This service does not reset controllers, restore
driver state or resume I/O automatically. AER and native slot events can share
a root-port interrupt when both select message zero and their respective `_OSC`
ownership grants are available. Active PME interrupt users prevent takeover;
native PME handling and bridge resource rebalancing remain unsupported.

## Native PCIe slots

The x86 slot service requires a hotplug-capable root port, native `_OSC`
ownership, supported message routing and configured bus and MMIO windows.
Runtime insertion currently accepts one single-function Type 0 endpoint at
device 0, function 0 on the secondary bus. It allocates MMIO BARs within existing
forwarding windows. I/O BARs, inserted bridges, multifunction endpoints and
active VFs are rejected; bus numbers and bridge windows are not rebalanced.

NVMe registers for boot and runtime attachment, including when no controller is
present at module load. Newly attached namespaces notify the partition service.
Driver registration serializes binding and removal with module unload;
partition notifications retain the service while it handles the call.

Attention-button eject prepares the driver, drains I/O and stops DMA before
reclaiming the endpoint and turning off slot power. Mounted or paging disks,
outstanding cache loans and legacy DMA domains that cannot be detached prevent
removal. Cancellation or a busy result leaves the device powered. Surprise
removal or a power fault contains the branch and retains its resources; automatic
recovery is not provided. Drivers outside the registration interface do not gain
orderly removal support automatically.

## SR-IOV and DMA isolation

`PciVirtualFunctions` is an explicit PF-owned group; enumeration never enables
VFs automatically. `enable()` requires the PF quiesced with bus mastering off,
disabled VF decoding, collision-free requester IDs, and reserved VF BAR
apertures. VFs remain off the global device tree. The group waits for usable VF
configuration, validates it, clears bus mastering and installs isolated DMA
domains before exposing `function()` results. `bar()` validates each VF's slice
of the PF aperture; VF configuration BARs remain zero. Cached VF identity uses
the PF vendor ID and its SR-IOV VF Device ID, including when VF configuration
offset zero reads as all ones.

The PF and group must outlive VF users. Drivers attach beneath the group-owned
VF nodes and must be removed before `disable()`. Teardown refuses active bus
mastering or live DMA mappings, disables VF decoding, waits for hardware, then
reclaims domains, nodes and reservations and restores the PF's original SR-IOV
state. Unverifiable quiescence or rollback is fatal. These APIs do not assign VFs
to virtual machines or establish ACS/peer-to-peer isolation.

Intel VT-d provides eight requester-domain slots shared by PFs and VFs. Strict
domains expose only explicitly mapped DMA pages, including pages above 4 GiB
through 32-bit IOVAs. The backend requires one segment-zero DRHD and accepts
include-all coverage, direct endpoint scopes and direct bridge scopes. Bridge
coverage validates the live bridge header and its primary, secondary and
subordinate bus numbers; multi-hop scope paths are unsupported. Slot exhaustion
rolls back VF enablement. ARM currently has no corresponding SMMU isolation
backend, so isolated VF enablement fails.

NVMe exposes primary capability and secondary-controller queries, queue and
interrupt resource allocation, secondary online/offline commands, and namespace
attachment changes. Callers must quiesce affected I/O and retire published disk
and cache users before changing controller or namespace ownership. Primary
resource allocation changes require a controller-level reset; these management
APIs do not rescan or republish disks automatically.

## USB firmware handoff

EHCI disables and verifies legacy SMI sources after ownership handoff, including
when BIOS ownership was already clear. The existing BIOS-unowned shortcut is
retained because requesting that already-released semaphore can hang some
firmware; it is a compatibility exception to the unconditional request in EHCI's
handoff flow. A failed ownership request is withdrawn before any controller
register takeover. If a previously BIOS-owned controller fails during startup,
EHCI drains enumeration and driver probes, halts DMA, detaches its schedules,
unregisters its IRQ, and releases its transfer buffers before restoring the
saved legacy SMI enables and clearing OS Owned. Firmware receives its original
PCI decoder/DMA permissions, but no old schedule pointers are replayed.

Startup recovery covers controller initialization, initial port enumeration,
nested hubs, and matched driver initialization failures. A successfully bound
non-hub driver commits the controller to native operation; later device failures
cannot return the whole controller to firmware and disrupt that device. An
unmatched interface is not a failed probe. Recovery stays armed until that first
binding, including probes from drivers loaded after enumeration and hotplug
before any device has become usable. A controller that started BIOS-unowned
keeps the existing native/hotplug behavior and has no firmware handback path.

Firmware reclaim is polled for at most one second. The log distinguishes a
confirmed BIOS-owned semaphore from a release that firmware did not acknowledge.
Even confirmation does not prove that firmware resumed keyboard emulation; this
requires testing on the machine. Native PS/2 IRQ1/IRQ12 input remains independent.
There is necessarily a USB input gap while controller ownership changes.
If DMA cannot be stopped, the kernel retains its fatal teardown policy rather
than giving firmware a controller that could still access freed OS buffers.

UHCI verifies legacy emulation/SMI controls and explicitly enables
its I/O decoder. Partially initialized controllers are not published, and cleanup
does not halt hardware still owned by firmware or resume its old DMA configuration.
See [EHCI](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf)
and [Linux's handoff compatibility policy](https://raw.githubusercontent.com/torvalds/linux/master/drivers/usb/host/pci-quirks.c).

## Validation limits

Host validation passed 74 focused tests and the hosted-core lifecycle checks,
including recursive asynchronous request publication. Firmware-service and
RTC/APIC fixtures cover states normal OVMF boots do not produce.

Fresh x86 UEFI QEMU guests cover uACPI initialization. One- and four-CPU guests
completed two SR-IOV NVMe cycles and two orderly ejects separated by a native
reinsertion, including busy-disk eject refusal and persisted-data checks. The
four-CPU run used 5 GiB RAM and Intel VT-d, mapped pages above 4 GiB in four domains, and
reported no VT-d faults in tracing. A separate two-CPU guest exercised repeated
correctable AER acknowledgement and nonfatal requester containment while AER and
the native slot service shared an IRQ.

The interrupt-scaling checks additionally exercised four CPU-local NVMe I/O
queues, verified completions on every queue, and observed vector reuse on CPUs
0 through 3 during reinsertion. A four-controller guest used 17 simultaneous
NVMe MSI-X vectors and verified negotiation down to one I/O queue when the
controller offered only one. A single-CPU NVMe-root guest passed with one
shared MSI-X entry. Both profiles completed real SCSI commands over AML-routed
INTx through a PCI bridge and I/O APIC. General disabled-link firmware templates
and physical-board interrupt routing remain unqualified.

The restored normal configuration, with storage smoke modules disabled, also
booted a fresh UEFI image to the serial login prompt on one and four CPUs.

ARM64 kernel, ACPI, PCI, NVMe and partition coverage is compile-only; ARMv7
remains unqualified. Inherited x2APIC has no guest or hardware qualification.
These x86 QEMU results do not establish physical hardware compatibility or timing.
