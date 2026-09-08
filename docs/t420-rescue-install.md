# Install Pedigree from rescue Linux

The install bundle contains a compressed, raw 2 GiB ext2 root filesystem and
an `esp/` directory to copy onto a FAT32 EFI System Partition. It boots through
GRUB2 into Pedigree's EFI loader. The install kernel has disk writes enabled.

**Current UEFI display limitation:** this loader does not hand a framebuffer
to the kernel. QEMU reaches a working serial login while its screen remains
at `UEFI: loading kernel`. Treat this bundle as install media for bring-up,
not a verified standalone laptop console. A UEFI framebuffer console repair
is needed before relying on the T420 screen for login; the Ethernet driver
does not provide a remote login service by itself.

`EFI/PEDIGREE/current` includes the external Intel 82579LM driver for the T420.
`EFI/PEDIGREE/known-good` uses the same kernel with the original initrd, giving
a fallback if the external driver prevents startup. This is a clean-initrd
fallback, not a previously qualified T420 installation. Both entries use the
same root filesystem; neither rolls back filesystem changes.

Keep the Linux rescue USB available. Physical T420 graphics, storage and
Ethernet still need validation. The 82579LM path requires a firmware-configured
PHY; start with onboard Ethernet enabled and AMT disabled. Select UEFI boot
and SATA AHCI in firmware. These EFI binaries are unsigned; disable Secure
Boot if the firmware offers it.

## Prepare the rescue environment

Boot a 64-bit Linux rescue USB in **UEFI mode**. Copy/extract the whole bundle
onto storage accessible from Linux. Do not write its filesystem image over
the rescue USB or the whole SSD.

On Debian/Ubuntu rescue Linux, the relevant tools can be installed with:

```sh
sudo apt-get update
sudo apt-get install gdisk dosfstools e2fsprogs efibootmgr parted
sudo -i
bash
set -euo pipefail
cd /path/to/pedigree-t420-install
sha256sum -c SHA256SUMS
test -d /sys/firmware/efi
lsblk -o NAME,PATH,SIZE,MODEL,SERIAL,FSTYPE,MOUNTPOINTS
```

The commands below **erase the selected SSD**. They assume a dedicated target
with no data to preserve and 512-byte logical sectors. Match its model, serial
and capacity in `lsblk` before setting these three paths. Existing multi-boot
disks need separate layout planning: preserve their data and ESP, and ensure
the root entry precedes the ESP in the GPT. This erase recipe does not apply
to them.

```sh
DISK=/dev/sda
ROOT=/dev/sda1
ESP=/dev/sda2
lsblk -o NAME,SIZE,MODEL,SERIAL,FSTYPE,MOUNTPOINTS "$DISK"
test "$(blockdev --getss "$DISK")" = 512
```

Unmount all target partitions and disable any swap on that SSD before
continuing. The root partition will be **partition 1**, and the ESP will be
**partition 2**, although the ESP sits first physically. Keep this entry order
so Pedigree finds its root before probing FAT. Do not sort/renumber the GPT
entries afterward. The SSD must have more than 2.5 GiB available.

## Partition, copy and expand

```sh
sgdisk --clear \
  --new=2:2048:+512M --typecode=2:ef00 --change-name=2:'EFI System' \
  --new=1:1050624:0 --typecode=1:8300 --change-name=1:Pedigree \
  "$DISK"
partprobe "$DISK"
udevadm settle
sgdisk --verify "$DISK"
lsblk -o NAME,START,SIZE,FSTYPE,MOUNTPOINTS "$DISK"
test "$(blockdev --getsize64 "$ROOT")" -ge 2147483648

gzip -dc rootfs.ext2.gz | dd of="$ROOT" bs=4M status=progress conv=fsync
e2fsck -f "$ROOT"
resize2fs "$ROOT"
e2fsck -f "$ROOT"
dumpe2fs -h "$ROOT"

mkfs.fat -F 32 -n PEDIGREEESP "$ESP"
mkdir -p /mnt/pedigree-esp
mount "$ESP" /mnt/pedigree-esp
cp -r esp/EFI /mnt/pedigree-esp/
```

`e2fsck` returns 1 when it corrects errors. With `set -e`, that stops the
sequence: inspect its report, rerun the check, and continue only once clean.

`resize2fs` without a size grows the filesystem to the already-created
partition. It does not change the partition table. Keep this filesystem as
ext2: do not add a journal, extents, directory indexing, 64-bit block
descriptors or metadata checksums. In particular, do not use `resize2fs -b`.
The supplied image uses 4096-byte blocks and 128-byte inodes. Pedigree's current
ext2 implementation also requires individual files to stay below 4 GiB.

Copying and resizing preserve the filesystem UUID. Verify it against **both**
ESP command-line files:

```sh
ROOT_UUID=$(blkid -s UUID -o value "$ROOT")
for variant in current known-good; do
  cat "/mnt/pedigree-esp/EFI/PEDIGREE/$variant/cmdline"
  printf '\n'
done
printf 'Expected: root=UUID=%s\n' "$ROOT_UUID"
```

If deliberately changing the UUID, update both `cmdline` files to
`root=UUID=<new UUID>`. Do not change it merely to resize. Avoid attaching
multiple cloned Pedigree disks with the same UUID during boot.

## Set up UEFI boot

The bundle includes a standalone GRUB2 binary with its menu embedded. No
installed Linux system, `grub-install`, or `/boot/grub` tree is required.
Register its dedicated path in the laptop's firmware:

```sh
efibootmgr --create --disk "$DISK" --part 2 \
  --label 'Pedigree GRUB' --loader '\EFI\PEDIGREE\GRUBX64.EFI'
efibootmgr -v
sync
umount /mnt/pedigree-esp
```

`EFI/BOOT/BOOTX64.EFI` contains the same GRUB binary as a fallback for firmware
that cannot retain an NVRAM entry. If `efibootmgr` reports unsupported EFI
variables, confirm Linux was booted in UEFI mode; the fallback path can still
be selected through firmware on implementations that support it. Remove the
rescue USB for the first SSD boot, or select the SSD explicitly in the boot
menu.

For an **existing GRUB2 UEFI installation**, copy only `EFI/PEDIGREE` into its
ESP and add the menu entries from the supplied `grub.cfg` to its maintained
custom configuration (usually `/etc/grub.d/40_custom`, preserving that file's
header), then regenerate the installed GRUB configuration from that Linux
installation. Do not overwrite its existing `EFI/BOOT/BOOTX64.EFI`. The crucial
entry is:

```grub
menuentry 'Pedigree (T420)' {
    insmod part_gpt
    insmod fat
    insmod search
    insmod search_fs_file
    insmod chain
    search --no-floppy --file --set=esp /EFI/PEDIGREE/current/BOOTX64.EFI
    chainloader ($esp)/EFI/PEDIGREE/current/BOOTX64.EFI current
    boot
}
```

Use the equivalent `known-good` paths and argument for the clean-initrd entry.
The `current`/`known-good` argument selects the matching artifact directory;
the kernel root option belongs in that directory's `cmdline` file. Keep the
loader, kernel, initrd, configuration database and command line together.

## First boot and later updates

Select the T420 entry. The initial credentials are `root` / `root`; change
them after installation. Verify the display and keyboard, then Ethernet link
and DHCP. If the NIC entry fails, retry the clean-initrd entry and keep the
failure details. QEMU's 82574L is different hardware from the T420's 82579LM.

For a storage check, create a small file under `/root`, run `sync`, shut down
cleanly, then use rescue Linux to run `e2fsck` on the unmounted root partition
and read the file. Repeat after a cold boot before relying on the SSD for
unique data.

For kernel updates, replace only the `current` artifact set with a matching
kernel/config/initrd/EFI set. Rebuild the external driver against that same
Pedigree build. UEFI needs the **uncompressed** initrd tar despite its `.tar`
name. Once a set works on the laptop, copy it to `known-good`. Keep this
backup when updating `current`. Do not `dd` the initial root image again over
an installed system: that would replace its files and filesystem metadata.

The `BUILD.txt`, `VALIDATION.txt` and `SHA256SUMS` in each bundle describe the
actual artifact versions and checks. Encumbered artifacts stay outside the
main Pedigree repository and its normal releases.

References: [resize2fs](https://man7.org/linux/man-pages/man8/resize2fs.8.html),
[GRUB EFI chainloader arguments](https://www.gnu.org/software/grub/manual/grub/html_node/chainloader.html).
