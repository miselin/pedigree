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

OHCI recurring interrupt-IN remains unavailable. Physical hubs, transaction
translators, timeout recovery and controller-specific behavior need hardware
qualification.

UHCI recurring transfers use separate periodic frame roots and honor the
largest power-of-two interval no greater than the endpoint interval. Reports
remain inactive until callback delivery finishes; rearming preserves the
subscription generation and packet toggle. Inactive IOC bits are cleared while
reports await delivery. Bulk short reads stop at the completed packet, and
errors are attributed to individual transfer descriptors. Early short control-IN
transfers still need additional status-stage handling.

## xHCI

xHCI supports directly attached USB 1.x, USB 2.0 and SuperSpeed devices using
control, bulk and recurring interrupt transfers. Supported Protocol capabilities
provide the root-port speed mapping; USB 2 and USB 3 ports can have different
hardware numbers for the same connector. SuperSpeed enumeration accepts the
512-byte endpoint-zero encoding, companion descriptors and 1024-byte bulk
endpoints with bursts. External hubs, isochronous transfers and streams are
declined.

The driver uses one command ring, one event ring and one transfer ring per
endpoint, with one active transfer per endpoint. Rings contain 256 entries;
transfers use private DMA buffers and are limited to 64 KiB. A shared threaded
INTx handler processes completions and a separate worker delivers callbacks.
Recurring input buffers remain stable until callbacks return. Cancellation stops
the endpoint and advances its dequeue pointer before releasing transfer memory;
device retirement disables its slot before releasing contexts. A controller that
cannot halt DMA is a terminal failure.

The initial limits are 16 active device slots, 32 root ports and 32 scratchpad
pages. Context sizes of 32 and 64 bytes are supported with a 4 KiB page size and
DMA below 4 GiB. BIOS ownership is requested before controller reset. MSI-X,
multiple interrupters, zero-copy transfers, power management and reconnect
qualification remain follow-up work. SuperSpeed link support does not establish
physical SSD throughput.

## Disposable guest checks

```sh
cmake -S . -B build -DPEDIGREE_USB_SMOKE_TESTS=ON \
  -DPEDIGREE_AHCI_SMOKE_TESTS=OFF -DPEDIGREE_NVME_SMOKE_TESTS=OFF \
  -DPEDIGREE_CRIPPLE_HDD=OFF -DPEDIGREE_WITH_INIT=OFF
cmake --build build --target uefi-image --parallel 6
uv run scripts/test_qemu_usb.py --image build/pedigree-uefi.img \
  --controller ehci --sector-size 4096 --cpus 4 --run-dir /tmp/usb-ehci-smp
```

Use `--controller uhci` for UHCI or `--controller xhci` for xHCI. Add `--root nvme`
to move the root filesystem onto a native 4 KiB GPT NVMe namespace while keeping
the ESP on a separate disk. Repeat with one CPU and 512-byte blocks at the
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

The xHCI profile mixes USB 1.x mouse, USB 2 keyboard and SuperSpeed storage.
It also requires 320 uncached storage-page reads, actual event and transfer ring
wraps and an interrupt-delivered transfer completion. After all input and storage
checks pass, QMP removes each device in turn; three distinct successful slot
retirements prove idle disconnect drained the recurring input subscriptions.

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

The xHCI milestone passed a 512-byte SuperSpeed fixture on one CPU and a 4 KiB
SuperSpeed fixture on four CPUs. The four-CPU machine also mounted its root
filesystem from a native 4 KiB GPT NVMe namespace. Both runs passed all storage
and HID contracts, 320 uncached page reads, event/transfer ring wrap evidence,
interrupt-delivered completion, 15 guest storage flushes, the complete 32 MiB
backing-file comparison and successful retirement of all three devices after
sequential QMP disconnects. The 12 host fixture/QMP/evidence tests passed.
EHCI with 512-byte blocks on one CPU and UHCI with 4 KiB blocks on four CPUs
passed again after the shared xHCI enumeration changes. The normal UEFI image
also rebuilt with smoke tests disabled and the original init/write settings.

The first xHCI boot failed because the event segment table was programmed while
PCI bus mastering was disabled. QEMU traces showed the resulting controller
error before the first command. Bus mastering now starts after DMA metadata is
initialized and before its register addresses are published; both host-system
and controller errors stop further I/O. Idle disconnect is covered; disconnect
during active storage I/O, reconnect, external hubs and physical controllers are
not established by this checkpoint.

Behavior follows the USB-IF's
[Bulk-Only Transport specification](https://www.usb.org/sites/default/files/usbmassbulk_10.pdf),
[HID 1.11 specification](https://www.usb.org/sites/default/files/hid1_11.pdf), and
Intel's [EHCI specification](https://www.intel.com/content/dam/www/public/us/en/documents/technical-specifications/ehci-specification-for-usb.pdf).
The xHCI driver follows Intel's
[xHCI 1.2b specification](https://cdrdv2-public.intel.com/625472/625472_xHCI_Rev1_2b.pdf).
