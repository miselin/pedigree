#!/usr/bin/env python3
"""Check aggregate instruction counts against a disk-free, fixed-sequence ROM."""

import argparse
from collections import Counter
import json
from pathlib import Path
import subprocess


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--plugin", type=Path, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--qemu", default="qemu-system-x86_64")
    parser.add_argument("--nasm", default="nasm")
    args = parser.parse_args()
    args.output.mkdir(parents=True, exist_ok=False)
    output = args.output.resolve()
    rom, profile = output / "qualification.bin", output / "profile.tsv"
    subprocess.run([args.nasm, "-f", "bin", str(Path(__file__).with_name("qualification.asm")),
                    "-o", str(rom)], check=True)
    command = [args.qemu, "-machine", "pc", "-accel", "tcg", "-smp", "1", "-m", "16",
               "-bios", str(rom), "-display", "none", "-serial", "none", "-monitor", "none",
               "-nic", "none", "-device", "isa-debug-exit,iobase=0xf4,iosize=2", "-plugin",
               f"{args.plugin.resolve()},start=0xf0040,stop=0xf0080,kernel-base=0xf0000,output={profile}"]
    result = subprocess.run(command, capture_output=True, timeout=15)
    (output / "qemu.stderr").write_bytes(result.stderr)
    assert result.returncode == 33, result.stderr.decode(errors="replace")
    rows = [line.split("\t", 4) for line in profile.read_text().splitlines()
            if not line.startswith("#")]
    assert rows[0] == ["B", "f0040", "0", "8000"]
    assert rows[-1] == ["E", "complete", "16", "0"]
    expected = Counter([0x40, 0x41, 0x44, 0x45, 0x44, 0x45, 0x44, 0x45,
                        0x47, 0x4F, 0x50, 0x4A, 0x51, 0x52, 0x4C, 0x4D])
    actual = {int(row[1], 16) - 0xF0000: int(row[2]) for row in rows if row[0] == "P"}
    assert actual == expected
    assert [row for row in rows if row[0] == "U"] == [["U", "0"]]
    assert sorted(row for row in rows if row[0] == "D") == [
        ["D", "1", "f004a", "f0051", "1"], ["D", "2", "f004a", "f004a", "1"]]
    original = profile.read_bytes()
    repeated = subprocess.run(command, capture_output=True, timeout=15)
    assert repeated.returncode != 33 and profile.read_bytes() == original
    report = {"result": "PASS", "command": command, "dispatches": 16,
              "instruction_variants": len(actual), "overwrite_refused": True,
              "qemu_version": subprocess.check_output([args.qemu, "--version"], text=True).splitlines()[0]}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(output / "report.json")


if __name__ == "__main__":
    main()
