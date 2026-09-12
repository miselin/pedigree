"""Exercise the production static AML S5 decoder with malformed firmware data."""

from pathlib import Path
import shutil
import subprocess
import tempfile
import unittest


class AcpiS5Tests(unittest.TestCase):
    def test_static_packages_and_bounds(self):
        compiler = shutil.which("clang++")
        if not compiler:
            self.skipTest("clang++ is required")
        root = Path(__file__).resolve().parents[1]
        with tempfile.TemporaryDirectory(prefix="acpi-s5-") as temporary:
            binary = Path(temporary) / "acpi-s5"
            subprocess.run(
                [compiler, "-std=c++17", "-Wall", "-Wextra", "-Werror",
                 "-fsanitize=address,undefined", "-I", str(root),
                 str(root / "tests/fixtures/acpi-s5.cc"), "-o", str(binary)],
                check=True, timeout=60,
            )
            subprocess.run([str(binary)], check=True, timeout=20)


if __name__ == "__main__":
    unittest.main()
