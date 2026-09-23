"""Create the UEFI ESP image used by QEMU."""

from __future__ import annotations

import argparse
import shutil
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
    candidates.extend(
        f"{directory}/{name}"
        for directory in ("/usr/sbin", "/sbin")
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
    run([args.mcopy, *mtools, str(args.kernel), f"{directory}/kernel"])
    run([args.mcopy, *mtools, str(cmdline), f"{directory}/cmdline"])
    if args.arch == "x64":
        run([args.mcopy, *mtools, str(args.efi), f"{directory}/BOOTX64.EFI"])
        run([args.mcopy, *mtools, str(args.initrd), f"{directory}/initrd.tar"])
        run([args.mcopy, *mtools, str(args.config), f"{directory}/config.db"])


def create_esp(path: Path, args: argparse.Namespace, root_uuid: str, temp_dir: Path) -> None:
    with path.open("wb") as image:
        image.truncate(64 * 1024 * 1024)
    run([args.mkfs, "-F", "32", "-n", "PEDIGREEESP", str(path)])
    mtools = ["-i", str(path)]
    run([args.mmd, *mtools, "::EFI"])
    run([args.mmd, *mtools, "::EFI/BOOT"])
    run([args.mmd, *mtools, "::EFI/PEDIGREE"])
    bootloader = args.grub if args.grub else args.efi
    boot_name = "BOOTAA64.EFI" if args.arch == "arm64" else "BOOTX64.EFI"
    run([args.mcopy, *mtools, str(bootloader), f"::EFI/BOOT/{boot_name}"])
    if args.grub:
        if not args.grub_config:
            raise ValueError("--grub-config is required with --grub")
        run([args.mcopy, *mtools, str(args.grub_config), "::EFI/PEDIGREE/grub.cfg"])
    cmdline = temp_dir / "cmdline"
    cmdline.write_text(f"root=UUID={root_uuid} splash=logs")
    install_variant(args, mtools, "current", cmdline)
    if args.arch == "x64":
        install_variant(args, mtools, "known-good", cmdline)


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--arch", choices=("x64", "arm64"), default="x64")
    parser.add_argument("--image", type=Path, required=True)
    parser.add_argument("--grub", type=Path)
    parser.add_argument("--grub-config", type=Path)
    parser.add_argument("--efi", type=Path, required=True)
    parser.add_argument("--kernel", type=Path, required=True)
    parser.add_argument("--initrd", type=Path)
    parser.add_argument("--config", type=Path)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--mkfs", default=find_tool("mkfs.fat", "mkfs.vfat"))
    parser.add_argument("--mmd", default=find_tool("mmd"))
    parser.add_argument("--mcopy", default=find_tool("mcopy"))
    args = parser.parse_args()
    if args.arch == "x64" and (not args.initrd or not args.config):
        parser.error("x64 requires --initrd and --config")
    if args.arch == "arm64" and args.grub:
        parser.error("--grub is only supported for x64")
    if not args.mkfs or not args.mmd or not args.mcopy:
        parser.error("mkfs.fat, mmd, and mcopy are required")

    args.image.parent.mkdir(parents=True, exist_ok=True)
    root_uuid = read_ext2_uuid(args.root)
    with tempfile.TemporaryDirectory(prefix="pedigree-uefi-", dir=args.image.parent) as temp_dir:
        temp_path = Path(temp_dir)
        esp = temp_path / "esp.img"
        create_esp(esp, args, root_uuid, temp_path)
        shutil.copyfile(esp, args.image)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
