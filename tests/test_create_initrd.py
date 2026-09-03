import gzip
import hashlib
import io
import os
import subprocess
import sys
import tarfile
import tempfile
import unittest
from pathlib import Path


REPOSITORY = Path(__file__).resolve().parents[1]
CREATE_INITRD = REPOSITORY / "scripts" / "create_initrd.py"


class CreateInitrdTests(unittest.TestCase):
    def test_archive_preserves_order_normalizes_and_is_reproducible(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            first = root / "first.o"
            second = root / "second.o"
            first.write_bytes(b"first module")
            second.write_bytes(b"second module")
            os.utime(first, (100, 100))
            os.utime(second, (200, 200))

            outputs = []
            for suffix in ("one", "two"):
                compressed = root / (suffix + ".tar.gz")
                uncompressed = root / (suffix + ".tar")
                manifest = root / (suffix + ".manifest")
                subprocess.run(
                    [
                        sys.executable,
                        str(CREATE_INITRD),
                        "--output",
                        str(compressed),
                        "--uncompressed",
                        str(uncompressed),
                        "--manifest",
                        str(manifest),
                        "zeta.o=" + str(second),
                        "alpha.o=" + str(first),
                    ],
                    check=True,
                )
                outputs.append((compressed, uncompressed, manifest))

            for left, right in zip(outputs[0], outputs[1]):
                self.assertEqual(left.read_bytes(), right.read_bytes())

            compressed, uncompressed, manifest = outputs[0]
            self.assertEqual(gzip.decompress(compressed.read_bytes()), uncompressed.read_bytes())
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


if __name__ == "__main__":
    unittest.main()
