"""Exercise the loader's gzip decoder with real compressed streams."""

import ctypes
import gzip
import os
import shlex
import struct
import subprocess
import tempfile
import unittest
import zlib
from pathlib import Path


class GzipTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        repository = Path(__file__).resolve().parents[1]
        cls.temporary = tempfile.TemporaryDirectory(prefix="pedigree-uefi-gzip-")
        cls.addClassCleanup(cls.temporary.cleanup)
        library = Path(cls.temporary.name) / "gzip.so"
        subprocess.run(
            [
                *shlex.split(os.environ.get("CC", "cc")),
                "-std=c11",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-shared",
                "-fPIC",
                str(repository / "tests/fixtures/uefi-gzip.c"),
                "-o",
                str(library),
            ],
            check=True,
            capture_output=True,
            text=True,
        )
        cls.decoder = ctypes.CDLL(str(library))
        cls.decoder.gzip_detect.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        cls.decoder.gzip_size.argtypes = [ctypes.c_void_p, ctypes.c_uint64]
        cls.decoder.gzip_size.restype = ctypes.c_uint
        cls.decoder.gzip_unpack.argtypes = [
            ctypes.c_void_p,
            ctypes.POINTER(ctypes.c_uint),
            ctypes.c_void_p,
            ctypes.c_uint,
        ]

    def unpack(self, stream, capacity):
        destination = ctypes.create_string_buffer(capacity)
        length = ctypes.c_uint(capacity)
        result = self.decoder.gzip_unpack(
            destination, ctypes.byref(length), stream, len(stream)
        )
        return result, destination.raw[: length.value]

    def test_plain_tar_detection(self):
        for payload in (b"", b"\x1f", b"module.o" + bytes(512)):
            self.assertEqual(self.decoder.gzip_detect(payload, len(payload)), 0)

    def test_stored_fixed_and_dynamic_blocks(self):
        payload = (b"module.o\0" + bytes(range(256))) * 512
        for level, strategy, block_type in (
            (0, zlib.Z_DEFAULT_STRATEGY, 0),
            (6, zlib.Z_FIXED, 1),
            (9, zlib.Z_DEFAULT_STRATEGY, 2),
        ):
            with self.subTest(block_type=block_type):
                compressor = zlib.compressobj(
                    level, zlib.DEFLATED, 31, strategy=strategy
                )
                stream = compressor.compress(payload) + compressor.flush()
                self.assertEqual((stream[10] >> 1) & 3, block_type)
                self.assertEqual(
                    self.decoder.gzip_size(stream, len(stream)), len(payload)
                )
                self.assertEqual(self.unpack(stream, len(payload)), (0, payload))

    def test_optional_headers(self):
        payload = b"initrd module data" * 512
        stream = gzip.compress(payload, mtime=0)
        header = bytearray(stream[:10])
        header[3] = 2 | 4 | 8 | 16
        header += b"\x03\x00abcinitrd.tar\0module archive\0"
        header += struct.pack("<H", zlib.crc32(header) & 0xFFFF)
        stream = bytes(header) + stream[10:]
        self.assertEqual(self.unpack(stream, len(payload)), (0, payload))
        broken = bytearray(stream)
        broken[len(header) - 1] ^= 1
        self.assertNotEqual(self.unpack(bytes(broken), len(payload))[0], 0)

    def test_corruption_and_truncation(self):
        payload = b"initrd module data" * 512
        stream = gzip.compress(payload, mtime=0)
        for offset in (2, 3, 10, len(stream) - 8, len(stream) - 4):
            with self.subTest(offset=offset):
                broken = bytearray(stream)
                broken[offset] ^= 0x80
                self.assertNotEqual(self.unpack(bytes(broken), len(payload))[0], 0)
        for length in range(len(stream)):
            self.assertNotEqual(self.unpack(stream[:length], len(payload))[0], 0)
        self.assertNotEqual(self.unpack(stream, len(payload) - 1)[0], 0)

    def test_allocation_limit(self):
        stream = bytearray(gzip.compress(b"module", mtime=0))
        for size in (0, 256 * 1024 * 1024 + 1, 0xFFFFFFFF):
            stream[-4:] = struct.pack("<I", size)
            self.assertEqual(self.decoder.gzip_size(bytes(stream), len(stream)), 0)
        self.assertEqual(self.decoder.gzip_size(b"\x1f\x8b", 2), 0)

    def test_trailing_data_and_concatenated_members(self):
        payload = b"module" * 128
        stream = gzip.compress(payload, mtime=0)
        for broken in (stream[:-8] + b"extra" + stream[-8:], stream + stream):
            self.assertNotEqual(self.unpack(broken, len(payload))[0], 0)


if __name__ == "__main__":
    unittest.main()
