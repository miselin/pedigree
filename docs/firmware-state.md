# Firmware handoff contracts

Pedigree establishes the state needed by its supported hardware paths and rejects
unsupported modes before using their registers. Firmware allocations remain the
source of truth for memory reservations, PCI BARs, bridge windows, and routing;
resetting these indiscriminately can break working devices.

The UEFI loader refreshes the memory map and retries `ExitBootServices` up to four
times when the map key is stale. It validates descriptor geometry and address
arithmetic on each attempt, and passes only the successful map to the kernel.
After an exit attempt it uses only the map/exit services; terminal failure halts
locally because console protocols and returning to firmware are no longer safe.
See [UEFI 7.4.6](https://uefi.org/specs/UEFI/2.10_A/07_Services_Boot_Services.html#efi-boot-services-exitbootservices).

The local APIC MMIO backend checks IA32_APIC_BASE on each processor. Inherited
x2APIC or an invalid mode halts before MMIO access. This does not add x2APIC
support. Device interrupts still use the PIC, with the existing ELCR programming
and verification; the local APIC supplies timers and IPIs.

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

Validation uses native firmware-service stubs and RTC/APIC state fixtures, then
fresh UEFI boots with one and four CPUs, a 4 KiB-sector NVMe root and xHCI devices.
Native fixtures exercise states normal OVMF boots do not produce. QEMU does not
establish physical hardware compatibility or timing.

PCI mechanism-1 accesses validate the complete byte range and serialize each
CF8/CFC transaction with an IRQ-saving lock. Legacy dword-index callers cannot
wrap past the 256-byte configuration space. Command updates use the 16-bit
register, preserve unrelated bits, and require readback; they never rewrite PCI
Status. BAR sizing temporarily masks only address decoding, preserving firmware
bus mastering until driver takeover. It restores and verifies BARs and decoding
before creating mappings or logging. This follows the separation of decoding and
bus mastering in [Linux PCI enumeration](https://raw.githubusercontent.com/torvalds/linux/master/drivers/pci/probe.c).

AHCI, NVMe, EHCI, UHCI and xHCI inspect the live function before acquisition. The
supported profile requires D0, a valid bounded capability chain, a usable PIC
INTx pin/line, and a matching retained BAR mapping. MSI/MSI-X controls are disabled
with width-correct writes and readback. Drivers establish their required decoder,
bus-master and INTx settings at the appropriate ownership/DMA boundaries and
verify that BARs and interrupt metadata survived takeover. Non-D0 controllers are
rejected; a D3-to-D0 transition can reset configuration and needs a separate power
and resource-restoration policy. See [PCI power management](https://docs.kernel.org/power/pci.html).

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

These checks do not construct ACPI `_PRT` routing, reallocate resources, implement
IOAPIC/x2APIC, or take ownership of platform power management. PCI Interrupt Line
remains firmware-provided metadata. Storage readiness requires an actual command
completion through the registered IRQ handler before publishing disk endpoints;
a polled completion alone fails that check. Shared-line delivery is operational
evidence, not proof of which device electrically asserted the line. The global
PCI lock serializes kernel callers; it cannot serialize firmware SMM execution.
