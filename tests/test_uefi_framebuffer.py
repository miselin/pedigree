"""Validate GOP pixel formats and physical framebuffer bounds before handoff."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class FramebufferTests(unittest.TestCase):
    def test_framebuffer_contracts(self):
        repository = Path(__file__).resolve().parents[1]
        compiler = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory(prefix="pedigree-uefi-framebuffer-") as temporary:
            binary = Path(temporary) / "uefi-framebuffer-contracts"
            subprocess.run(
                [
                    *compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(repository / "tests/fixtures/uefi-framebuffer.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), "UEFI framebuffer contracts passed")


if __name__ == "__main__":
    unittest.main()
