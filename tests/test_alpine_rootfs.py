import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]


class AlpinePreparationTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.directory = Path(self.temporary.name)
        self.output = self.directory / "prepared root"
        self.capture = self.directory / "docker.jsonl"
        docker = self.directory / "docker"
        docker.write_text(
            f"#!{sys.executable}\n"
            "import json, os, sys\n"
            "from pathlib import Path\n"
            "args = sys.argv[1:]\n"
            "with open(os.environ['DOCKER_CAPTURE'], 'a') as capture:\n"
            "    capture.write(json.dumps(args) + '\\n')\n"
            "if args[0] == 'version':\n"
            "    print('linux/arm64')\n"
            "elif args[0] == 'run':\n"
            "    if os.environ.get('DOCKER_FAIL'):\n"
            "        sys.exit(42)\n"
            "    output = Path(args[args.index('-v') + 1].removesuffix(':/out'))\n"
            "    for name in ('rootfs/lib/apk/db/installed', 'sysroot/usr/include/errno.h',\n"
            "                 'sysroot/lib/ld-musl.so.1', 'rootfs.img',\n"
            "                 'rootfs.packages', 'sysroot.packages', 'sysroot.tar'):\n"
            "        path = output / name\n"
            "        path.parent.mkdir(parents=True, exist_ok=True)\n"
            "        path.write_text('prepared')\n"
            "    libraries = output / 'sysroot/usr/lib'\n"
            "    libraries.mkdir()\n"
            "    (libraries / 'libc.so').symlink_to('../../lib/ld-musl.so.1')\n"
        )
        docker.chmod(0o755)
        self.environment = {
            **os.environ,
            "PATH": f"{self.directory}:{os.environ['PATH']}",
            "DOCKER_CAPTURE": str(self.capture),
        }

    def prepare(self, *arguments):
        return subprocess.run(
            [str(ROOT / "scripts/alpine/build.sh"), "x86_64", str(self.output), *arguments],
            env=self.environment,
            check=False,
            capture_output=True,
            text=True,
            timeout=10,
        )

    def commands(self):
        return [json.loads(line) for line in self.capture.read_text().splitlines()]

    def test_foreign_packages_use_native_builder_and_complete_cache(self):
        first = self.prepare()
        self.assertEqual(first.returncode, 0, first.stderr)
        run = self.commands()[-1]
        self.assertEqual(run[run.index("--platform") + 1], "linux/arm64")
        self.assertIn("ALPINE_ARCH=x86_64", run)
        self.assertIn("ALPINE_PROFILE=base", run)
        self.assertTrue((self.output / "sysroot/usr/lib/libc.so").is_file())
        self.capture.write_text("")
        cached = self.prepare()
        self.assertEqual(cached.returncode, 0, cached.stderr)
        self.assertEqual(self.commands(), [])
        (self.output / "sysroot/lib/ld-musl.so.1").unlink()
        incomplete = self.prepare()
        self.assertEqual(incomplete.returncode, 0, incomplete.stderr)
        self.assertTrue(self.commands())

    def test_profile_changes_and_refresh_rebuild(self):
        self.assertEqual(self.prepare().returncode, 0)
        self.capture.write_text("")
        result = self.prepare("--profile", "desktop")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertIn("ALPINE_PROFILE=desktop", self.commands()[-1])
        self.capture.write_text("")
        result = self.prepare("--profile", "desktop", "--refresh")
        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertTrue(self.commands())

    def test_failed_refresh_preserves_existing_preparation(self):
        self.assertEqual(self.prepare().returncode, 0)
        original = (self.output / ".prepared").read_text()
        self.environment["DOCKER_FAIL"] = "1"
        result = self.prepare("--refresh")
        self.assertEqual(result.returncode, 42, result.stderr)
        self.assertEqual((self.output / ".prepared").read_text(), original)
        self.assertEqual((self.output / "rootfs.img").read_text(), "prepared")
        self.assertEqual(list(self.output.glob(".prepare.*")), [])


if __name__ == "__main__":
    unittest.main()
