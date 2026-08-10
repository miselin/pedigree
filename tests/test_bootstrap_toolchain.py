import json
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/bootstrap_toolchain.py"
MANIFEST = ROOT / "build-etc/toolchain/pedigree-cross-toolchain.json"


class BootstrapToolchainContractTests(unittest.TestCase):
    def test_manifest_preserves_pinned_toolchain_inputs(self):
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

        self.assertEqual(manifest["gcc"]["version"], "8.3.0")
        self.assertEqual(manifest["binutils"]["version"], "2.32")
        self.assertEqual(manifest["nasm"]["version"], "2.12.02")
        self.assertEqual(
            manifest["gcc"]["sha256"],
            "64baadfe6cc0f4947a84cb12d7f0dfaf45bb58b7e92461639596c21e02d97d2c",
        )
        self.assertEqual(
            manifest["binutils"]["sha256"],
            "de38b15c902eb2725eac6af21183a5f34ea4634cb0bcef19612b50e5ed31072d",
        )
        self.assertEqual(
            manifest["nasm"]["sha256"],
            "00b0891c678c065446ca59bcee64719d0096d54d6886e6e472aeee2e170ae324",
        )

    def test_dry_run_plans_host_and_cross_stages_without_mutation(self):
        with tempfile.TemporaryDirectory() as tempdir:
            prefix = Path(tempdir) / "compiler"
            result = subprocess.run(
                [
                    sys.executable,
                    str(SCRIPT),
                    "x86_64-pedigree",
                    str(prefix),
                    "--source-root",
                    str(ROOT),
                    "--osx-compat",
                    "--libcpp",
                    "--dry-run",
                ],
                check=True,
                capture_output=True,
                text=True,
            )

        self.assertIn("download gcc 8.3.0", result.stdout)
        self.assertIn("--target=x86_64-pedigree", result.stdout)
        self.assertIn("make all-gcc all-target-libgcc", result.stdout)
        self.assertIn("make all-target-libstdc++-v3", result.stdout)
        self.assertIn("compilers/dir", result.stdout)
        self.assertFalse(prefix.exists())

    def test_dry_run_rejects_an_unknown_target(self):
        result = subprocess.run(
            [
                sys.executable,
                str(SCRIPT),
                "not-a-pedigree-target",
                "/tmp/compiler",
                "--dry-run",
            ],
            check=False,
            capture_output=True,
            text=True,
        )

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("invalid choice", result.stderr)


if __name__ == "__main__":
    unittest.main()
