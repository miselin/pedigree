#!/usr/bin/env python3
"""Build a standalone UEFI GRUB image with the Pedigree menu embedded."""

from __future__ import annotations

import argparse
import subprocess
from pathlib import Path


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--grub-mkstandalone", required=True, type=Path)
    parser.add_argument("--config", required=True, type=Path)
    parser.add_argument("--output", required=True, type=Path)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    subprocess.run(
        [
            str(args.grub_mkstandalone),
            "-O",
            "x86_64-efi",
            "-o",
            str(args.output),
            "--modules",
            "part_gpt part_msdos fat search search_fs_file chain normal",
            f"boot/grub/grub.cfg={args.config}",
        ],
        check=True,
    )
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
