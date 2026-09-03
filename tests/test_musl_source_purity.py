import os
import shutil
import subprocess
import tarfile
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
PATCHES = (
    ROOT / "build-etc/toolchain/musl-1.2.6-cve-2026-40200-qsort.patch",
    ROOT / "build-etc/toolchain/musl-1.2.6-cve-2026-6042-iconv.patch",
)
IGNORED_TOP_LEVEL = {".pedigree-upstream", "build"}


def source_files(root):
    result = {}
    for path in root.rglob("*"):
        relative = path.relative_to(root)
        if relative.parts[0] in IGNORED_TOP_LEVEL:
            continue
        if relative.name == ".pedigree-build-musl.lock":
            continue
        if path.is_symlink():
            result[relative.as_posix()] = b"symlink\0" + os.readlink(path).encode()
        elif path.is_file():
            result[relative.as_posix()] = path.read_bytes()
    return result


class MuslSourcePurityTests(unittest.TestCase):
    def test_target_source_matches_upstream_plus_security_backports(self):
        archive_value = os.environ.get("PEDIGREE_MUSL_ARCHIVE")
        source_value = os.environ.get("PEDIGREE_MUSL_SOURCE")
        if not archive_value or not source_value:
            self.skipTest("target musl derivation paths were not provided")

        archive = Path(archive_value)
        source = Path(source_value)
        self.assertTrue(archive.is_file(), archive)
        self.assertTrue(source.is_dir(), source)

        patch_tool = shutil.which("patch")
        self.assertIsNotNone(patch_tool, "patch is required to reproduce the derivation")

        with tempfile.TemporaryDirectory() as temporary:
            baseline_parent = Path(temporary)
            with tarfile.open(archive, "r:gz") as musl_archive:
                if hasattr(tarfile, "data_filter"):
                    musl_archive.extractall(baseline_parent, filter="data")
                else:
                    musl_archive.extractall(baseline_parent)

            roots = [path for path in baseline_parent.iterdir() if path.is_dir()]
            self.assertEqual(len(roots), 1)
            baseline = roots[0]
            for source_patch in PATCHES:
                subprocess.run(
                    [patch_tool, "-f", "-s", "-p1", "-i", str(source_patch)],
                    cwd=baseline,
                    check=True,
                    capture_output=True,
                    text=True,
                )

            expected = source_files(baseline)
            actual = source_files(source)

        missing = sorted(expected.keys() - actual.keys())
        added = sorted(actual.keys() - expected.keys())
        changed = sorted(
            name
            for name in expected.keys() & actual.keys()
            if expected[name] != actual[name]
        )
        self.assertEqual(
            (missing, added, changed),
            ([], [], []),
            "target musl source differs from upstream plus the security patch queue",
        )


if __name__ == "__main__":
    unittest.main()
