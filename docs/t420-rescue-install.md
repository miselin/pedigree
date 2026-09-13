# Install Pedigree from rescue Linux

The install bundle contains a compressed, raw 4 GiB ext2 root filesystem and
an `esp/` directory to copy onto a FAT32 EFI System Partition. It boots through
GRUB2 into Pedigree's EFI loader. The install kernel has disk writes enabled.

The T420 needs a unique random seed generated from rescue Linux using the
step below. Its Sandy Bridge CPU lacks RDRAND/RDSEED; the kernel's software
random generator uses this seed for Python, TLS, SSH and other consumers.
No seed or private key is included in the shared image.

The UEFI text console uses the firmware's GOP framebuffer for login and shell
output. It supports linear 32-bit RGB/BGR modes at least 640×400, including
equivalent RGB bitmask modes. It retains the firmware's resolution and scales
an 80×25 console to fit. Firmware without a supported framebuffer can still
boot to serial login; the Ethernet driver alone does not provide remote login.

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
disks need separate layout planning: preserve their data and ESP. This erase
recipe does not apply to them.

```sh
DISK=/dev/sda
ROOT=/dev/sda1
ESP=/dev/sda2
lsblk -o NAME,SIZE,MODEL,SERIAL,FSTYPE,MOUNTPOINTS "$DISK"
test "$(blockdev --getss "$DISK")" = 512
```

Unmount all target partitions and disable any swap on that SSD before
continuing. The root partition will be **partition 1**, and the ESP will be
**partition 2**, although the ESP sits first physically. These numbers match
the commands below; other layouts can use either entry order. The SSD must
have more than 4.5 GiB available.

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
test "$(blockdev --getsize64 "$ROOT")" -ge 4294967296

gzip -dc rootfs.ext2.gz | dd of="$ROOT" bs=4M status=progress conv=fsync
e2fsck -f "$ROOT"
resize2fs "$ROOT"
e2fsck -f "$ROOT"
dumpe2fs -h "$ROOT"

mkdir -p /mnt/pedigree-root
mount "$ROOT" /mnt/pedigree-root
install -d -o 0 -g 0 -m 700 /mnt/pedigree-root/var/lib/pedigree
umask 077
dd if=/dev/random of=/mnt/pedigree-root/var/lib/pedigree/random-seed \
  bs=32 count=1 iflag=fullblock status=none
chmod 600 /mnt/pedigree-root/var/lib/pedigree/random-seed
chown 0:0 /mnt/pedigree-root/var/lib/pedigree/random-seed
sync
umount /mnt/pedigree-root

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

Arguments after the directory selector are appended to its `cmdline` contents.
For example, `chainloader ($esp)/EFI/PEDIGREE/current/BOOTX64.EFI current intelgfx=off`
retains the saved root UUID and other options while disabling the Intel graphics
module. Use `current intelgfx=on` to allow that module to probe when it is included
in the initrd. Keep the graphics toggle in the GRUB menu only: appending `on` does
not remove an existing `intelgfx=off` from the saved command line. A selector
without additional arguments keeps the saved command line unchanged.

The loader accepts printable ASCII options or their UTF-16LE representation,
normalizes tabs to spaces, and rejects malformed or oversized inputs. Options
are limited to 1023 decoded characters; the combined kernel command line is
limited to 4095 characters.

## Included tools and source

The filesystem includes curl, Git, Dropbear (`ssh`, `scp` and server tools),
PUP, nano, Vim, less, ripgrep (`rg`), Make, CMake, GCC, binutils, NASM,
diffutils, gawk, grep, sed, gzip and bsdtar, together with their package
dependencies. HTTPS clients use the CA bundle at `/etc/ssl/cert.pem`.
`/etc/resolv.conf` points to `/proc/resolv.conf`, which reflects DHCP's DNS
servers. Wait for Ethernet link and DHCP before using network tools.
PUP's configuration is `/etc/pup/pup.conf`; its catalog and cache live under
`/var/lib/pup`. Once networking works, refresh the catalog with `pup sync`
before installing more packages with `pup install PACKAGE`.

Source checkouts are seeded under `/root/src`:

| Checkout | Branch |
| --- | --- |
| `/root/src/pedigree` | `develop` |
| `/root/src/pedigree-encumbered` | `main` |

The Pedigree checkout includes the local changes used to build this bundle
beyond the fetched `origin/develop`. Exact revisions are recorded in
`BUILD.txt` and can be read with `git -C /root/src/pedigree rev-parse HEAD`.
These are shallow, self-contained checkouts with their normal origin URLs;
they do not depend on another machine's Git object storage. Access to the
private encumbered repository still requires your own credentials. The
`external/googletest` submodule is included for the seeded Pedigree revision.
Check `BUILD.txt` for the exact source and package inventory in your bundle.

The installed compilers and source provide a starting point for experiments.
Their presence does not qualify a complete native Pedigree build, compiler
bootstrap or full test suite. `VALIDATION.txt` records the version checks and
small runtime tests actually completed for the bundle.

## Enable SSH access

SSH startup is gated on a nonempty `/root/.ssh/authorized_keys`. No host keys
or user private keys are supplied. The startup script generates this laptop's
host key when first enabled and allows public-key authentication only. The
console's initial `root` / `root` login is not an SSH password credential.
This setup requires the rescue-generated seed above. Startup advances the
saved seed durably before enabling secure randomness. `random-seed --check`
reports readiness. If startup finds an interrupted `.next` transaction,
recover from rescue Linux using the included `RANDOMNESS.md` instructions.

To authorize your existing public key from rescue Linux, mount the installed
root filesystem after its filesystem checks have completed:

```sh
mkdir -p /mnt/pedigree-root
mount "$ROOT" /mnt/pedigree-root
install -d -m 700 /mnt/pedigree-root/root/.ssh
install -m 600 /path/to/your/id_ed25519.pub \
  /mnt/pedigree-root/root/.ssh/authorized_keys
chown -R 0:0 /mnt/pedigree-root/root/.ssh
sync
umount /mnt/pedigree-root
```

Copy only the public `.pub` file. SSH will start on the next Pedigree boot.
Alternatively, enable it from the Pedigree console by pasting your public key
as one complete line in the file:

```sh
mkdir -p /root/.ssh
chmod 700 /root/.ssh
nano /root/.ssh/authorized_keys
chmod 600 /root/.ssh/authorized_keys
/etc/init.d/40_dropbear.sh
```

Connect from your usual computer with `ssh root@LAPTOP_IP`. For file transfers
from an OpenSSH client, use `scp -O FILE root@LAPTOP_IP:/root/`; the installed
Dropbear package supports the SCP protocol but does not include an SFTP
server. Keep the generated files in `/etc/dropbear` across later updates.

On Pedigree, `/usr/bin/ssh` points to `dbclient`. Git is configured with
`core.sshCommand=/usr/bin/dbclient` and `ssh.variant=simple` so it does not send
OpenSSH-specific options. Ordinary `git@host:path` remote URLs use this setup.
Git's simple variant does not accept explicit ports in SSH URLs or Git's
`-4`/`-6` switches; a custom port can instead be placed in `core.sshCommand`.
Outgoing SSH authentication needs your own credentials; the bundle supplies
none.

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
