"""Exercise actual console pixels, padded scanlines and cursor restoration."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class FramebufferConsoleTests(unittest.TestCase):
    def test_pixel_contracts(self):
        repository = Path(__file__).resolve().parents[1]
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        with tempfile.TemporaryDirectory(prefix="pedigree-framebuffer-") as temporary:
            binary = Path(temporary) / "framebuffer-console"
            subprocess.run(
                [
                    *compiler,
                    "-std=c++17",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-fsanitize=address,undefined",
                    str(repository / "tests/fixtures/framebuffer-console.cc"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), "Framebuffer console contracts passed")


if __name__ == "__main__":
    unittest.main()
