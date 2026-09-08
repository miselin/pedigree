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
