#!/usr/bin/env python3
"""Wrap the freestanding UEFI COFF object in a small PE32+ image."""

from __future__ import annotations

import struct
import sys
from pathlib import Path


COFF_SECTION = 40
COFF_SYMBOL = 18
IMAGE_BASE = 0x800000
FILE_ALIGNMENT = 0x200
SECTION_ALIGNMENT = 0x1000


def align(value: int, alignment: int) -> int:
    return (value + alignment - 1) & ~(alignment - 1)


def read_object(path: Path) -> tuple[bytearray, int]:
    data = bytearray(path.read_bytes())
    machine, count, _, symbol_offset, symbol_count, optional_size, _ = struct.unpack_from(
        "<HHIIIHH", data, 0
    )
    if machine != 0x8664 or optional_size:
        raise ValueError("UEFI loader must be an AMD64 COFF object")

    sections = []
    cursor = 20
    for index in range(count):
        raw = struct.unpack_from("<8sIIIIIIHHI", data, cursor)
        name, virtual_size, _, raw_size, raw_offset, reloc_offset, _, reloc_count, _, chars = raw
        sections.append(
            {
                "index": index + 1,
                "name": name.rstrip(b"\0").decode("ascii"),
                "data": bytes(data[raw_offset : raw_offset + raw_size]),
                "raw_offset": raw_offset,
                "reloc_offset": reloc_offset,
                "reloc_count": reloc_count,
                "virtual_size": max(virtual_size, raw_size),
                "chars": chars,
            }
        )
        cursor += COFF_SECTION

    symbols = []
    symbol_cursor = symbol_offset
    symbol_index = 0
    while symbol_index < symbol_count:
        name_bytes, value, section, typ, storage, aux = struct.unpack_from(
            "<8sIhHBB", data, symbol_cursor
        )
        if name_bytes[:4] == b"\0\0\0\0":
            string_offset = struct.unpack_from("<I", name_bytes, 4)[0]
            string_end = data.find(b"\0", symbol_offset + symbol_count * COFF_SYMBOL + string_offset)
            name = data[ symbol_offset + symbol_count * COFF_SYMBOL + string_offset : string_end ].decode(
                "ascii"
            )
        else:
            name = name_bytes.rstrip(b"\0").decode("ascii")
        symbols.append({"name": name, "value": value, "section": section})
        symbols.extend({"name": "", "value": 0, "section": 0} for _ in range(aux))
        symbol_cursor += COFF_SYMBOL * (1 + aux)
        symbol_index += 1 + aux

    output = bytearray()
    section_bases = {}
    for section in sections:
        if section["name"].startswith(".debug") or section["name"] in (".pdata", ".xdata"):
            continue
        section_bases[section["index"]] = len(output)
        output.extend(section["data"])
        output.extend(b"\0" * (align(len(output), 16) - len(output)))

    symbol_addresses = {}
    for symbol in symbols:
        if symbol["section"] > 0 and symbol["section"] in section_bases:
            symbol_addresses[symbol["name"]] = section_bases[symbol["section"]] + symbol["value"]

    relocations: list[int] = []
    for section in sections:
        if section["index"] not in section_bases:
            continue
        base = section_bases[section["index"]]
        for i in range(section["reloc_count"]):
            offset, symbol_index, kind = struct.unpack_from(
                "<IIH", data, section["reloc_offset"] + i * 10
            )
            symbol_record = symbols[symbol_index]
            symbol = symbol_record["name"]
            if symbol_record["section"] in section_bases:
                target = section_bases[symbol_record["section"]] + symbol_record["value"]
            elif symbol in symbol_addresses:
                target = symbol_addresses[symbol]
            else:
                raise ValueError(f"unresolved UEFI loader symbol: {symbol}")
            place = base + offset
            addend = struct.unpack_from("<q", output, place)[0] if kind == 1 else 0
            if kind == 1:  # IMAGE_REL_AMD64_ADDR64
                struct.pack_into("<Q", output, place, IMAGE_BASE + target + addend)
                relocations.append(place)
            elif 4 <= kind <= 9:  # IMAGE_REL_AMD64_REL32 through REL32_5
                variant = kind - 4
                displacement = IMAGE_BASE + target + addend - (IMAGE_BASE + place + 4 + variant)
                struct.pack_into("<i", output, place, displacement)
            else:
                raise ValueError(f"unsupported AMD64 COFF relocation type {kind} for {symbol}")

    entry_offset = symbol_addresses.get("efi_main")
    if entry_offset is None:
        raise ValueError("UEFI loader does not export efi_main")
    return output, entry_offset


def build_pe(code: bytearray, entry_offset: int, output_path: Path) -> None:
    reloc = bytearray()
    # The loader is built RIP-relative, so this is normally empty. Keep the
    # directory absent until an absolute relocation is actually emitted.
    text_rva = SECTION_ALIGNMENT
    reloc_rva = align(text_rva + len(code), SECTION_ALIGNMENT)
    section_count = 1
    headers_size = align(0x80 + 4 + 20 + 240 + section_count * 40, FILE_ALIGNMENT)
    raw_size = align(len(code), FILE_ALIGNMENT)
    image_size = align(text_rva + len(code), SECTION_ALIGNMENT)
    entry_rva = text_rva + entry_offset

    dos = bytearray(0x80)
    struct.pack_into("<2sI", dos, 0, b"MZ", 0)
    struct.pack_into("<I", dos, 0x3C, 0x80)
    nt = bytearray()
    nt.extend(b"PE\0\0")
    nt.extend(struct.pack("<HHIIIHH", 0x8664, section_count, 0, 0, 0, 240, 0x0022))
    optional = bytearray(240)
    struct.pack_into("<HBBIII", optional, 0, 0x20B, 14, 0, raw_size, 0, 0)
    struct.pack_into("<III", optional, 4, raw_size, 0, 0)
    struct.pack_into("<IIQII", optional, 16, entry_rva, text_rva, IMAGE_BASE,
                     SECTION_ALIGNMENT, FILE_ALIGNMENT)
    struct.pack_into("<I", optional, 56, image_size)
    struct.pack_into("<I", optional, 60, headers_size)
    struct.pack_into("<H", optional, 68, 10)
    struct.pack_into("<I", optional, 72, 0x100000)
    struct.pack_into("<I", optional, 80, 0x100000)
    struct.pack_into("<I", optional, 88, 0x100000)
    struct.pack_into("<I", optional, 108, 16)
    nt.extend(optional)
    section = bytearray(40)
    struct.pack_into("<8sIIIIIIHHI", section, 0, b".text\0\0\0", len(code), text_rva, raw_size,
                     headers_size, 0, 0, 0, 0, 0xE0000020)
    nt.extend(section)

    image = dos + nt
    image.extend(b"\0" * (headers_size - len(image)))
    image.extend(code)
    image.extend(b"\0" * (raw_size - len(code)))
    output_path.write_bytes(image)


def main() -> int:
    if len(sys.argv) != 3:
        print("usage: uefi_pe.py LOADER.obj BOOTX64.EFI", file=sys.stderr)
        return 2
    code, entry = read_object(Path(sys.argv[1]))
    build_pe(code, entry, Path(sys.argv[2]))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
