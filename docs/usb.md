# USB driver coverage

Mass storage binds the SCSI transparent command set over Bulk-Only Transport
(class 8, subclass 6, protocol 0x50). UAS and other storage protocols are not
claimed. GET_MAX_LUN may stall for a single-LUN device; other errors and invalid
LUN counts fail enumeration. All command directions validate CBW and CSW sizes,
tags, signatures, status and residue. Short reads cannot publish incomplete SCSI
cache fills. A stalled CSW gets one clear-halt retry; invalid transport state
requires reset and both endpoint halts cleared before the next command.

Device enumeration learns endpoint zero's packet size from the first eight
descriptor bytes, observes SET_ADDRESS recovery time, checks control-transfer
byte counts and descriptor boundaries, and selects alternate settings using the
interface number. Interrupt endpoint intervals are retained. Unsupported
high-bandwidth interrupt endpoints are declined by the interrupt submission
path; unrelated interfaces remain available.

HID uses the completed report length and ignores failed transfers. Report IDs
have independent previous-report state; short or unknown reports leave that
state unchanged. Multiple top-level collections and the global push/pop stack
are supported. Descriptor items, field widths, total report size and collection
depth are bounded. Relative fields use their declared bit width for sign
extension, including packed 12-bit axes. SET_IDLE targets the bound interface.

EHCI interrupt queues use a periodic tree with power-of-two frame intervals and
explicit microframe start/completion masks. The schedule is stopped before
changing hardware links. Conservative microframe admission limits reserve room
for asynchronous traffic. Recurring reports remain inactive until their callback
finishes, then their original DMA buffer positions are restored before Active is
published. Cancellation unlinks every periodic reference before freeing queues.
Supported high-speed interrupt intervals are 125 microseconds through 1024 ms;
high-bandwidth endpoints and longer intervals are declined. Full/low-speed split
routes use the nearest high-speed hub and its one-based downstream port. Stock
QEMU's full-speed hub does not validate this transaction-translator path.

EHCI bulk packets sharing a physical page are combined into a transfer
descriptor, so a 4 KiB full-speed storage request does not exhaust the descriptor
pool. Short IN transfers complete without waiting for unsent packets. Completion
bytes are counted once even when an earlier descriptor generates an interrupt.

OHCI recurring interrupt-IN and xHCI remain unavailable. This prevents a claim
of full USB 1/2/3 coverage. Physical hub, transaction translator, disconnect,
timeout and controller-specific behavior need hardware qualification.

UHCI recurring transfers use separate periodic frame roots and honor the
largest power-of-two interval no greater than the endpoint interval. Reports
remain inactive until callback delivery finishes; rearming preserves the
subscription generation and packet toggle. Inactive IOC bits are cleared while
reports await delivery. Bulk short reads stop at the completed packet, and
errors are attributed to individual transfer descriptors. Early short control-IN
transfers still need additional status-stage handling.

## Disposable guest checks

```sh
cmake -S . -B build -DPEDIGREE_USB_SMOKE_TESTS=ON \
  -DPEDIGREE_AHCI_SMOKE_TESTS=OFF -DPEDIGREE_NVME_SMOKE_TESTS=OFF \
  -DPEDIGREE_CRIPPLE_HDD=OFF -DPEDIGREE_WITH_INIT=OFF
cmake --build build --target uefi-image --parallel 6
uv run scripts/test_qemu_usb.py --image build/pedigree-uefi.img \
  --controller ehci --sector-size 4096 --cpus 4 --run-dir /tmp/usb-ehci-smp
```

Use `--controller uhci` for UHCI. Repeat with one CPU and 512-byte blocks at the
milestone. The harness uses a root snapshot and creates its own marked scratch
disk. UHCI uses two controllers because each has only two root ports. Restore
the original CMake options and rebuild the normal image after testing.

`PEDIGREE_USB_SMOKE_TESTS=ON` requires `PEDIGREE_CRIPPLE_HDD=OFF` and
`PEDIGREE_WITH_INIT=OFF`. Use a fresh UEFI image and disposable root/storage images.
The test requires one 32 MiB USB mass-storage disk with 512- or 4096-byte logical blocks distinct from
the root disk. Its first 4096 bytes must be zero except:

| Offset | Value |
| --- | --- |
| 0 | ASCII `PEDIGREE-USB-SMOKE-v1`, without terminator |
| 32 | byte 1 |
| 40 | little-endian uint64 33554432 |
| 48 | little-endian uint32 logical block size (512 or 4096) |

All other disk bytes use
`((offset * 37) ^ (offset >> 8) ^ (offset >> 16) ^ 0x5a) & 255`.
The test validates the entire header before writing. It writes 8192 bytes at
8388608 and 4096 bytes at 33550336 using seed `0xa5`, flushes, retires the cache
pages and rereads. Verify those ranges in the stopped host backing image too.

After `USB-SMOKE: READY input`, inject keyboard HID scancode 4 down/up, relative
mouse X +17 and Y -9, and left-button down/up. InputManager events produce
individual bit markers (1/2 keyboard, 4/8 axes, 16/32 button). The final marker is
`USB-SMOKE: PASS complete`. Disable the QEMU PS/2 controller so that the keyboard
and mouse evidence comes from USB. High-speed QEMU keyboard and mouse devices
can attach directly to EHCI; UHCI uses their USB 1.x profiles.

The BOT contracts cover complete/short/failed IN, OUT and no-data commands plus
CSW validation and recovery. HID contracts check report IDs, packed signed
axes, short-report suppression and descriptor bounds. These contracts execute
inside the guest smoke module as well as the full hosted Linux test lane. The
macOS hosted core subset does not include USB.

### Validation checkpoint (2026-09-08)

QEMU 11.0.3, Q35 and OVMF passed EHCI and UHCI with a 512-byte storage fixture
on one CPU and a 4 KiB fixture on four CPUs. Every profile passed the BOT/HID
contracts, actual keyboard press/release and mouse motion/button callbacks,
storage writes and uncached rereads, 15 guest SYNCHRONIZE CACHE commands, and
the complete 32 MiB host backing-file comparison. PS/2 input was disabled.
The 11 host fixture/QMP/evidence-parser tests passed.

The first EHCI boot exposed an incorrect capability-length register mask;
the corrected driver accepts QEMU's 0x20-byte capability region. The controller
layout, interval scheduling, input-buffer ownership and full-speed transfer
descriptor capacity fixes were included in these guest runs. This checkpoint
does not establish physical-device timing, disconnect or transaction-translator
coverage.

Behavior follows the USB-IF's
[Bulk-Only Transport specification](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
[HID 1.11 specification](https://www.usb.org/sites/default/files/hid1_11.pdf), and
Intel's [EHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf).
