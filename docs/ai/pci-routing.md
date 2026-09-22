# QM67 PCI interrupt routing

Pedigree's `chipset` module repairs native Intel QM67 INTx routes before PCI
device drivers load. Its QM67 backend covers the ThinkPad T420's 82579LM,
SATA/AHCI and both EHCI controllers when firmware leaves their PCI Interrupt
Line bytes at `0xff`.

The required module order is `pci-enumeration` → `chipset` → `pci` → device
drivers. `pci-enumeration` discovers PCI functions and their BARs; `chipset`
applies platform configuration; `pci` indicates that both stages have completed.
Existing in-tree and external drivers continue to depend on `pci`. The chipset
module succeeds without changes on machines without a matching backend.

All three modules remain loaded for the kernel lifetime: enumerated device nodes,
PIRQ programming and PIC reservations have no removal or rollback procedure.
They are included in both normal initrds and static driver builds. When building
a custom module set, keep both prerequisites enabled with `pci`.

The QM67 LPC bridge (`8086:1c4f`) remains in the device tree without ordinary BAR
sizing. Its command-register decoding bits are read-only ones, and it has no
standard BARs. Other PCI functions retain the existing decoding and restoration
checks.

For native functions on bus 0, the router checks the function's Interrupt Pin
against DxxIP and reads its DxxIR selector. It leaves those firmware selectors
unchanged. Existing valid PIRQ-to-PIC routes are preserved if the PIC can reserve
their level-triggered lines. Disabled routes use IRQ10 or IRQ11 after checking
SCI, TCO, HPET and the fixed PS/2 mouse IRQ12 source and obtaining PIC
ownership. Every PIRQ write is read back; failure restores the old value. A
restoration failure stops boot.
Reservations remain masked until a handler registers and survive driver removal.

The configuration-space Interrupt Line byte and both software copies are only
published after routing succeeds. Failed per-function routing leaves the software
IRQ invalid, including when firmware originally supplied a valid-looking byte.
This prevents a driver from binding to an obsolete IRQ. Ordinary driver D0,
capability, resource and interrupt-delivery checks still apply.

Look for these messages (the PIRQ letter and IRQ vary with firmware):

```text
PCI: QM67 native INTx routing: RCBA=... OIC=... SCI=... excluded=...
PCI: QM67 PIRQA=80 -> PIC IRQ 10
PCI: 0:25:0 8086:1502 INTA -> PIRQA -> IRQ 10
```

`excluded` is a bitmap of legacy IRQs reserved for chipset sources. If HPET
registers are inaccessible, IRQ11 is conservatively unavailable. Valid enabled
firmware routes remain in place; a route is moved only when it conflicts with an
explicitly excluded chipset or fixed legacy source. Failure messages retain the
original PIRQ byte and identify the failing routing check.

## Scope and validation

The QM67 backend matches only the `8086:1c4f` ISA bridge at `00:1f.0`. It is a
native-function PIC route implementation, not general ACPI PCI
resource allocation or I/O APIC support. Root ports and downstream endpoints are
excluded: their mapping requires bridge topology and physical-port/function
remapping, and cannot be inferred from a root port's own Interrupt Pin. Firmware
ISA resource ownership beyond the explicitly checked chipset sources is not
resolved through AML. Unsupported machines keep their existing routing behavior.

Native tests exercise BAR probing, route selection and rollback, SCI/TCO/HPET
exclusions, and PIC reservation lifetime/sharing. QEMU can check regressions in
PCI discovery, AHCI, EHCI and the shared 82574L packet path, but it does not emulate
the QM67 route registers or qualify physical 82579LM operation. Real T420 testing
must confirm the route messages, interrupt-driven AHCI/EHCI progress, and Ethernet
traffic with a freshly rebuilt external NIC module.

## Hardware reference

Implemented from Intel's [6 Series/C200 datasheet, 324645-006](https://www.intel.com/content/dam/www/public/us/en/documents/datasheets/6-chipset-c200-chipset-datasheet.pdf):

- Chapter 10, pp375–398: dword-only RCBA access, TCTL, DxxIP and DxxIR.
- Section 10.1.42, p404: HPET address decoding.
- Sections 13.1.3/13/16/18, pp451–459: LPC command, SCI and PIRQ registers.
- Section 19.1.21, p768: root-port local interrupt pin.
- Sections 20.1.2–5, pp803–807: HPET capability, status and timer routing.

No Linux or other GPL implementation code is included in this change.
