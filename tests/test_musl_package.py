"""Offline integration checks using the published musl PUP and installed PUP."""

import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


ROOT = Path(__file__).resolve().parents[1]
ARCHIVE = os.environ.get("PEDIGREE_TEST_MUSL_PUP")
PUP = os.environ.get("PEDIGREE_TEST_PUP") or shutil.which("pup")
CMAKE = shutil.which("cmake")


@unittest.skipUnless(ARCHIVE and PUP and CMAKE, "requires a cached musl PUP and pup")
class MuslPackageTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory(prefix="musl package ")
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.cache = self.root / "cache"
        self.cache.mkdir()
        shutil.copyfile(ARCHIVE, self.cache / "musl-1.2.6-amd64.pup")
        self.sdk = self.root / "sdk"

    def install(self, pup=PUP):
        return subprocess.run(
            [
                CMAKE,
                f"-DSDK_ROOT={self.sdk}",
                f"-DPUP_CACHE={self.cache}",
                "-DPUP_SERVER=http://127.0.0.1:1",
                f"-DPUP_COMMAND={pup}",
                "-P",
                str(ROOT / "build-etc/cmake/InstallMuslPackage.cmake"),
            ],
            capture_output=True,
            text=True,
            timeout=30,
        )

    def assert_success(self, result):
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)

    def test_offline_install_and_reuse_without_pup(self):
        self.assert_success(self.install())
        libc = self.sdk / "usr/lib/libc.so"
        modified = libc.stat().st_mtime_ns
        self.assert_success(self.install(pup="/nonexistent/pup"))
        self.assertEqual(libc.stat().st_mtime_ns, modified)
        self.assertEqual(
            os.readlink(self.sdk / "usr/lib/ld-musl-x86_64.so.1"), "libc.so"
        )

    def test_missing_header_is_repaired_from_cache(self):
        self.assert_success(self.install())
        header = self.sdk / "usr/include/stdio.h"
        expected = header.read_bytes()
        header.unlink()
        self.assert_success(self.install())
        self.assertEqual(header.read_bytes(), expected)

    def test_failed_repair_preserves_previous_sdk(self):
        self.assert_success(self.install())
        libc = self.sdk / "usr/lib/libc.so"
        expected = libc.read_bytes()
        (self.sdk / "usr/include/stdio.h").unlink()
        result = self.install(pup="/nonexistent/pup")
        self.assertNotEqual(result.returncode, 0)
        self.assertEqual(libc.read_bytes(), expected)


if __name__ == "__main__":
    unittest.main()
