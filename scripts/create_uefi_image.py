#!/usr/bin/env python3
"""Create the UEFI ESP and partitioned disk image used by QEMU."""

from __future__ import annotations

import argparse
import shutil
import struct
import subprocess
import tempfile
from pathlib import Path


def run(command: list[str]) -> None:
    subprocess.run(command, check=True)


def read_ext2_uuid(path: Path) -> str:
    with path.open("rb") as image:
        image.seek(1024 + 104)
        value = image.read(16)
    if len(value) != 16:
        raise ValueError("could not read the ext2 filesystem UUID")
    return "{}{}{}{}-{}{}-{}{}-{}{}-{}{}{}{}{}{}".format(
        *(f"{byte:02x}" for byte in value)
    )


def find_tool(*names: str) -> str | None:
    candidates = [shutil.which(name) for name in names]
    candidates.extend(
        f"/opt/homebrew/{directory}/{name}"
        for directory in ("bin", "sbin")
        for name in names
    )
    return next((candidate for candidate in candidates if candidate and Path(candidate).is_file()), None)


def install_variant(
    args: argparse.Namespace,
    mtools: list[str],
    variant: str,
    cmdline: Path,
) -> None:
    directory = f"::EFI/PEDIGREE/{variant}"
    run([args.mmd, *mtools, directory])
    run([args.mcopy, *mtools, str(args.efi), f"{directory}/BOOTX64.EFI"])
    run([args.mcopy, *mtools, str(args.kernel), f"{directory}/kernel"])
    run([args.mcopy, *mtools, str(args.initrd), f"{directory}/initrd.tar"])
    run([args.mcopy, *mtools, str(args.config), f"{directory}/config.db"])
    run([args.mcopy, *mtools, str(cmdline), f"{directory}/cmdline"])


def create_esp(path: Path, args: argparse.Namespace, root_uuid: str, temp_dir: Path) -> None:
    with path.open("wb") as image:
        image.truncate(64 * 1024 * 1024)
    run([args.mkfs, "-F", "32", "-n", "PEDIGREEESP", str(path)])
    mtools = ["-i", str(path)]
    run([args.mmd, *mtools, "::EFI"])
    run([args.mmd, *mtools, "::EFI/BOOT"])
    run([args.mmd, *mtools, "::EFI/PEDIGREE"])
    bootloader = args.grub if args.grub else args.efi
    run([args.mcopy, *mtools, str(bootloader), "::EFI/BOOT/BOOTX64.EFI"])
    cmdline = temp_dir / "cmdline"
    cmdline.write_text(f"root=UUID={root_uuid}")
    install_variant(args, mtools, "current", cmdline)
    install_variant(args, mtools, "known-good", cmdline)


def partition_entry(bootable: bool, partition_type: int, start: int, length: int) -> bytes:
    entry = bytearray(16)
    entry[0] = 0x80 if bootable else 0
    entry[1:4] = b"\xff\xff\xff"
    entry[4] = partition_type
    entry[5:8] = b"\xff\xff\xff"
    struct.pack_into("<II", entry, 8, start, length)
    return bytes(entry)


def create_partitioned_image(path: Path, esp: Path, root: Path) -> None:
    sector_size = 512
    esp_offset = 1 * 1024 * 1024
    esp_size = esp.stat().st_size
    root_offset = esp_offset + esp_size
    root_filesystem_offset = 0
    root_size = root.stat().st_size - root_filesystem_offset
    if root_size <= 0 or root_filesystem_offset % sector_size:
        raise ValueError("root image does not contain a valid ext2 partition offset")

    esp_start = esp_offset // sector_size
    root_start = (root_offset + root_filesystem_offset) // sector_size
    image_size = root_offset + root.stat().st_size
    with path.open("wb") as image:
        image.truncate(image_size)
        image.seek(446)
        # Put root first so mountroot can establish the root view before any
        # auxiliary filesystem is registered with the VFS.
        image.write(partition_entry(False, 0x83, root_start, root_size // sector_size))
        image.write(partition_entry(True, 0xEF, esp_start, esp_size // sector_size))
        image.seek(510)
        image.write(b"\x55\xaa")
        image.seek(esp_offset)
        with esp.open("rb") as source:
            shutil.copyfileobj(source, image)
        image.seek(root_offset)
        with root.open("rb") as source:
            shutil.copyfileobj(source, image)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--grub", type=Path)
    parser.add_argument("--efi", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--initrd", type=Path, required=True)
    parser.add_argument("--config", type=Path, required=True)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--mkfs", default=find_tool("mkfs.fat", "mkfs.vfat"))
    parser.add_argument("--mmd", default=find_tool("mmd"))
    parser.add_argument("--mcopy", default=find_tool("mcopy"))
    args = parser.parse_args()
    if not args.mkfs or not args.mmd or not args.mcopy:
        parser.error("mkfs.fat, mmd, and mcopy are required")

    args.image.parent.mkdir(parents=True, exist_ok=True)
    root_uuid = read_ext2_uuid(args.root)
    with tempfile.TemporaryDirectory(prefix="pedigree-uefi-", dir=args.image.parent) as temp_dir:
        temp_path = Path(temp_dir)
        esp = temp_path / "esp.img"
        create_esp(esp, args, root_uuid, temp_path)
        create_partitioned_image(args.image, esp, args.root)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
