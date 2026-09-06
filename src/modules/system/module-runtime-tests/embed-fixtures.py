#!/usr/bin/env python3
"""Embed the three compiled runtime-module regression fixtures."""
import argparse
from pathlib import Path

parser = argparse.ArgumentParser(description=__doc__)
parser.add_argument("--provider", type=Path, required=True)
parser.add_argument("--consumer", type=Path, required=True)
parser.add_argument("--undeclared", type=Path, required=True)
parser.add_argument("--output", type=Path, required=True)
args = parser.parse_args()
lines = [
    '// Generated runtime-module regression fixtures.',
    '#include "pedigree/kernel/processor/types.h"',
    'extern "C" {',
]
for kind in ("provider", "consumer", "undeclared"):
    data = getattr(args, kind).read_bytes()
    if not data or len(data) > 1024 * 1024:
        parser.error(f"{kind} fixture must contain between 1 byte and 1 MiB")
    name = f"runtime_{kind}_image"
    lines.append(f"alignas(16) extern const uint8_t {name}[] = {{")
    for offset in range(0, len(data), 16):
        lines.append("  " + ", ".join(f"0x{byte:02x}" for byte in data[offset:offset + 16]) + ",")
    lines += ["};", f"extern const size_t {name}_size = sizeof({name});"]
lines += ["}", ""]
args.output.write_text("\n".join(lines))
