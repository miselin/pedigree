import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parents[1]
LINUX_HOSTED_SCRIPTS = (
    SOURCE_ROOT / "scripts/test-hosted-irq-closure.sh",
    SOURCE_ROOT / "scripts/test-hosted-kernel.sh",
    SOURCE_ROOT / "scripts/verify-irq-closure.sh",
)


@unittest.skipUnless(os.name == "posix", "requires a POSIX shell")
class HostedEntrypointTests(unittest.TestCase):
    def test_linux_hosted_scripts_have_no_container_fallback(self):
        for script in LINUX_HOSTED_SCRIPTS:
            with self.subTest(script=script.name):
                contents = script.read_text(encoding="utf-8").lower()
                self.assertNotIn("docker", contents)

        self.assertFalse(
            (SOURCE_ROOT / "build-etc/docker/hosted.Dockerfile").exists()
        )

    def test_linux_hosted_scripts_reject_cross_host_execution(self):
        with tempfile.TemporaryDirectory() as temporary:
            bin_dir = Path(temporary) / "bin"
            bin_dir.mkdir()
            uname = bin_dir / "uname"
            uname.write_text(
                "#!/bin/sh\n"
                'case "$1" in\n'
                '  -s) echo Darwin ;;\n'
                '  -m) echo arm64 ;;\n'
                '  *) echo "Darwin test-host" ;;\n'
                "esac\n",
                encoding="utf-8",
            )
            uname.chmod(0o755)
            environment = os.environ.copy()
            environment["PATH"] = str(bin_dir) + os.pathsep + environment["PATH"]

            for script in LINUX_HOSTED_SCRIPTS:
                with self.subTest(script=script.name):
                    result = subprocess.run(
                        [str(script)],
                        capture_output=True,
                        check=False,
                        env=environment,
                        text=True,
                    )
                    self.assertEqual(result.returncode, 2)
                    self.assertIn("native x86-64 Linux host", result.stderr)

    @unittest.skipUnless(shutil.which("cmake"), "requires CMake")
    def test_linux_hosted_toolchain_rejects_cross_host_configuration(self):
        result = subprocess.run(
            [
                shutil.which("cmake"),
                "-DCMAKE_HOST_SYSTEM_NAME=Darwin",
                "-DCMAKE_HOST_SYSTEM_PROCESSOR=arm64",
                "-P",
                str(SOURCE_ROOT / "build-etc/cmake/pedigree_hosted.cmake"),
            ],
            capture_output=True,
            check=False,
            text=True,
        )
        self.assertNotEqual(result.returncode, 0)
        self.assertIn("native x86-64 Linux host", result.stderr)


if __name__ == "__main__":
    unittest.main()
