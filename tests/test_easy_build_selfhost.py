import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == "posix", "requires a POSIX shell")
class SelfhostWrapperTests(unittest.TestCase):
    def test_boot_artifacts_disable_all_image_packaging(self):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary).resolve()
            source = directory / "source checkout"
            source.mkdir()
            shutil.copy2(ROOT / "easy_build_selfhost.sh", source)
            commands = directory / "commands"
            commands.mkdir()
            capture = directory / "cmake.args"
            scripts = {
                "uname": "printf '%s\\n' Pedigree\n",
                "make": "exit 97\n",
                "cmake": (
                    "printf '%s\\0' \"$@\" >>\"$CMAKE_CAPTURE\"\n"
                    "printf '\\0' >>\"$CMAKE_CAPTURE\"\n"
                ),
            }
            for name, body in scripts.items():
                command = commands / name
                command.write_text("#!/bin/sh\nset -eu\n" + body)
                command.chmod(0o755)
            environment = {
                key: value
                for key, value in os.environ.items()
                if not key.startswith("PEDIGREE_")
            }
            environment.update(
                PATH=f"{commands}:/usr/bin:/bin",
                CMAKE_CAPTURE=str(capture),
            )
            result = subprocess.run(
                ["/bin/sh", "./easy_build_selfhost.sh"],
                cwd=source,
                env=environment,
                capture_output=True,
                text=True,
                timeout=10,
            )
            self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
            invocations = [
                record.decode().split("\0")
                for record in capture.read_bytes().split(b"\0\0")
                if record
            ]
            self.assertEqual(len(invocations), 2)
            configure, build = invocations
            self.assertEqual(
                configure[:4],
                ["-S", str(source), "-B", str(source / "build-selfhost")],
            )
            for product in ("UEFI",):
                self.assertIn(f"-DPEDIGREE_BUILD_{product}=OFF", configure)
            self.assertIn("-DPEDIGREE_BUILD_ROLE=TARGET", configure)
            self.assertEqual(
                build,
                [
                    "--build",
                    str(source / "build-selfhost"),
                    "--target",
                    "boot-artifacts",
                    "--parallel",
                    "1",
                ],
            )


if __name__ == "__main__":
    unittest.main()
