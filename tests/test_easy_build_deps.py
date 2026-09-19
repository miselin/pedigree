import os
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


@unittest.skipUnless(os.name == "posix", "requires a POSIX shell")
class OpenSuseDependencyTests(unittest.TestCase):
    def run_dependencies(self, arguments, installer_status=0):
        with tempfile.TemporaryDirectory() as temporary:
            directory = Path(temporary)
            commands = directory / "commands"
            commands.mkdir()
            capture = directory / "install.args"
            sudo = commands / "sudo"
            sudo.write_text(
                "#!/bin/sh\n"
                'printf \'%s\\n\' "$@" >"$INSTALL_CAPTURE"\n'
                'exit "$INSTALL_STATUS"\n'
            )
            sudo.chmod(0o755)
            environment = os.environ.copy()
            environment.update(
                PATH=f"{commands}:/usr/bin:/bin",
                INSTALL_CAPTURE=str(capture),
                INSTALL_STATUS=str(installer_status),
            )
            result = subprocess.run(
                [
                    "/bin/bash",
                    "-c",
                    'script_dir="$1"; shift; . "$DEPS_SCRIPT" "$@"',
                    "dependency-test",
                    str(directory),
                    *arguments,
                ],
                env={
                    **environment,
                    "DEPS_SCRIPT": str(ROOT / "scripts/easy_build_deps.sh"),
                },
                capture_output=True,
                text=True,
                timeout=10,
            )
            installed = capture.read_text().splitlines() if capture.exists() else []
            os_file = directory / ".easy_os"
            recorded_os = os_file.read_text().strip() if os_file.exists() else None
            return result, installed, recorded_os

    def test_unattended_install_includes_default_image_dependencies(self):
        result, installed, recorded_os = self.run_dependencies(
            ["noconfirm", "opensuse"]
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(installed[:3], ["zypper", "install", "-y"])
        for package in ("clang", "gettext-tools", "mtools", "dosfstools", "e2fsprogs"):
            self.assertIn(package, installed)
        self.assertEqual(recorded_os, "opensuse")

    def test_nosudo_skips_package_installation(self):
        result, installed, recorded_os = self.run_dependencies(
            ["nosudo", "opensuse"]
        )
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(installed, [])
        self.assertEqual(recorded_os, "opensuse")

    def test_failed_install_does_not_record_completed_setup(self):
        result, installed, recorded_os = self.run_dependencies(
            ["opensuse"], installer_status=42
        )
        self.assertEqual(result.returncode, 42, result.stdout + result.stderr)
        self.assertEqual(installed[:2], ["zypper", "install"])
        self.assertIsNone(recorded_os)


if __name__ == "__main__":
    unittest.main()
