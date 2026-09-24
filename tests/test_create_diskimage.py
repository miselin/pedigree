import subprocess
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts.create_diskimage import build_file_list, create_image, image_size


class AlpineImageOverlayTests(unittest.TestCase):
    def setUp(self):
        self.temporary = tempfile.TemporaryDirectory()
        self.addCleanup(self.temporary.cleanup)
        self.root = Path(self.temporary.name)
        self.base = self.root / "rootfs"
        for directory in ("bin", "lib/apk/db", "usr/bin", "usr/lib", "etc", "boot"):
            (self.base / directory).mkdir(parents=True, exist_ok=True)
        self.write(self.base / "bin/busybox", "Alpine busybox")
        self.write(self.base / "lib/ld-musl-x86_64.so.1", "Alpine musl")
        self.write(self.base / "lib/apk/db/installed", "Alpine packages")
        self.write(self.base / "etc/passwd", "Alpine accounts")
        (self.base / "usr/bin/init").symlink_to("../../bin/busybox")

    def write(self, path, contents="Pedigree artifact", mode=0o644):
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text(contents)
        path.chmod(mode)
        return path

    def test_overlays_only_explicit_artifacts_without_base_runtime_changes(self):
        init = self.write(self.root / "build/init", mode=0o755)
        config = self.write(self.root / "desktop/inittab")
        self.write(self.root / "images/local/libraries/libc.so", "obsolete libc")
        self.write(
            self.root / "images/local/support/pup/db/packages.pupdb", "obsolete DB"
        )
        sdk = self.root / "pedigree-sdk/usr"
        header = self.write(sdk / "include/pedigree/fb.h")
        library = self.write(sdk / "lib/libpedigree-c.so", mode=0o755)
        commands = build_file_list(
            self.base,
            [(init, "/usr/bin/init"), (config, "/etc/inittab")],
            [(sdk, "/usr")],
        )
        self.assertIn("rm /usr/bin/init", commands)
        self.assertIn(f"write {init} /usr/bin/init", commands)
        self.assertLess(
            commands.index("rm /usr/bin/init"),
            commands.index(f"write {init} /usr/bin/init"),
        )
        self.assertIn("chmod /usr/bin/init 755", commands)
        self.assertIn(f"write {header} /usr/include/pedigree/fb.h", commands)
        self.assertIn(f"write {library} /usr/lib/libpedigree-c.so", commands)
        for protected in (
            "/bin/busybox",
            "/lib/ld-musl",
            "/lib/apk",
            "/etc/passwd",
            "pup",
            "images/local",
        ):
            self.assertFalse(
                any(protected in command for command in commands), protected
            )
        self.assertTrue((self.base / "usr/bin/init").is_symlink())
        self.assertFalse(
            any(
                command in commands
                for command in ("mkdir /usr", "mkdir /usr/lib", "mkdir /etc")
            )
        )

    def test_keeps_base_directory_symlinks_and_resolves_them_inside_image(self):
        (self.base / "libraries").symlink_to("/usr/lib")
        library = self.write(self.root / "libpedigree.so")
        commands = build_file_list(self.base, [(library, "/libraries/libpedigree.so")])
        self.assertIn(f"write {library} /usr/lib/libpedigree.so", commands)
        self.assertFalse(any("/libraries" in command for command in commands))
        self.assertTrue((self.base / "libraries").is_symlink())
        (self.base / "config").symlink_to("/etc")
        with self.assertRaisesRegex(ValueError, "account or package"):
            build_file_list(self.base, [(library, "/config/passwd")])

    def test_rejects_runtime_replacements_and_ambiguous_overlays(self):
        source = self.write(self.root / "source")
        other = self.write(self.root / "other")
        for target in (
            "/etc/group",
            "/etc/shadow",
            "/lib/apk/db/installed",
            "/etc/apk/repositories",
            "/lib/ld-musl-x86_64.so.1",
            "/usr/lib/libc.so",
            "/usr/lib/libc.a",
            "/bin",
            "/usr/../etc/passwd",
            "usr/bin/relative",
        ):
            with self.subTest(target=target), self.assertRaises(ValueError):
                build_file_list(self.base, [(source, target)])
        with self.assertRaisesRegex(ValueError, "Multiple overlay sources"):
            build_file_list(
                self.base, [(source, "/usr/bin/tool"), (other, "/usr/bin/tool")]
            )
        with self.assertRaisesRegex(ValueError, "also used as a directory"):
            build_file_list(self.base, [(source, "/usr/new"), (other, "/usr/new/tool")])
        with self.assertRaisesRegex(ValueError, "not a file"):
            build_file_list(self.base, [(self.root / "missing", "/usr/bin/tool")])

    def test_tree_does_not_follow_symlinked_directories(self):
        outside = self.write(self.root / "outside/not-declared")
        tree = self.root / "tree"
        tree.mkdir()
        (tree / "link").symlink_to("../outside")
        commands = build_file_list(self.base, trees=[(tree, "/usr/share/selected")])
        self.assertIn("symlink /usr/share/selected/link ../outside", commands)
        self.assertFalse(any(str(outside) in command for command in commands))

    def test_image_growth_keeps_base_capacity_and_accounts_for_overlay(self):
        source = self.write(self.root / "source", "x" * 4097)
        self.assertEqual(image_size(64 << 20, []), 64 << 20)
        self.assertEqual(
            image_size(64 << 20, [f"write {source} /usr/bin/tool"]), 128 << 20
        )

    def test_copy_is_atomic_and_never_modifies_base(self):
        base_image = self.write(self.root / "alpine.img", "pristine Alpine image")
        target = self.write(self.root / "output.img", "previous output")
        create_image(target, "ext2img", base_image, [])
        self.assertEqual(target.read_bytes(), base_image.read_bytes())
        target.write_text("previous output")
        source = self.write(self.root / "source")

        def run(arguments, **kwargs):
            if arguments[0] == "ext2img":
                raise subprocess.CalledProcessError(1, arguments)
            return subprocess.CompletedProcess(arguments, 0)

        with (
            mock.patch(
                "scripts.create_diskimage.e2fsprog", side_effect=lambda name: name
            ),
            mock.patch("scripts.create_diskimage.subprocess.run", side_effect=run),
            self.assertRaises(subprocess.CalledProcessError),
        ):
            create_image(
                target, "ext2img", base_image, [f"write {source} /usr/bin/tool"]
            )
        self.assertEqual(target.read_text(), "previous output")
        self.assertEqual(base_image.read_text(), "pristine Alpine image")
        self.assertFalse(list(self.root.glob(".pedigree-image-*")))
        with self.assertRaisesRegex(ValueError, "must differ"):
            create_image(base_image, "ext2img", base_image, [])


if __name__ == "__main__":
    unittest.main()
