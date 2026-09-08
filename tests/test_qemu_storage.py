# SPDX-License-Identifier: ISC
import argparse
import importlib.util
from pathlib import Path
import struct
import tempfile
import unittest
import zlib

SPEC = importlib.util.spec_from_file_location("storage_smoke", Path(__file__).resolve().parents[1] / "scripts/test_qemu_storage.py")
smoke = importlib.util.module_from_spec(SPEC)
SPEC.loader.exec_module(smoke)


class StorageFixtures(unittest.TestCase):
    def test_trace_evidence(self):
        smoke.verify_nvme_flushes("pci_nvme_flush_ns nsid 0x7\npci_nvme_flush_ns nsid 0x17\n")
        with self.assertRaises(RuntimeError):
            smoke.verify_nvme_flushes("pci_nvme_flush_ns nsid 0x7\n")
        trace = ("process_ncq_command ahci(0x1)[1][tag:2]: NCQ\n"
                 "process_ncq_command ahci(0x1)[1][tag:3]: NCQ\n"
                 "ncq_finish ahci(0x1)[1][tag:2]: done\n"
                 "ncq_finish ahci(0x1)[1][tag:3]: done\n")
        self.assertEqual(smoke.ncq_maximum(trace), 2)
        with self.assertRaises(RuntimeError):
            smoke.ncq_maximum(trace.splitlines()[0] + "\n" + trace)

    def test_nvme_header_and_write_boundaries(self):
        for sector in (512, 4096):
            header = smoke.nvme_chunk(0, 4096, sector)
            self.assertEqual(header[:22], b"PEDIGREE-NVME-SMOKE-v1")
            self.assertEqual(struct.unpack_from("<QI", header, 40), (smoke.DISK_SIZE, sector))
            for start, length in ((8 * 1024 * 1024, 65536), (16 * 1024 * 1024, 4096),
                                  (smoke.DISK_SIZE - sector, sector)):
                self.assertEqual(smoke.nvme_chunk(start - 1, 1, sector, True), smoke.ahci.pattern(start - 1, 1, 0x5a))
                self.assertEqual(smoke.nvme_chunk(start, length, sector, True), smoke.ahci.pattern(start, length, 0xa5))

    def test_commands_keep_root_snapshots_and_sparse_native_namespaces(self):
        for root in ("nvme", "ahci"):
            args = argparse.Namespace(root=root, qemu="qemu", cpus=4, ovmf=Path("/ovmf"),
                                      image=Path("/root.img"), ahci_sector_size=4096)
            command = smoke.command(args, Path("/tmp/test"))
            self.assertIn("q35,i8042=off", command)
            self.assertIn("4", command)
            for nsid, sector in ((7, 512), (23, 4096)):
                self.assertTrue(any(f"nsid={nsid},shared=off,logical_block_size={sector}" in item for item in command))
            root_drive = next(item for item in command if "if=none,id=root," in item)
            self.assertIn("snapshot=on", root_drive)

    def test_4kn_gpt_root_wrap_preserves_payload_and_boot_uuid_source(self):
        with tempfile.TemporaryDirectory() as directory:
            source, root, boot = (Path(directory) / name for name in ("source", "root", "boot"))
            image = bytearray(1024 * 1024)
            image[450], image[466] = 0x83, 0xef
            struct.pack_into("<II", image, 454, 128, 16)
            struct.pack_into("<II", image, 470, 8, 32)
            image[510:512] = b"\x55\xaa"
            image[65536:73728] = b"r" * 8192
            image[4096:20480] = b"e" * 16384
            source.write_bytes(image)
            smoke.gpt_root(source, root, boot)
            self.assertEqual(source.read_bytes(), image)
            boot_bytes = boot.read_bytes()
            self.assertEqual(boot_bytes[446:462], bytes(16))
            self.assertEqual(boot_bytes[4096:], b"e" * 16384)
            with root.open("rb") as stream:
                stream.seek(4096)
                primary = bytearray(stream.read(4096))
                checksum = struct.unpack_from("<I", primary, 16)[0]
                primary[16:20] = bytes(4)
                self.assertEqual(zlib.crc32(primary[:92]), checksum)
                stream.seek(1024 * 1024)
                self.assertEqual(stream.read(8192), b"r" * 8192)
                stream.seek(-4096, 2)
                self.assertEqual(stream.read(8), b"EFI PART")
