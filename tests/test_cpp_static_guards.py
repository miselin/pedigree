# SPDX-License-Identifier: ISC
"""Exercise the production guard ABI with compiler-generated static initialization."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class StaticGuardTests(unittest.TestCase):
    def test_compiler_generated_guards(self):
        compiler = shutil.which("clang++")
        if not compiler:
            self.skipTest("clang++ is required")
        root = Path(__file__).resolve().parents[1]
        source = (root / "src/system/kernel/core/lib/cppsupport.cc").read_text()
        start = source.index("// The compiler tests byte zero")
        end = source.index("#ifndef HOSTED_SYSTEM_MALLOC", start)
        with tempfile.TemporaryDirectory(prefix="cpp-static-guards-") as temporary:
            work = Path(temporary)
            (work / "cpp-static-guards.inc").write_text(source[start:end])
            for threads, sanitizer_runtime in [(1, 0), (0, 0), (1, 1)]:
                with self.subTest(threads=threads, sanitizer_runtime=sanitizer_runtime):
                    binary = work / f"guards-{threads}-{sanitizer_runtime}"
                    subprocess.run([
                        compiler, "-std=c++20", "-O2", "-Wall", "-Wextra", "-Werror",
                        "-pthread", "-fsanitize=address,undefined",
                        "-fno-omit-frame-pointer", f"-DTHREADS={threads}",
                        f"-DHAS_THREAD_SANITIZER={sanitizer_runtime}", "-I", str(work),
                        str(root / "tests/fixtures/cpp-static-guards.cc"), "-o", str(binary),
                    ], check=True, timeout=60)
                    subprocess.run([str(binary)], check=True, timeout=20,
                                   env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    unittest.main()
