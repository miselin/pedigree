import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from contextlib import redirect_stdout
from pathlib import Path
from unittest import mock

from scripts.bootstrap_toolchain import Bootstrapper, parse_args

ROOT = Path(__file__).resolve().parents[1]
SCRIPT = ROOT / "scripts/bootstrap_toolchain.py"
MANIFEST = ROOT / "build-etc/toolchain/pedigree-cross-toolchain.json"


class BootstrapToolchainContractTests(unittest.TestCase):
    def test_default_sysroot_uses_the_alpine_sdk(self):
        with tempfile.TemporaryDirectory() as tempdir:
            bootstrapper = Bootstrapper(
                parse_args(
                    [
                        "x86_64-pedigree",
                        str(Path(tempdir) / "compiler"),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )

            self.assertEqual(
                bootstrapper.sysroot, ROOT / "scripts/alpine/build/x86_64/sysroot/usr"
            )

            arm64 = Bootstrapper(
                parse_args(
                    [
                        "aarch64-linux-musl",
                        str(Path(tempdir) / "compiler"),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )
            self.assertEqual(
                arm64.sysroot, ROOT / "scripts/alpine/build/aarch64/sysroot/usr"
            )

            armv7 = Bootstrapper(
                parse_args(
                    [
                        "armv7-alpine-linux-musleabihf",
                        str(Path(tempdir) / "compiler"),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )
            self.assertEqual(
                armv7.sysroot, ROOT / "scripts/alpine/build/armv7/sysroot/usr"
            )

    def test_build_tree_is_target_specific(self):
        with tempfile.TemporaryDirectory() as tempdir:
            prefix = Path(tempdir) / "compiler"
            x64 = Bootstrapper(
                parse_args(
                    [
                        "x86_64-pedigree",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )
            arm64 = Bootstrapper(
                parse_args(
                    [
                        "aarch64-linux-musl",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )

            self.assertEqual(
                x64.build_root, (prefix / "build_tmp/x86_64-pedigree").resolve()
            )
            self.assertEqual(
                arm64.build_root, (prefix / "build_tmp/aarch64-linux-musl").resolve()
            )

    def test_manifest_preserves_pinned_toolchain_inputs(self):
        manifest = json.loads(MANIFEST.read_text(encoding="utf-8"))

        expected = {
            "gcc": (
                "15.3.0",
                "fa59c1beef8995f27c4d71c1df227587189315d3e6faff1bb4306e61b0c530eb",
            ),
            "binutils": (
                "2.46.1",
                "e127a709cba24c76de8936cb7083dd768f28cd37eb010492e2f19b71eb1294e4",
            ),
            "gmp": (
                "6.2.1",
                "eae9326beb4158c386e39a356818031bd28f3124cf915f8c5b1dc4c7a36b4d7c",
            ),
            "mpfr": (
                "4.1.0",
                "feced2d430dd5a97805fa289fed3fc8ff2b094c02d05287fd6133e7f1f0ec926",
            ),
            "mpc": (
                "1.2.1",
                "17503d2c395dfcf106b622dc142683c1199431d095367c6aacba6eec30340459",
            ),
            "nasm": (
                "3.02",
                "87336eba53b4acfe917424ab5d500d2b0054d9f5148d35c2273ccf2cfb712f0d",
            ),
        }
        self.assertEqual(set(manifest), set(expected))
        for name, (version, sha256) in expected.items():
            self.assertEqual(manifest[name]["version"], version)
            self.assertEqual(manifest[name]["sha256"], sha256)

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
                    "--libcpp",
                    "--dry-run",
                    "--jobs",
                    "4",
                ],
                check=True,
                capture_output=True,
                text=True,
            )

            self.assertIn("download gcc 15.3.0", result.stdout)
            self.assertIn("download binutils 2.46.1", result.stdout)
            self.assertIn("download gmp 6.2.1", result.stdout)
            self.assertIn("download mpfr 4.1.0", result.stdout)
            self.assertIn("download mpc 1.2.1", result.stdout)
            self.assertIn("download nasm 3.02", result.stdout)
            self.assertIn("--target=x86_64-pedigree", result.stdout)
            self.assertIn("--disable-multilib", result.stdout)
            self.assertIn("--disable-gold", result.stdout)
            self.assertIn(
                "--with-newlib --without-headers --disable-threads --disable-fixincludes",
                result.stdout,
            )
            self.assertIn(
                "--without-newlib --with-headers --enable-threads=posix",
                result.stdout,
            )
            self.assertIn("--disable-shared", result.stdout)
            self.assertIn(
                "--with-gxx-include-dir="
                + str(prefix.resolve() / "include/c++/15.3.0"),
                result.stdout,
            )
            self.assertIn("make -j4 all-gcc all-target-libgcc", result.stdout)
            self.assertIn(
                "make -j4 all-gcc all-target-libgcc all-target-libstdc++-v3",
                result.stdout,
            )
            self.assertIn(
                "make install-gcc install-target-libgcc install-target-libstdc++-v3",
                result.stdout,
            )
            self.assertNotIn("would activate", result.stdout)
            self.assertFalse(prefix.exists())

    def test_linux_musl_stage_one_defers_libgcc_until_libc_is_available(self):
        with tempfile.TemporaryDirectory() as tempdir:
            for target in ("aarch64-linux-musl", "armv7-alpine-linux-musleabihf"):
                with self.subTest(target=target):
                    result = subprocess.run(
                        [
                            sys.executable,
                            str(SCRIPT),
                            target,
                            str(Path(tempdir) / "compiler"),
                            "--source-root",
                            str(ROOT),
                            "--libcpp",
                            "--dry-run",
                            "--jobs",
                            "4",
                        ],
                        check=True,
                        capture_output=True,
                        text=True,
                    )

                    self.assertIn("make -j4 all-gcc (in ", result.stdout)
                    self.assertIn("make install-gcc (in ", result.stdout)
                    self.assertIn(
                        "make -j4 all-gcc all-target-libgcc all-target-libstdc++-v3",
                        result.stdout,
                    )
                    if target == "armv7-alpine-linux-musleabihf":
                        self.assertIn("--with-arch=armv7-a", result.stdout)
                        self.assertIn("--with-fpu=vfpv3-d16", result.stdout)
                        self.assertIn("--with-float=hard", result.stdout)

    def test_activation_is_explicit_and_dry_run_does_not_mutate(self):
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
                    "--activate",
                    "--libcpp",
                    "--dry-run",
                ],
                check=True,
                capture_output=True,
                text=True,
            )

            self.assertIn("would activate", result.stdout)
            self.assertIn("compilers/dir", result.stdout)
            self.assertFalse(prefix.exists())

    def test_final_stage_forces_pic_target_runtimes(self):
        bootstrapper = Bootstrapper(
            parse_args(
                [
                    "x86_64-pedigree",
                    "/tmp/pedigree-toolchain-contract",
                    "--source-root",
                    str(ROOT),
                ]
            )
        )

        stage_one = bootstrapper.gcc_environment(
            with_headers=False, configure=False
        )
        final = bootstrapper.gcc_environment(with_headers=True, configure=False)

        self.assertEqual(stage_one["CFLAGS_FOR_TARGET"], "")
        self.assertEqual(stage_one["CXXFLAGS_FOR_TARGET"], "")
        self.assertEqual(final["CFLAGS_FOR_TARGET"], "-g -O2 -fPIC")
        self.assertEqual(final["CXXFLAGS_FOR_TARGET"], "-g -O2 -fPIC")

    def test_build_and_validation_ignore_ambient_toolchain_overrides(self):
        bootstrapper = Bootstrapper(
            parse_args(
                [
                    "x86_64-pedigree",
                    "/tmp/pedigree-toolchain-contract",
                    "--source-root",
                    str(ROOT),
                ]
            )
        )
        overrides = {
            "C_INCLUDE_PATH": "/legacy/c",
            "COMPILER_PATH": "/legacy/tools",
            "CPATH": "/legacy/include",
            "CPLUS_INCLUDE_PATH": "/legacy/c++/8.3.0",
            "GCC_EXEC_PREFIX": "/legacy/gcc",
            "LIBRARY_PATH": "/legacy/lib",
        }

        with mock.patch.dict(os.environ, overrides):
            build = bootstrapper.environment()
            validation = bootstrapper.validation_environment()

        for name in overrides:
            self.assertNotIn(name, build)
            self.assertNotIn(name, validation)
        self.assertEqual(
            validation["PATH"].split(os.pathsep)[0],
            str(bootstrapper.prefix / "bin"),
        )

    def test_state_fingerprint_controls_stage_and_patch_identity(self):
        with tempfile.TemporaryDirectory() as tempdir:
            prefix = Path(tempdir) / "compiler"
            bootstrapper = Bootstrapper(
                parse_args(
                    [
                        "x86_64-pedigree",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )
            prefix.mkdir()

            bootstrapper.write_state(libcpp=False)
            bootstrapper.sysroot = Path(tempdir) / "missing-sdk"
            with (
                mock.patch.object(bootstrapper, "validate_installation"),
                mock.patch.object(bootstrapper, "link_sysroot") as link_sysroot,
            ):
                self.assertTrue(
                    bootstrapper.installation_current(
                        require_libcpp=False, refresh_sysroot=True
                    )
                )
                self.assertFalse(
                    bootstrapper.installation_current(require_libcpp=True)
                )
                link_sysroot.assert_not_called()

            bootstrapper.write_state(libcpp=True)
            with mock.patch.object(bootstrapper, "validate_installation"):
                self.assertTrue(
                    bootstrapper.installation_current(require_libcpp=True)
                )

            state = json.loads(
                bootstrapper.state_path.read_text(encoding="utf-8")
            )
            state["patches"]["gcc"] = "stale"
            bootstrapper.state_path.write_text(
                json.dumps(state), encoding="utf-8"
            )
            with (
                mock.patch.object(bootstrapper, "validate_installation"),
                mock.patch.object(bootstrapper, "link_sysroot") as link_sysroot,
            ):
                self.assertFalse(
                    bootstrapper.installation_current(
                        require_libcpp=True, refresh_sysroot=True
                    )
                )
                link_sysroot.assert_not_called()

    def test_state_is_kept_separately_for_each_target(self):
        with tempfile.TemporaryDirectory() as tempdir:
            prefix = Path(tempdir) / "compiler"
            x64 = Bootstrapper(
                parse_args(
                    [
                        "x86_64-pedigree",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )
            arm64 = Bootstrapper(
                parse_args(
                    [
                        "aarch64-linux-musl",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )

            x64.write_state(libcpp=False)
            arm64.write_state(libcpp=False)

            self.assertEqual(x64.read_state()["target"], "x86_64-pedigree")
            self.assertEqual(arm64.read_state()["target"], "aarch64-linux-musl")
            self.assertTrue(x64.state_path.is_file())
            self.assertTrue(arm64.state_path.is_file())

    def test_active_prefix_only_blocks_rebuild_of_the_active_target(self):
        with tempfile.TemporaryDirectory() as tempdir:
            temp = Path(tempdir)
            source_root = temp / "source"
            (source_root / "compilers").mkdir(parents=True)
            prefix = temp / "compiler"
            prefix.mkdir()
            (source_root / "compilers/dir").symlink_to(prefix)

            x64 = Bootstrapper(
                parse_args(
                    [
                        "x86_64-pedigree",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )
            x64.write_state(libcpp=False)
            x64.source_root = source_root

            arm64 = Bootstrapper(
                parse_args(
                    [
                        "aarch64-linux-musl",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )
            arm64.source_root = source_root

            self.assertTrue(x64.active_target_needs_rebuild())
            self.assertFalse(arm64.active_target_needs_rebuild())

    def test_atomic_activation_switches_symlinks_and_refuses_directories(self):
        with tempfile.TemporaryDirectory() as tempdir:
            temp = Path(tempdir)
            source_root = temp / "source"
            compilers = source_root / "compilers"
            old_prefix = temp / "old"
            new_prefix = temp / "new"
            compilers.mkdir(parents=True)
            old_prefix.mkdir()
            new_prefix.mkdir()
            link = compilers / "dir"
            link.symlink_to(old_prefix)
            bootstrapper = Bootstrapper(
                parse_args(
                    [
                        "x86_64-pedigree",
                        str(new_prefix),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )
            bootstrapper.source_root = source_root

            with mock.patch.object(
                bootstrapper, "installation_current", return_value=True
            ):
                bootstrapper.activate_prefix()
            self.assertEqual(link.resolve(), new_prefix.resolve())
            self.assertTrue(old_prefix.is_dir())

            link.unlink()
            link.symlink_to(old_prefix)
            with mock.patch.object(
                bootstrapper, "installation_current", return_value=True
            ), mock.patch(
                "scripts.bootstrap_toolchain.os.replace",
                side_effect=OSError("simulated activation failure"),
            ):
                with self.assertRaisesRegex(OSError, "simulated activation failure"):
                    bootstrapper.activate_prefix()
            self.assertEqual(link.resolve(), old_prefix.resolve())
            temporary_link = compilers / f".dir.{os.getpid()}.tmp"
            self.assertFalse(temporary_link.exists())
            self.assertFalse(temporary_link.is_symlink())

            link.unlink()
            link.mkdir()
            with mock.patch.object(
                bootstrapper, "installation_current", return_value=True
            ):
                with self.assertRaisesRegex(
                    RuntimeError, "refusing to replace compiler directory"
                ):
                    bootstrapper.activate_prefix()

    def test_matching_state_still_requires_a_complete_tool_surface(self):
        with tempfile.TemporaryDirectory() as tempdir:
            prefix = Path(tempdir) / "compiler"
            prefix.mkdir()
            bootstrapper = Bootstrapper(
                parse_args(
                    [
                        "x86_64-pedigree",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )
            bootstrapper.write_state(libcpp=False)

            self.assertFalse(
                bootstrapper.installation_current(require_libcpp=False)
            )

    def test_sysroot_linking_exposes_musl_to_the_installed_driver(self):
        with tempfile.TemporaryDirectory() as tempdir:
            temp = Path(tempdir)
            prefix = temp / "compiler"
            sysroot = temp / "musl"
            (sysroot / "usr/include").mkdir(parents=True)
            (sysroot / "include").symlink_to("usr/include")
            (sysroot / "lib").mkdir()
            for name in (
                "crt1.o",
                "rcrt1.o",
                "Scrt1.o",
                "crti.o",
                "crtn.o",
                "libc.a",
                "libc.so",
            ):
                (sysroot / "lib" / name).touch()

            bootstrapper = Bootstrapper(
                parse_args(
                    [
                        "x86_64-pedigree",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                        "--sysroot",
                        str(sysroot),
                    ]
                )
            )
            gcc_startup = (
                prefix / "lib/gcc/x86_64-pedigree/15.3.0/crti.o"
            )
            gcc_startup.parent.mkdir(parents=True)
            gcc_startup.touch()
            with redirect_stdout(io.StringIO()):
                bootstrapper.link_sysroot()

            target = prefix / "x86_64-pedigree"
            self.assertTrue(gcc_startup.is_symlink())
            self.assertEqual(gcc_startup.resolve(), (sysroot / "lib/crti.o").resolve())
            self.assertEqual(
                (target / "include").resolve(), (sysroot / "include").resolve()
            )
            self.assertEqual(
                (target / "lib/libc.a").resolve(), (sysroot / "lib/libc.a").resolve()
            )
            self.assertEqual(
                (target / "lib/libc.so").resolve(), (sysroot / "lib/libc.so").resolve()
            )

            relocated = temp / "alpine/usr"
            (relocated / "include").mkdir(parents=True)
            (relocated / "lib").mkdir()
            for source in (sysroot / "lib").iterdir():
                (relocated / "lib" / source.name).touch()
            (relocated / "lib/libstdc++.a").write_text("Alpine compiler runtime")
            (target / "lib/libstdc++.a").write_text("Pedigree compiler runtime")
            shutil.rmtree(sysroot / "lib")
            gcc_startup.unlink()
            gcc_startup.symlink_to(sysroot / "usr/lib/crti.o")
            bootstrapper.sysroot = relocated
            bootstrapper.args.libcpp = True
            bootstrapper.write_state(libcpp=True)

            def validate_relocated_sdk(*, require_libcpp):
                self.assertTrue(require_libcpp)
                self.assertEqual(
                    (target / "include").resolve(), (relocated / "include").resolve()
                )
                self.assertEqual(
                    gcc_startup.resolve(), (relocated / "lib/crti.o").resolve()
                )
                self.assertTrue((target / "lib/libc.so").is_file())

            with (
                mock.patch.object(bootstrapper, "require_commands"),
                mock.patch.object(
                    bootstrapper,
                    "validate_installation",
                    side_effect=validate_relocated_sdk,
                ) as validate,
                mock.patch.object(
                    bootstrapper, "active_target_needs_rebuild", return_value=True
                ) as needs_rebuild,
                mock.patch.object(bootstrapper, "download") as download,
                redirect_stdout(io.StringIO()),
            ):
                bootstrapper.build()
                validate.assert_called_once_with(require_libcpp=True)
                needs_rebuild.assert_not_called()
                download.assert_not_called()
            self.assertEqual(
                (target / "include").resolve(), (relocated / "include").resolve()
            )
            self.assertEqual(
                gcc_startup.resolve(), (relocated / "lib/crti.o").resolve()
            )
            self.assertEqual(
                (target / "lib/libc.a").resolve(), (relocated / "lib/libc.a").resolve()
            )
            self.assertEqual(
                (target / "lib/libstdc++.a").read_text(), "Pedigree compiler runtime"
            )

            (target / "lib/libc.a").unlink()
            (target / "lib/libc.a").symlink_to(temp / "unmanaged-library.a")
            with (
                self.assertRaisesRegex(
                    RuntimeError, "refusing to replace target library"
                ),
                redirect_stdout(io.StringIO()),
            ):
                bootstrapper.link_sysroot()
            self.assertEqual(
                os.readlink(target / "lib/libc.a"), str(temp / "unmanaged-library.a")
            )

            (target / "lib/libc.a").unlink()
            (target / "lib/libc.a").touch()
            with self.assertRaisesRegex(
                RuntimeError, "refusing to replace target library"
            ):
                with redirect_stdout(io.StringIO()):
                    bootstrapper.link_sysroot()

    def test_libcpp_install_check_is_version_specific(self):
        with tempfile.TemporaryDirectory() as tempdir:
            prefix = Path(tempdir) / "compiler"
            target = prefix / "x86_64-pedigree"
            (target / "lib").mkdir(parents=True)
            (target / "lib/libstdc++.a").touch()

            bootstrapper = Bootstrapper(
                parse_args(
                    [
                        "x86_64-pedigree",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                    ]
                )
            )
            self.assertFalse(bootstrapper.libcpp_installed())

            for header in ("concepts", "memory", "version"):
                (bootstrapper.gxx_include_dir / header).parent.mkdir(
                    parents=True, exist_ok=True
                )
                (bootstrapper.gxx_include_dir / header).touch()
            config = bootstrapper.gxx_include_dir / (
                "x86_64-pedigree/bits/c++config.h"
            )
            config.parent.mkdir(parents=True, exist_ok=True)
            config.touch()
            self.assertTrue(bootstrapper.libcpp_installed())

            (target / "lib64").mkdir()
            (target / "lib/libstdc++.a").rename(target / "lib64/libstdc++.a")
            self.assertTrue(bootstrapper.libcpp_installed())

    def test_libcpp_headers_survive_sysroot_replacement(self):
        with tempfile.TemporaryDirectory() as tempdir:
            temp = Path(tempdir)
            prefix = temp / "compiler"
            sysroot = temp / "musl"
            (sysroot / "include").mkdir(parents=True)

            bootstrapper = Bootstrapper(
                parse_args(
                    [
                        "x86_64-pedigree",
                        str(prefix),
                        "--source-root",
                        str(ROOT),
                        "--sysroot",
                        str(sysroot),
                    ]
                )
            )
            (prefix / "x86_64-pedigree/lib").mkdir(parents=True)
            (prefix / "x86_64-pedigree/lib/libstdc++.a").touch()
            config = (
                bootstrapper.gxx_include_dir
                / "x86_64-pedigree/bits/c++config.h"
            )
            config.parent.mkdir(parents=True)
            config.touch()
            for header in ("concepts", "memory", "version"):
                (bootstrapper.gxx_include_dir / header).touch()

            with redirect_stdout(io.StringIO()):
                bootstrapper.link_sysroot()
            previous_sysroot = temp / "previous-musl"
            sysroot.rename(previous_sysroot)
            (sysroot / "include").mkdir(parents=True)

            self.assertTrue(config.is_file())
            self.assertTrue(bootstrapper.libcpp_installed())
            self.assertEqual(
                (prefix / "x86_64-pedigree/include").resolve(),
                (sysroot / "include").resolve(),
            )

    def test_cmake_does_not_inject_legacy_libstdcxx_headers(self):
        cmake_files = (
            ROOT / "CMakeLists.txt",
            ROOT / "src/modules/CMakeLists.txt",
            ROOT / "src/user/CMakeLists.txt",
        )
        for path in cmake_files:
            contents = path.read_text(encoding="utf-8")
            self.assertNotIn("PEDIGREE_CXX_STDLIB_INCLUDE_DIR", contents)
            self.assertNotIn("support/gcc/include/c++", contents)

    def test_amd64_toolchain_rebinds_compiler_companion_tools(self):
        expected = {
            "CMAKE_ADDR2LINE": "x86_64-pedigree-addr2line",
            "CMAKE_AR": "x86_64-pedigree-ar",
            "CMAKE_ASM_COMPILER": "x86_64-pedigree-gcc",
            "CMAKE_ASM_COMPILER_AR": "x86_64-pedigree-gcc-ar",
            "CMAKE_ASM_COMPILER_RANLIB": "x86_64-pedigree-gcc-ranlib",
            "CMAKE_ASM_NASM_COMPILER": "nasm",
            "CMAKE_C_COMPILER_AR": "x86_64-pedigree-gcc-ar",
            "CMAKE_C_COMPILER_RANLIB": "x86_64-pedigree-gcc-ranlib",
            "CMAKE_CXX_COMPILER_AR": "x86_64-pedigree-gcc-ar",
            "CMAKE_CXX_COMPILER_RANLIB": "x86_64-pedigree-gcc-ranlib",
            "CMAKE_LINKER": "x86_64-pedigree-ld",
            "CMAKE_NM": "x86_64-pedigree-nm",
            "CMAKE_OBJCOPY": "x86_64-pedigree-objcopy",
            "CMAKE_OBJDUMP": "x86_64-pedigree-objdump",
            "CMAKE_RANLIB": "x86_64-pedigree-ranlib",
            "CMAKE_READELF": "x86_64-pedigree-readelf",
            "CMAKE_STRIP": "x86_64-pedigree-strip",
        }

        with tempfile.TemporaryDirectory() as tempdir:
            script = Path(tempdir) / "check-tools.cmake"
            commands = [
                'set(PEDIGREE_TOOLCHAIN_ROOT "/new-toolchain")',
                'set(PEDIGREE_TOOLCHAIN_TRIPLE "x86_64-pedigree")',
                'set(PEDIGREE_TARGET_SYSROOT "/sdk")',
                'set(CMAKE_AR "/old-toolchain/bin/ar" CACHE FILEPATH "" FORCE)',
                f'include("{ROOT / "build-etc/cmake/PedigreeCrossToolchain.cmake"}")',
            ]
            for variable, tool in expected.items():
                commands.extend(
                    [
                        f'if (NOT {variable} STREQUAL "/new-toolchain/bin/{tool}")',
                        f'  message(FATAL_ERROR "Wrong {variable}: ${{{variable}}}")',
                        "endif ()",
                    ]
                )
            script.write_text("\n".join(commands) + "\n")
            subprocess.run(
                ["cmake", "-P", str(script)], check=True, capture_output=True
            )

    def test_easy_build_refreshes_metadata_when_toolchain_changes(self):
        contents = (ROOT / "easy_build_x64.sh").read_text(encoding="utf-8")

        self.assertIn(
            'COMPILER_DIR=$(cd -P -- "$COMPILER_DIR" && pwd -P)', contents
        )
        self.assertIn(
            'PEDIGREE_TOOLCHAIN_ROOT:PATH=$COMPILER_DIR', contents
        )
        self.assertIn('set(CMAKE_SYSTEM_NAME "Pedigree")', contents)
        self.assertIn("cmake -E rm -f build/CMakeCache.txt", contents)
        self.assertIn("cmake -E remove_directory build/CMakeFiles", contents)

    def test_patches_do_not_carry_legacy_regeneration_or_emulations(self):
        gcc_patch = (ROOT / "compilers/pedigree-gcc.patch").read_text(
            encoding="utf-8"
        )
        binutils_patch = (ROOT / "compilers/pedigree-binutils.patch").read_text(
            encoding="utf-8"
        )

        self.assertIn("/usr/lib/ld-musl-x86_64.so.1", gcc_patch)
        self.assertIn("crtbeginT.o", gcc_patch)
        self.assertIn("file_end_indicate_exec_stack", gcc_patch)
        self.assertIn('extra_options="$extra_options gnu-user.opt"', gcc_patch)
        self.assertIn("%{pthread:-D_REENTRANT}", gcc_patch)
        self.assertIn("use_gcc_stdint=wrap", gcc_patch)
        self.assertGreaterEqual(
            gcc_patch.count("*-*-musl* | *-*-pedigree*)"), 3
        )
        self.assertIn("t-gthr-noweak", gcc_patch)
        self.assertNotIn("use_fixproto", gcc_patch)
        self.assertNotIn("config/override.m4", gcc_patch)
        self.assertNotIn("pedigree_x86_64.sh", binutils_patch)
        self.assertNotIn("ld/Makefile.in", binutils_patch)

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
