import importlib.util
import io
from pathlib import Path
import sys
import tarfile
import tempfile
import unittest
from unittest import mock


scripts = Path(__file__).resolve().parents[1] / "scripts" / "pup"
sys.path.insert(0, str(scripts))
spec = importlib.util.spec_from_file_location("pup_install", scripts / "pup-install.py")
installer = importlib.util.module_from_spec(spec)
spec.loader.exec_module(installer)
sys.path.remove(str(scripts))


class PackageReplacementTests(unittest.TestCase):
    def extract(self, target):
        data = b"replacement executable"
        info = tarfile.TarInfo("executable")
        info.mode = 0o755
        info.size = len(data)
        archive = mock.Mock()
        archive.extractfile.return_value = io.BytesIO(data)
        installer.TarEntry(archive, info, str(target), None).extract()

    def test_replacement_keeps_open_inode(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "executable"
            target.write_bytes(b"running executable")
            with target.open("rb") as running:
                self.extract(target)
                self.assertEqual(running.read(), b"running executable")
            self.assertEqual(target.read_bytes(), b"replacement executable")
            self.assertEqual(target.stat().st_mode & 0o777, 0o755)

    def test_failure_preserves_previous_file(self):
        with tempfile.TemporaryDirectory() as directory:
            target = Path(directory) / "executable"
            target.write_bytes(b"running executable")
            with mock.patch.object(installer.os, "replace", side_effect=OSError):
                with self.assertRaises(OSError):
                    self.extract(target)
            self.assertEqual(target.read_bytes(), b"running executable")
            self.assertEqual(list(Path(directory).iterdir()), [target])


if __name__ == "__main__":
    unittest.main()
