import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = shutil.which("cmake")
SDK_HELPER = ROOT / "build-etc/cmake/PedigreeAlpineSdk.cmake"
TARGETS = {
    "X64": ("x86_64", "x86_64", 2, 62),
    "ARM64": ("aarch64", "aarch64", 2, 183),
    "ARMV7": ("armv7", "armhf", 1, 40),
}


@unittest.skipUnless(CMAKE, "requires CMake")
class AlpineSdkTests(unittest.TestCase):
    def make_sdk(self, directory, target="X64"):
        root = Path(directory) / "SDK with spaces"
        _, loader_arch, elf_class, machine = TARGETS[target]
        loader = root / f"lib/ld-musl-{loader_arch}.so.1"
        loader.parent.mkdir(parents=True)
        header = bytearray(20)
        header[:7] = b"\x7fELF" + bytes((elf_class, 1, 1))
        header[18:20] = machine.to_bytes(2, "little")
        loader.write_bytes(header)
        for relative in (
            "usr/include/errno.h",
            "usr/include/stdio.h",
            "usr/include/stdlib.h",
            "usr/include/stdint.h",
            "usr/include/unistd.h",
            "usr/include/bits/syscall.h",
            "usr/include/sys/syscall.h",
            "usr/lib/crt1.o",
            "usr/lib/Scrt1.o",
            "usr/lib/rcrt1.o",
            "usr/lib/crti.o",
            "usr/lib/crtn.o",
            "usr/lib/libc.a",
            "usr/lib/libm.a",
            "usr/lib/libpthread.a",
            "usr/lib/libdl.a",
            "usr/lib/librt.a",
        ):
            path = root / relative
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(b"fixture")
        os.symlink(f"../../lib/{loader.name}", root / "usr/lib/libc.so")
        return root

    def configure(self, root, target="X64"):
        script = root.parent / "check.cmake"
        script.write_text(
            "cmake_minimum_required(VERSION 3.21)\n"
            f'include("{SDK_HELPER}")\n'
            "pedigree_use_alpine_sdk()\n"
            'message(STATUS "root=${PEDIGREE_MUSL_SDK_ROOT}")\n'
            'message(STATUS "prefix=${PEDIGREE_MUSL_PREFIX_ROOT}")\n'
            'message(STATUS "loader=${PEDIGREE_MUSL_DYNAMIC_LOADER}")\n'
        )
        return subprocess.run(
            [
                CMAKE,
                f"-DPEDIGREE_ARCH_TARGET={target}",
                f"-DPEDIGREE_TARGET_SYSROOT={root}",
                "-P",
                str(script),
            ],
            text=True,
            capture_output=True,
            check=False,
        )

    def test_accepts_each_alpine_architecture_and_preserves_layout(self):
        for target, (_, loader_arch, _, _) in TARGETS.items():
            with (
                self.subTest(target=target),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = self.make_sdk(directory, target)
                result = self.configure(root, target)
                self.assertEqual(result.returncode, 0, result.stderr)
                self.assertIn(f"root={root}", result.stdout)
                self.assertIn(f"prefix={root}/usr", result.stdout)
                self.assertIn(f"loader=/lib/ld-musl-{loader_arch}.so.1", result.stdout)

    def test_missing_sdk_files_explain_preparation(self):
        for relative in (
            "lib/ld-musl-x86_64.so.1",
            "usr/lib/crt1.o",
            "usr/include/stdio.h",
        ):
            with (
                self.subTest(path=relative),
                tempfile.TemporaryDirectory() as directory,
            ):
                root = self.make_sdk(directory)
                (root / relative).unlink()
                result = self.configure(root)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn(relative, result.stderr)
                self.assertIn(
                    "scripts/alpine/build.sh x86_64", " ".join(result.stderr.split())
                )

    def test_rejects_wrong_loader_architecture(self):
        with tempfile.TemporaryDirectory() as directory:
            root = self.make_sdk(directory)
            loader = root / "lib/ld-musl-x86_64.so.1"
            data = bytearray(loader.read_bytes())
            data[18:20] = (183).to_bytes(2, "little")
            loader.write_bytes(data)
            result = self.configure(root)
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("loader is not x86_64 ELF", result.stderr)

    def test_rejects_libc_links_outside_the_sdk_loader(self):
        for link in ("/lib/ld-musl-x86_64.so.1", "other-libc.so"):
            with self.subTest(link=link), tempfile.TemporaryDirectory() as directory:
                root = self.make_sdk(directory)
                libc = root / "usr/lib/libc.so"
                libc.unlink()
                os.symlink(link, libc)
                result = self.configure(root)
                self.assertNotEqual(result.returncode, 0)
                self.assertIn("libc.so must resolve to its in-root", result.stderr)

    def test_toolchain_sdk_environment_default_and_explicit_override(self):
        environment = dict(os.environ)
        environment["PEDIGREE_TARGET_SYSROOT"] = "/environment/sdk"
        environment["PEDIGREE_TOOLCHAIN_ROOT"] = "/environment/compiler"
        for target in ("amd64", "arm64", "armv7"):
            for explicit in (False, True):
                with (
                    self.subTest(target=target, explicit=explicit),
                    tempfile.TemporaryDirectory() as directory,
                ):
                    script = Path(directory) / "toolchain.cmake"
                    script.write_text(
                        f'include("{ROOT}/build-etc/cmake/pedigree_{target}.cmake")\n'
                        'message(STATUS "sysroot=${CMAKE_SYSROOT}")\n'
                        'message(STATUS "compiler=${PEDIGREE_TOOLCHAIN_ROOT}")\n'
                    )
                    command = [CMAKE]
                    if explicit:
                        command.extend(
                            (
                                "-DPEDIGREE_TARGET_SYSROOT=/explicit/sdk",
                                "-DPEDIGREE_TOOLCHAIN_ROOT=/explicit/compiler",
                            )
                        )
                    result = subprocess.run(
                        [*command, "-P", str(script)],
                        env=environment,
                        capture_output=True,
                        text=True,
                        check=False,
                    )
                    self.assertEqual(result.returncode, 0, result.stderr)
                    prefix = "/explicit" if explicit else "/environment"
                    self.assertIn(f"sysroot={prefix}/sdk", result.stdout)
                    self.assertIn(f"compiler={prefix}/compiler", result.stdout)


if __name__ == "__main__":
    unittest.main()
