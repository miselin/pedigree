"""Check COFF relocation offsets used by the freestanding EFI loader."""

import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest


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
                        output, entry = uefi_pe.read_object(path)
                    self.assertEqual(entry, 0)
                    self.assertEqual(struct.unpack_from("<i", output)[0], 12 + addend - kind)


if __name__ == "__main__":
    unittest.main()
