import gzip
import hashlib
import io
import os
import struct
import subprocess
import sys
import tarfile
import tempfile
import unittest
import zlib
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[4]
LEGACY_GENERATOR = REPOSITORY / "scripts" / "create_initrd.py"
GENERATOR = Path(sys.argv.pop(1)).resolve()


class InitrdBuilderTests(unittest.TestCase):
    def run_generator(self, root, suffix):
        compressed = root / (suffix + ".tar.gz")
        uncompressed = root / (suffix + ".tar")
        manifest = root / (suffix + ".manifest")
        subprocess.run(
            [
                str(GENERATOR),
                "--output",
                str(compressed),
                "--uncompressed",
                str(uncompressed),
                "--manifest",
                str(manifest),
                "zeta.o=" + str(root / "second.o"),
                "alpha.o=" + str(root / "first.o"),
            ],
            check=True,
        )
        return compressed, uncompressed, manifest

    def test_matches_legacy_archive_and_is_reproducible(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first.o"
            second = root / "second.o"
            first.write_bytes(b"first module")
            second.write_bytes(b"second module")
            os.utime(first, (100, 100))
            os.utime(second, (200, 200))

            first_run = self.run_generator(root, "one")
            second_run = self.run_generator(root, "two")
            for left, right in zip(first_run, second_run):
                self.assertEqual(left.read_bytes(), right.read_bytes())

            legacy_compressed = root / "legacy.tar.gz"
            legacy_uncompressed = root / "legacy.tar"
            legacy_manifest = root / "legacy.manifest"
            subprocess.run(
                [
                    sys.executable,
                    str(LEGACY_GENERATOR),
                    "--output",
                    str(legacy_compressed),
                    "--uncompressed",
                    str(legacy_uncompressed),
                    "--manifest",
                    str(legacy_manifest),
                    "zeta.o=" + str(second),
                    "alpha.o=" + str(first),
                ],
                check=True,
            )

            compressed, uncompressed, manifest = first_run
            self.assertEqual(uncompressed.read_bytes(), legacy_uncompressed.read_bytes())
            self.assertEqual(manifest.read_bytes(), legacy_manifest.read_bytes())
            compressed_payload = compressed.read_bytes()
            uncompressed_payload = uncompressed.read_bytes()
            self.assertEqual(gzip.decompress(compressed_payload), uncompressed_payload)
            self.assertEqual(
                compressed_payload[:10],
                b"\x1f\x8b\x08\x00\x00\x00\x00\x00\x02\xff",
            )
            crc, size = struct.unpack("<II", compressed_payload[-8:])
            self.assertEqual(crc, zlib.crc32(uncompressed_payload) & 0xFFFFFFFF)
            self.assertEqual(size, len(uncompressed_payload) & 0xFFFFFFFF)
            self.assertLess(len(compressed_payload), len(uncompressed_payload) // 2)
            self.assertEqual(
                manifest.read_text(encoding="utf-8"),
                "%s  zeta.o\n%s  alpha.o\n"
                % (
                    hashlib.sha256(b"second module").hexdigest(),
                    hashlib.sha256(b"first module").hexdigest(),
                ),
            )

            with tarfile.open(fileobj=io.BytesIO(uncompressed.read_bytes())) as archive:
                members = archive.getmembers()
                self.assertEqual([member.name for member in members], ["zeta.o", "alpha.o"])
                for member in members:
                    self.assertEqual(member.mtime, 0)
                    self.assertEqual(member.uid, 0)
                    self.assertEqual(member.gid, 0)
                    self.assertEqual(member.mode, 0o755)

    def test_failure_removes_all_outputs(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            compressed = root / "initrd.tar.gz"
            uncompressed = root / "initrd.tar"
            manifest = root / "initrd.manifest"
            for output in (compressed, uncompressed, manifest):
                output.write_bytes(b"stale")

            result = subprocess.run(
                [
                    str(GENERATOR),
                    "--output",
                    str(compressed),
                    "--uncompressed",
                    str(uncompressed),
                    "--manifest",
                    str(manifest),
                    "missing.o=" + str(root / "missing.o"),
                ],
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )

            self.assertNotEqual(result.returncode, 0)
            for output in (compressed, uncompressed, manifest):
                self.assertFalse(output.exists())

    def test_streams_large_incompressible_archive(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            payload = b"".join(
                hashlib.sha256(index.to_bytes(4, "little")).digest()
                for index in range(2209)
            )[:70657]
            (root / "second.o").write_bytes(payload)
            (root / "first.o").write_bytes(b"x")

            first_run = self.run_generator(root, "large-one")
            second_run = self.run_generator(root, "large-two")
            for left, right in zip(first_run, second_run):
                self.assertEqual(left.read_bytes(), right.read_bytes())

            compressed, uncompressed, _ = first_run
            self.assertEqual(len(uncompressed.read_bytes()), 81920)
            self.assertEqual(gzip.decompress(compressed.read_bytes()), uncompressed.read_bytes())


if __name__ == "__main__":
    unittest.main()
