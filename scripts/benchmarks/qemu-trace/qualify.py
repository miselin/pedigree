#!/usr/bin/env python3
"""Qualify the plugin with a disk-free ROM before interpreting kernel traces."""

import argparse
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
    rom = output / "qualification.bin"
    subprocess.run([
        args.nasm, "-f", "bin", str(Path(__file__).with_name("qualification.asm")),
        "-o", str(rom), "-l", str(output / "qualification.lst"),
    ], check=True)
    command = [
        args.qemu, "-machine", "pc", "-accel", "tcg", "-smp", "1", "-m", "16",
        "-bios", str(rom), "-display", "none", "-serial", "none", "-monitor", "none",
        "-nic", "none", "-device", "isa-debug-exit,iobase=0xf4,iosize=2",
    ]
    runs = []
    for name, limit in [("complete", 100), ("bounded", 5)]:
        trace = output / f"{name}.tsv"
        spec = (f"{args.plugin.resolve()},start=0xf0040,stop=0xf0080,skip=0,"
                f"count=1,limit={limit},output={trace}")
        invocation = command + ["-plugin", spec]
        result = subprocess.run(invocation, capture_output=True, timeout=15)
        (output / f"{name}.stderr").write_bytes(result.stderr)
        assert result.returncode == 33, result.stderr.decode(errors="replace")
        rows = [line.split("\t") for line in trace.read_text().splitlines()
                if not line.startswith("#")]
        insns = [row for row in rows if row[0] == "I"]
        expected = [0x40, 0x41, 0x44, 0x45, 0x44, 0x45, 0x44, 0x45,
                    0x47, 0x4F, 0x50, 0x4A, 0x51, 0x52, 0x4C, 0x4D]
        expected = [0xF0000 + pc for pc in expected][:limit]
        assert [int(row[3], 16) for row in insns] == expected
        assert [int(row[2]) for row in insns] == list(range(1, len(expected) + 1))
        ending = rows[-1]
        assert ending[:4] == ["E", "1", name if name == "complete" else "limit",
                              str(len(expected))]
        if name == "complete":
            assert ending[4:] == ["f0080", "3"]
            assert int(insns[9][4], 16) == 0x7FFE  # CALL pushed a return address.
            assert int(insns[12][4], 16) == 0x7FFA  # INT pushed FLAGS, CS, IP.
            events = [row for row in rows if row[0] == "D"]
            assert any(row[3:] == ["2", "f004a", "f004a"] for row in events)
            assert any(row[3:] == ["1", "f004a", "f0051"] for row in events)
        original = trace.read_bytes()
        repeat = subprocess.run(invocation, capture_output=True, timeout=15)
        assert repeat.returncode != 33 and trace.read_bytes() == original
        runs.append({"command": invocation, "exit": result.returncode,
                     "dispatches": len(insns), "status": ending[2],
                     "overwrite_refused": True})
    report = {"result": "PASS", "runs": runs, "qemu_version": subprocess.check_output(
        [args.qemu, "--version"], text=True).splitlines()[0]}
    (output / "report.json").write_text(json.dumps(report, indent=2) + "\n")
    print(json.dumps(report, indent=2))


if __name__ == "__main__":
    main()
