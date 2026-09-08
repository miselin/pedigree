"""Execute the loader's exit/map logic against native firmware-service stubs."""

import os
from pathlib import Path
import shlex
import subprocess
import tempfile
import unittest


class ExitBootServicesTests(unittest.TestCase):
    def test_exit_boot_services_contracts(self):
        repository = Path(__file__).resolve().parents[1]
        compiler = shlex.split(os.environ.get("CC", "cc"))
        with tempfile.TemporaryDirectory(prefix="pedigree-uefi-exit-") as temporary:
            binary = Path(temporary) / "uefi-exit-contracts"
            subprocess.run(
                [
                    *compiler,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    str(repository / "tests/fixtures/uefi-exit-boot-services.c"),
                    "-o",
                    str(binary),
                ],
                check=True,
                capture_output=True,
                text=True,
            )
            result = subprocess.run([str(binary)], check=True, capture_output=True, text=True)
            self.assertEqual(result.stdout.strip(), "UEFI exit contracts passed")


if __name__ == "__main__":
    unittest.main()
