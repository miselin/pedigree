"""Check COFF relocation offsets used by the freestanding EFI loader."""

import importlib.util
import struct
import tempfile
import unittest
from pathlib import Path

spec = importlib.util.spec_from_file_location(
    "uefi_pe", Path(__file__).resolve().parents[1] / "scripts/uefi_pe.py"
)
uefi_pe = importlib.util.module_from_spec(spec)
spec.loader.exec_module(uefi_pe)


class RelocationTests(unittest.TestCase):
    def test_relative_addends(self):
        for kind in range(4, 10):
            for addend in (-7, 0, 364):
                with self.subTest(kind=kind, addend=addend):
                    # One section, a relocation at offset zero and a target at 12.
                    raw_offset = 60
                    reloc_offset = raw_offset + 16
                    symbol_offset = reloc_offset + 10
                    header = struct.pack("<HHIIIHH", 0x8664, 1, 0, symbol_offset, 2, 0, 0)
                    section = struct.pack(
                        "<8sIIIIIIHHI", b".text", 0, 0, 16, raw_offset,
                        reloc_offset, 0, 1, 0, 0x60000020
                    )
                    code = struct.pack("<i", addend) + bytes(12)
                    relocation = struct.pack("<IIH", 0, 1, kind)
                    symbols = struct.pack("<8sIhHBB", b"efi_main", 0, 1, 0, 2, 0)
                    symbols += struct.pack("<8sIhHBB", b"target", 12, 1, 0, 3, 0)
                    with tempfile.TemporaryDirectory() as directory:
                        path = Path(directory) / "loader.obj"
                        path.write_bytes(header + section + code + relocation + symbols)
                        output, entry, machine, relocations = uefi_pe.read_object(path)
                    self.assertEqual(entry, 0)
                    self.assertEqual(machine, 0x8664)
                    self.assertEqual(relocations, [])
                    self.assertEqual(struct.unpack_from("<i", output)[0], 12 + addend - kind)

    def test_arm64_instruction_relocations(self):
        code = bytearray(struct.pack("<IIII", 0x90000000, 0x91000000, 0xF9400000, 0x94000000))
        uefi_pe.arm64_relocation(code, 0, 0x1234, 4)
        uefi_pe.arm64_relocation(code, 4, 0x1234, 6)
        uefi_pe.arm64_relocation(code, 8, 0x1238, 7)
        uefi_pe.arm64_relocation(code, 12, 24, 3)
        adrp, add, load, branch = struct.unpack("<IIII", code)
        self.assertEqual((adrp >> 29) & 3, 1)
        self.assertEqual((add >> 10) & 0xFFF, 0x234)
        self.assertEqual((load >> 10) & 0xFFF, 0x238 // 8)
        self.assertEqual(branch & 0x03FFFFFF, 3)

    def test_arm32_thumb_address_relocation(self):
        code = bytearray(struct.pack("<HHHH", 0xF240, 0x0000, 0xF2C0, 0x0000))
        kind = uefi_pe.arm32_relocation(code, 0, 0x1234, 0x11, True)
        self.assertEqual(kind, 7)
        self.assertEqual(struct.unpack("<HHHH", code), (0xF242, 0x2035, 0xF2C0, 0x0080))


if __name__ == "__main__":
    unittest.main()
