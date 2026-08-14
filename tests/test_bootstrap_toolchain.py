import io
import json
import os
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
            with mock.patch.object(bootstrapper, "validate_installation"):
                self.assertTrue(
                    bootstrapper.installation_current(require_libcpp=False)
                )
                self.assertFalse(
                    bootstrapper.installation_current(require_libcpp=True)
                )

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
            with mock.patch.object(bootstrapper, "validate_installation"):
                self.assertFalse(
                    bootstrapper.installation_current(require_libcpp=True)
                )

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
            (sysroot / "include").mkdir(parents=True)
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
            with redirect_stdout(io.StringIO()):
                bootstrapper.link_sysroot()

            target = prefix / "x86_64-pedigree"
            self.assertEqual(
                (target / "include").resolve(), (sysroot / "include").resolve()
            )
            self.assertEqual(
                (target / "lib/libc.a").resolve(), (sysroot / "lib/libc.a").resolve()
            )
            self.assertEqual(
                (target / "lib/libc.so").resolve(), (sysroot / "lib/libc.so").resolve()
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

            config = (
                target
                / "include/c++/15.3.0/x86_64-pedigree/bits/c++config.h"
            )
            config.parent.mkdir(parents=True)
            config.touch()
            self.assertTrue(bootstrapper.libcpp_installed())

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
