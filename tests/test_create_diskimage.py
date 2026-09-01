import tempfile
import unittest
from pathlib import Path

from scripts.create_diskimage import build_file_list, translate_target_path


class DiskImageLayoutTests(unittest.TestCase):
    def test_translates_legacy_package_paths_to_fhs(self):
        cases = {
            "/applications/bash": "/usr/bin/bash",
            "/libraries/libc.so": "/usr/lib/libc.so",
            "/config/profile": "/etc/profile",
            "/system/modules/vfs.o": "/usr/lib/modules/vfs.o",
            "/system/include/stdio.h": "/usr/include/stdio.h",
            "/system/locale/en_US.UTF-8": "/usr/share/locale/en_US.UTF-8",
            "/users/andy": "/home/andy",
            "/support/gcc/specs": "/usr/lib/pedigree/gcc/specs",
            "/support/pup/db/packages.pupdb": "/var/cache/pup/packages.pupdb",
            "/support/pup/pup.conf": "/etc/pup/pup.conf",
        }

        for legacy, fhs in cases.items():
            with self.subTest(legacy=legacy):
                self.assertEqual(translate_target_path(legacy), fhs)

    def test_does_not_translate_partial_path_components(self):
        self.assertEqual(
            translate_target_path("/applications-old/tool"),
            "/applications-old/tool",
        )

    def test_leaves_fhs_paths_unchanged(self):
        self.assertEqual(translate_target_path("/usr/bin/ls"), "/usr/bin/ls")

    def test_build_file_list_contains_only_canonical_layout(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            images = root / "images"
            base = root / "base"
            musl = root / "musl"
            binary = root / "build"

            files = {
                images / "applications" / "ls": "binary",
                images / "libraries" / "libc.so": "library",
                images / "support" / "pup" / "db" / "packages.pupdb": "db",
                base / "config" / "profile": "profile",
                base / ".profile": "root profile",
                base / "etc" / "passwd": "root:x:0:0:Root User:/root:/bin/bash\n",
                base / "etc" / "group": "administrators:x:0:root\n",
                base / "etc" / "shadow": "root:root:0:0:99999:7:::\n",
                musl / "lib" / "crt1.o": "crt",
                musl / "include" / "stdio.h": "header",
            }
            for path, content in files.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)

            for lang in ("en_US", "de_DE"):
                (binary / "src" / "po" / lang).mkdir(parents=True)
            (binary / "keymaps").mkdir(parents=True)

            configdb = root / "config.db"
            configdb.write_text("config")

            kernel = root / "kernel"
            grub = root / "menu.lst"
            kernel.write_text("kernel")
            grub.write_text("grub")

            commands = build_file_list(
                [
                    str(images),
                    str(root),
                    str(base),
                    str(kernel),
                    "__noinitrd__",
                    str(configdb),
                    str(grub),
                    str(musl),
                    str(binary),
                ]
            )

            self.assertIn("write %s /usr/bin/ls" % files_key(files, "ls"), commands)
            self.assertIn("symlink /bin /usr/bin", commands)
            self.assertIn("symlink /lib /usr/lib", commands)
            self.assertIn("mkdir /media", commands)
            self.assertTrue(any(command.endswith(" /etc/profile") for command in commands))
            self.assertTrue(any(command.endswith(" /etc/passwd") for command in commands))
            self.assertTrue(any(command.endswith(" /etc/group") for command in commands))
            self.assertTrue(any(command.endswith(" /etc/shadow") for command in commands))
            self.assertIn("chmod /etc/shadow 600", commands)
            self.assertTrue(any(command.endswith(" /root/.profile") for command in commands))
            self.assertTrue(any(command.endswith(" /var/cache/pup/packages.pupdb") for command in commands))
            self.assertFalse(any(" /applications" in command for command in commands))
            self.assertFalse(any(" /libraries" in command for command in commands))

def files_key(files, basename):
    return next(str(path) for path in files if path.name == basename)


if __name__ == "__main__":
    unittest.main()
