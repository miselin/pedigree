"""Check EFI argument decoding and command-line handoff with native fixtures."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class LoadOptionTests(unittest.TestCase):
    def test_load_option_contracts(self):
        repository = Path(__file__).resolve().parents[1]
        compiler = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory(prefix="pedigree-uefi-options-") as temporary:
            binary = Path(temporary) / "uefi-load-option-contracts"
            subprocess.run(
                [
                    *compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(repository / "tests/fixtures/uefi-load-options.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), "UEFI load option contracts passed")


if __name__ == "__main__":
    unittest.main()
