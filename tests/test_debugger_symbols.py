# SPDX-License-Identifier: ISC
"""Exercise production address lookup against bounded synthetic ELF tables."""
import os
from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class DebuggerSymbolTests(unittest.TestCase):
    def test_private_weak_boundaries_and_runtime_full_table(self):
        compiler = shutil.which("clang++")
        if not compiler:
            self.skipTest("clang++ is required")
        root = Path(__file__).resolve().parents[1]
        linker = root / "src/system/kernel/linker"
        source = (linker / "Elf.cc").read_text()
        start = source.index("template <class T>\nconst char* Elf::lookupSymbol(")
        end = source.index("uintptr_t Elf::lookupSymbol(const char*", start)
        runtime = (linker / "KernelElf-runtime.cc").read_text()
        runtime_start = runtime.index("const char* KernelElf::runtimeLookupSymbolLocked(")
        runtime_end = runtime.index("uintptr_t KernelElf::resolveRuntimeImport(", runtime_start)
        with tempfile.TemporaryDirectory(prefix="debugger-symbols-") as temporary:
            work = Path(temporary)
            (work / "debugger-symbols.inc").write_text(
                source[start:end] + runtime[runtime_start:runtime_end]
            )
            binary = work / "symbols-test"
            subprocess.run([
                compiler, "-std=c++20", "-Wall", "-Wextra", "-Werror",
                "-fsanitize=address,undefined", "-fno-omit-frame-pointer",
                "-I", str(work), str(root / "tests/fixtures/debugger-symbols.cc"),
                "-o", str(binary),
            ], check=True, timeout=60)
            subprocess.run([str(binary)], check=True, timeout=15,
                           env=dict(os.environ, ASAN_OPTIONS="detect_leaks=0"))


if __name__ == "__main__":
    unittest.main()
