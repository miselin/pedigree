import os
import tempfile
import unittest
from pathlib import Path
from unittest import mock

from scripts.create_diskimage import build_file_list, image_size, translate_target_path


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

    def test_image_size_covers_payload_blocks_and_keeps_minimum(self):
        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "payload"
            for payload, expected in (
                (4097, 2 << 30),
                ((2 << 30) * 4 // 5, 9 * (256 << 20)),
                (2615743697, 13 * (256 << 20)),
            ):
                with self.subTest(payload=payload):
                    with source.open("wb") as stream:
                        stream.truncate(payload)
                    self.assertEqual(
                        image_size(["write %s /usr/bin/payload" % source]),
                        expected,
                    )

    def test_build_file_list_contains_only_canonical_layout(self):
        with tempfile.TemporaryDirectory() as tempdir:
            root = Path(tempdir)
            images = root / "images"
            base = root / "base"
            musl = root / "musl"
            pedigree_c_sdk = root / "pedigree-c-sdk"
            binary = root / "build"

            files = {
                images / "applications" / "ls": "binary",
                images / "applications" / "[": "old binary",
                images / "usr/bin" / "[": "new binary",
                images / "applications" / "new-link": "old file",
                images / "usr/bin" / "old-link": "new file",
                images / "applications" / "overlaid": "old package",
                images / "usr/bin" / "overlaid": "new package",
                images / "usr/bin" / "built": "package binary",
                images / "libraries" / "libc.so": "library",
                images / "support" / "pup" / "db" / "packages.pupdb": "db",
                base / "applications" / "overlaid": "base override",
                base / "config" / "profile": "profile",
                base / ".profile": "root profile",
                base / "etc" / "passwd": "root:x:0:0:Root User:/root:/bin/bash\n",
                base / "etc" / "group": "administrators:x:0:root\n",
                base / "etc" / "shadow": "root:root:0:0:99999:7:::\n",
                musl / "usr/lib" / "crt1.o": "crt",
                musl / "usr/lib" / "libc.so": "libc",
                musl / "usr/include" / "stdio.h": "header",
                musl / "usr/share/pedigree/libc/manifest.json": "{}\n",
                pedigree_c_sdk / "usr/include/pedigree/fb.h": "header",
                pedigree_c_sdk / "usr/include/pedigree/log.h": "header",
                pedigree_c_sdk / "usr/lib/libpedigree-c.so": "library",
                binary / "src/user" / "built": "build override",
            }
            for path, content in files.items():
                path.parent.mkdir(parents=True, exist_ok=True)
                path.write_text(content)
            (musl / "usr/lib/ld-musl-x86_64.so.1").symlink_to("libc.so")
            (images / "usr/bin/new-link").symlink_to("new-target")
            (images / "applications/old-link").symlink_to("old-target")

            for lang in ("en_US", "de_DE"):
                (binary / "src" / "po" / lang).mkdir(parents=True)
            (binary / "keymaps").mkdir(parents=True)

            configdb = root / "config.db"
            configdb.write_text("config")

            kernel = root / "kernel"
            grub = root / "menu.lst"
            kernel.write_text("kernel")
            grub.write_text("grub")

            sources = [
                str(images),
                str(root),
                str(base),
                str(kernel),
                "__noinitrd__",
                str(configdb),
                str(grub),
                str(musl),
                str(pedigree_c_sdk),
                str(binary),
                str(binary / "src/user/built"),
            ]
            commands = build_file_list(sources)
            walk = os.walk

            def reverse_walk(directory):
                for dirpath, dirs, entries in walk(directory):
                    dirs.sort(reverse=True)
                    yield dirpath, dirs, entries

            with mock.patch("scripts.create_diskimage.os.walk", reverse_walk):
                self.assertEqual(
                    sorted(commands), sorted(build_file_list(sources))
                )

            for name, source in (
                ("[", images / "usr/bin/["),
                ("old-link", images / "usr/bin/old-link"),
                ("overlaid", base / "applications/overlaid"),
                ("built", binary / "src/user/built"),
            ):
                target = "/usr/bin/" + name
                self.assertEqual(
                    destination_commands(commands, target),
                    ["write %s %s" % (source, target)],
                )
            self.assertEqual(
                destination_commands(commands, "/usr/bin/new-link"),
                ["symlink /usr/bin/new-link new-target"],
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
            self.assertTrue(any(command.endswith(" /usr/lib/crt1.o") for command in commands))
            self.assertIn(
                "symlink /usr/lib/ld-musl-x86_64.so.1 libc.so", commands
            )
            self.assertTrue(any(command.endswith(" /usr/include/stdio.h") for command in commands))
            self.assertTrue(
                any(
                    command.endswith(" /usr/include/pedigree/fb.h")
                    for command in commands
                )
            )
            self.assertTrue(
                any(command.endswith(" /usr/include/pedigree/log.h") for command in commands)
            )
            self.assertTrue(
                any(command.endswith(" /usr/lib/libpedigree-c.so") for command in commands)
            )
            self.assertTrue(
                any(
                    command.endswith(" /usr/share/pedigree/libc/manifest.json")
                    for command in commands
                )
            )
            self.assertFalse(any(" /applications" in command for command in commands))
            self.assertFalse(any(" /libraries" in command for command in commands))

def files_key(files, basename):
    return next(str(path) for path in files if path.name == basename)


def destination_commands(commands, target):
    return [
        command
        for command in commands
        if (command.startswith("write ") and command.rsplit(" ", 1)[-1] == target)
        or command.startswith("symlink %s " % target)
    ]


if __name__ == "__main__":
    unittest.main()
