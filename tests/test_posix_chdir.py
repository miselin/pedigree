# SPDX-License-Identifier: ISC
"""Exercise production chdir error handling with native pathname lookup seams."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class PosixChdirTests(unittest.TestCase):
    def test_lookup_errors_and_working_directory(self):
        compiler = shutil.which("clang++")
        if not compiler:
            self.skipTest("clang++ is required")
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/modules/subsys/posix/file-syscalls.cc").read_text()
        start = source.index("int posix_chdir(const char* path)")
        end = source.index("int posix_dup(int fd)", start)
        with tempfile.TemporaryDirectory(prefix="posix-chdir-") as temporary:
            work = Path(temporary)
            (work / "posix-chdir.inc").write_text(source[start:end])
            binary = work / "chdir-test"
            subprocess.run([
                compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(work), str(root / "tests/fixtures/posix-chdir.cc"),
                "-o", str(binary),
            ], check=True, timeout=60)
            subprocess.run([str(binary), str(work)], check=True, timeout=15,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    unittest.main()
