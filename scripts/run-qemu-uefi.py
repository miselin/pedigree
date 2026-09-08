#!/usr/bin/env python3
"""Run a bounded native-UEFI Pedigree boot checkpoint under QEMU."""

from __future__ import annotations

import argparse
import os
import shlex
import signal
import shutil
import subprocess
import time
from pathlib import Path


FAILURE_MARKERS = ("panic:", "fatal:", "page fault exception", "triple fault")


def find_ovmf() -> Path | None:
    candidates = [
        os.environ.get("OVMF_CODE"),
        "/usr/share/OVMF/OVMF_CODE.fd",
        "/opt/homebrew/share/qemu/edk2-x86_64-code.fd",
    ]
    paths = [Path(candidate) for candidate in candidates if candidate]
    paths.extend(Path("/opt/homebrew/Cellar/qemu").glob("*/share/qemu/edk2-x86_64-code.fd"))
    return next((path for path in paths if path.is_file()), None)


def stop(process: subprocess.Popen[bytes]) -> None:
    if process.poll() is not None:
        return
    try:
        os.killpg(process.pid, signal.SIGTERM)
        process.wait(timeout=3)
    except (ProcessLookupError, subprocess.TimeoutExpired):
        if process.poll() is None:
            os.killpg(process.pid, signal.SIGKILL)
            process.wait(timeout=3)


def main() -> int:
    repository = Path(__file__).resolve().parent.parent
    parser = argparse.ArgumentParser()
    parser.add_argument("--image", type=Path, default=repository / "build/pedigree-uefi.img")
    parser.add_argument("--log-dir", type=Path, default=repository / "build/qemu-uefi-checkpoint")
    parser.add_argument("--seconds", type=float, default=45)
    parser.add_argument("--qemu", default=os.environ.get("QEMU", "qemu-system-x86_64"))
    parser.add_argument("--ovmf", type=Path, default=find_ovmf())
    parser.add_argument(
        "--require-marker", action="append", default=["BootIO is initialized!", "Archive: mapped to"]
    )
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        assert "file=/tmp/pedigree.img,if=ide,format=raw,snapshot=on" in build_command(
            "qemu", Path("/tmp/pedigree.img"), Path("/tmp/serial.log"), Path("/tmp/ovmf.fd")
        )
        return 0
    if not args.image.is_file():
        print(f"UEFI image is unavailable: {args.image}")
        return 2
    if not args.ovmf or not args.ovmf.is_file():
        print("OVMF_CODE is unavailable; pass --ovmf or set OVMF_CODE")
        return 2

    args.log_dir.mkdir(parents=True, exist_ok=True)
    serial_log = args.log_dir / "serial.log"
    output_log = args.log_dir / "qemu.log"
    ovmf_copy = args.log_dir / "OVMF_CODE.fd"
    shutil.copyfile(args.ovmf, ovmf_copy)
    serial_log.write_text("")
    command = build_command(args.qemu, args.image, serial_log, ovmf_copy)
    print(f"QEMU command: {shlex.join(command)}")
    print(f"Serial log: {serial_log}")
    with output_log.open("w", encoding="utf-8") as output:
        try:
            process = subprocess.Popen(
                command,
                stdin=subprocess.PIPE,
                stdout=output,
                stderr=subprocess.STDOUT,
                start_new_session=True,
            )
        except FileNotFoundError:
            print("QEMU is unavailable")
            return 127
        deadline = time.monotonic() + args.seconds
        while time.monotonic() < deadline:
            text = serial_log.read_text(encoding="utf-8", errors="replace")
            lowered = text.casefold()
            failure = next((marker for marker in FAILURE_MARKERS if marker in lowered), None)
            if failure:
                stop(process)
                print(f"QEMU-UEFI-CHECKPOINT: FAIL reason={failure}")
                return 1
            missing = [marker for marker in args.require_marker if marker not in text]
            if not missing:
                if process.stdin is not None:
                    process.stdin.write(b"quit\n")
                    process.stdin.flush()
                process.wait(timeout=5)
                print("QEMU-UEFI-CHECKPOINT: PASS")
                return 0
            if process.poll() is not None:
                print(f"QEMU-UEFI-CHECKPOINT: FAIL early-exit missing={','.join(missing)}")
                return 1
            time.sleep(0.05)
        text = serial_log.read_text(encoding="utf-8", errors="replace")
        missing = [marker for marker in args.require_marker if marker not in text]
        stop(process)
        print(f"QEMU-UEFI-CHECKPOINT: FAIL timeout missing={','.join(missing)}")
        return 1


def build_command(qemu: str, image: Path, serial_log: Path, ovmf: Path) -> list[str]:
    return [
        qemu,
        "-machine", "q35",
        "-smp", "1",
        "-m", "512",
        "-drive", f"if=pflash,format=raw,file={ovmf}",
        "-drive", f"file={image},if=ide,format=raw,snapshot=on",
        "-display", "none",
        "-monitor", "stdio",
        "-serial", f"file:{serial_log}",
        "-nic", "none",
        "-no-reboot",
        "-no-shutdown",
    ]


if __name__ == "__main__":
    raise SystemExit(main())
