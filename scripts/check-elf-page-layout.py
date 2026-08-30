#!/usr/bin/env python3

"""Validate that ELF load segments are safe for a target base-page size."""

from __future__ import annotations

import argparse
import struct
from dataclasses import dataclass
from pathlib import Path


PT_LOAD = 1


@dataclass(frozen=True)
class LoadSegment:
    offset: int
    address: int
    memory_size: int
    flags: int
    alignment: int


def load_segments(path: Path) -> list[LoadSegment]:
    data = path.read_bytes()
    if data[:4] != b"\x7fELF":
        raise ValueError("not an ELF file")

    elf_class = data[4]
    byte_order = data[5]
    if byte_order == 1:
        prefix = "<"
    elif byte_order == 2:
        prefix = ">"
    else:
        raise ValueError("unsupported ELF byte order")

    if elf_class == 2:
        header = struct.unpack_from(prefix + "16sHHIQQQIHHHHHH", data)
        program_offset, entry_size, entry_count = header[5], header[9], header[10]
        program_format = prefix + "IIQQQQQQ"
    elif elf_class == 1:
        header = struct.unpack_from(prefix + "16sHHIIIIIHHHHHH", data)
        program_offset, entry_size, entry_count = header[5], header[9], header[10]
        program_format = prefix + "IIIIIIII"
    else:
        raise ValueError("unsupported ELF class")

    expected_entry_size = struct.calcsize(program_format)
    if entry_size < expected_entry_size:
        raise ValueError("truncated ELF program-header entries")
    if program_offset + (entry_size * entry_count) > len(data):
        raise ValueError("program-header table extends beyond the file")

    result: list[LoadSegment] = []
    for index in range(entry_count):
        fields = struct.unpack_from(
            program_format, data, program_offset + (index * entry_size)
        )
        if fields[0] != PT_LOAD:
            continue
        if elf_class == 2:
            _, flags, offset, address, _, _, memory_size, alignment = fields
        else:
            _, offset, address, _, _, memory_size, flags, alignment = fields
        if memory_size:
            result.append(
                LoadSegment(offset, address, memory_size, flags, alignment)
            )
    return result


def validate(path: Path, page_size: int) -> list[str]:
    failures: list[str] = []
    try:
        segments = load_segments(path)
    except (IndexError, OSError, struct.error, ValueError) as error:
        return [f"{path}: {error}"]

    if not segments:
        return [f"{path}: no nonempty PT_LOAD segments"]

    page_mask = page_size - 1
    page_ranges: list[tuple[int, int, LoadSegment]] = []
    for segment in segments:
        if segment.alignment < page_size or segment.alignment % page_size:
            failures.append(
                f"{path}: PT_LOAD alignment 0x{segment.alignment:x} does not "
                f"honor the {page_size}-byte target page"
            )
        if (segment.address - segment.offset) & page_mask:
            failures.append(
                f"{path}: PT_LOAD address 0x{segment.address:x} and offset "
                f"0x{segment.offset:x} are not target-page congruent"
            )
        end = segment.address + segment.memory_size
        if end < segment.address:
            failures.append(f"{path}: PT_LOAD address range overflows")
            continue
        page_ranges.append(
            (
                segment.address & ~page_mask,
                (end + page_mask) & ~page_mask,
                segment,
            )
        )

    for index, (start, end, segment) in enumerate(page_ranges):
        for other_start, other_end, other in page_ranges[index + 1 :]:
            if max(start, other_start) >= min(end, other_end):
                continue
            if segment.flags != other.flags:
                failures.append(
                    f"{path}: differently protected PT_LOAD segments share "
                    f"target page 0x{max(start, other_start):x}"
                )
    return failures


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("--page-size", required=True, type=int)
    parser.add_argument("artifacts", nargs="+", type=Path)
    arguments = parser.parse_args()
    page_size = arguments.page_size
    if page_size <= 0 or page_size & (page_size - 1):
        parser.error("--page-size must be a positive power of two")

    failures = [
        failure
        for artifact in arguments.artifacts
        for failure in validate(artifact, page_size)
    ]
    if failures:
        print("\n".join(failures))
        return 1

    print(f"ELF target-page layout passed for {len(arguments.artifacts)} artifact(s).")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
