import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path


SOURCE_ROOT = Path(__file__).resolve().parents[1]
GENERATE = SOURCE_ROOT / "build-etc/cmake/GenerateMuslSdkManifest.cmake"
VALIDATE = SOURCE_ROOT / "build-etc/cmake/ValidateMuslSdkManifest.cmake"
SCHEMA = SOURCE_ROOT / "build-etc/toolchain/musl-sdk-manifest-v1.schema.json"

LIBRARIES = (
    "libc.so",
    "libc.a",
    "libcrypt.a",
    "libdl.a",
    "libm.a",
    "libpthread.a",
    "libresolv.a",
    "librt.a",
    "libutil.a",
    "libxnet.a",
)
CRT_OBJECTS = ("crt1.o", "rcrt1.o", "Scrt1.o", "crti.o", "crtn.o")
HEADERS = (
    "stdio.h",
    "stdlib.h",
    "stdint.h",
    "unistd.h",
    "bits/syscall.h",
    "sys/syscall.h",
)


class MuslSdkManifestTests(unittest.TestCase):
    def make_sdk_root(self, base):
        root = Path(base) / "root"
        library_dir = root / "usr/lib"
        include_dir = root / "usr/include"
        library_dir.mkdir(parents=True)
        include_dir.mkdir(parents=True)
        for filename in LIBRARIES + CRT_OBJECTS:
            (library_dir / filename).write_bytes(filename.encode("ascii"))
        for filename in HEADERS:
            path = include_dir / filename
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_text("/* {} */\n".format(filename), encoding="utf-8")
        os.symlink("libc.so", library_dir / "ld-musl-x86_64.so.1")
        return root

    def run_cmake(self, script, definitions, check=True):
        command = ["cmake"]
        command.extend(
            "-D{}={}".format(key, value) for key, value in definitions.items()
        )
        command.extend(("-P", str(script)))
        return subprocess.run(
            command,
            check=check,
            capture_output=True,
            text=True,
        )

    def generate(self, root, manifest, overrides=None, check=True):
        definitions = {
            "SDK_ROOT": root,
            "OUTPUT": manifest,
            "MUSL_VERSION": "1.2.6",
            "PORT_REVISION": "3",
            "ARCHITECTURE": "x86_64",
            "ABI": "pedigree-x86_64-syscall-v1",
            "LAYOUT": "fhs-usr-v1",
            "PROFILE": "target",
            "PAGE_SIZE": "4096",
            "DT_RELR": "ON",
            "UPSTREAM_SHA256": "A" * 64,
            "PEDIGREE_REVISION": "deadbeef",
            "BUILD_ID": "local:0123456789abcdef",
            "COMPILER_TARGET": "x86_64-pedigree",
            "COMPILER_VERSION": "15.3.0",
        }
        definitions.update(overrides or {})
        return self.run_cmake(
            GENERATE,
            definitions,
            check=check,
        )

    def validate(self, root, manifest, expected_build_id=None, check=True):
        definitions = {"SDK_ROOT": root, "MANIFEST": manifest}
        if expected_build_id is not None:
            definitions["EXPECTED_BUILD_ID"] = expected_build_id
        return self.run_cmake(
            VALIDATE,
            definitions,
            check=check,
        )

    def test_generates_and_validates_package_shaped_sdk(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sdk_root(temporary)
            manifest = Path(temporary) / "musl-sdk.json"

            self.generate(root, manifest)
            self.generate(root, manifest)
            self.validate(root, manifest)

            contents = json.loads(manifest.read_text(encoding="utf-8"))
            schema = json.loads(SCHEMA.read_text(encoding="utf-8"))
            self.assertEqual(contents["schema"], "pedigree.musl-sdk")
            self.assertEqual(contents["schema_version"], 1)
            self.assertEqual(
                schema["properties"]["schema"]["const"], contents["schema"]
            )
            self.assertEqual(
                schema["properties"]["schema_version"]["const"],
                contents["schema_version"],
            )
            self.assertEqual(
                contents["component"],
                {"name": "musl", "version": "1.2.6", "port_revision": 3},
            )
            self.assertEqual(contents["target"]["architecture"], "x86_64")
            self.assertEqual(contents["target"]["profile"], "target")
            self.assertEqual(contents["target"]["page_size"], 4096)
            self.assertIs(contents["target"]["dt_relr"], True)
            self.assertEqual(contents["layout"]["name"], "fhs-usr-v1")
            self.assertEqual(
                contents["layout"]["dynamic_loader_target"], "libc.so"
            )
            self.assertEqual(
                contents["provenance"]["upstream_sha256"], "a" * 64
            )
            self.assertEqual(
                contents["provenance"]["compiler_target"], "x86_64-pedigree"
            )
            self.assertEqual(contents["provenance"]["compiler_version"], "15.3.0")

    def test_hosted_derivation_keeps_version_in_build_identity(self):
        modules_cmake = (
            SOURCE_ROOT / "build-etc/cmake/PedigreeHostedMusl.cmake"
        ).read_text(encoding="utf-8")
        self.assertIn('set(MUSL_VERSION "1.2.6")', modules_cmake)
        build_id = modules_cmake.split(
            "string(CONCAT _PEDIGREE_MUSL_BUILD_ID_INPUT", 1
        )[1].split("string(SHA256 PEDIGREE_MUSL_BUILD_ID", 1)[0]
        self.assertIn("${MUSL_VERSION}", build_id)

    def test_rejects_missing_runtime_crt_and_header_files(self):
        cases = (
            ("usr/lib/libc.so", "runtime file is missing"),
            ("usr/lib/crt1.o", "CRT object is missing"),
            ("usr/include/unistd.h", "header is missing"),
        )
        for relative, expected in cases:
            with (
                self.subTest(relative=relative),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = self.make_sdk_root(temporary)
                (root / relative).unlink()
                manifest = Path(temporary) / "musl-sdk.json"

                result = self.generate(root, manifest, check=False)

                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stdout + result.stderr)
                self.assertFalse(manifest.exists())

    def test_rejects_absolute_loader_target(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sdk_root(temporary)
            loader = root / "usr/lib/ld-musl-x86_64.so.1"
            loader.unlink()
            os.symlink("/usr/lib/libc.so", loader)

            result = self.generate(
                root, Path(temporary) / "musl-sdk.json", check=False
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "dynamic loader target must be relative",
                result.stdout + result.stderr,
            )

    def test_rejects_loader_that_does_not_resolve_to_in_root_libc(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sdk_root(temporary)
            wrong_libc = root / "usr/wrong-libc.so"
            wrong_libc.write_bytes(b"wrong")
            loader = root / "usr/lib/ld-musl-x86_64.so.1"
            loader.unlink()
            os.symlink("../wrong-libc.so", loader)

            result = self.generate(
                root, Path(temporary) / "musl-sdk.json", check=False
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "dynamic loader must resolve to in-root libc.so",
                result.stdout + result.stderr,
            )

    def test_generator_rejects_invalid_page_size_and_dt_relr(self):
        cases = (
            ({"PAGE_SIZE": "6000"}, "PAGE_SIZE must be a power of two"),
            ({"DT_RELR": "sometimes"}, "DT_RELR must be a boolean value"),
        )
        for overrides, expected in cases:
            with (
                self.subTest(overrides=overrides),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = self.make_sdk_root(temporary)
                result = self.generate(
                    root,
                    Path(temporary) / "musl-sdk.json",
                    overrides=overrides,
                    check=False,
                )

                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stdout + result.stderr)

    def test_validator_rejects_invalid_target_build_settings(self):
        cases = (
            ("page_size", 6000, "target.page_size must be a power of two"),
            ("dt_relr", "true", "target.dt_relr must be a boolean"),
        )
        for field, value, expected in cases:
            with (
                self.subTest(field=field),
                tempfile.TemporaryDirectory() as temporary,
            ):
                root = self.make_sdk_root(temporary)
                manifest = Path(temporary) / "musl-sdk.json"
                self.generate(root, manifest)
                contents = json.loads(manifest.read_text(encoding="utf-8"))
                contents["target"][field] = value
                manifest.write_text(json.dumps(contents), encoding="utf-8")

                result = self.validate(root, manifest, check=False)

                self.assertNotEqual(result.returncode, 0)
                self.assertIn(expected, result.stdout + result.stderr)

    def test_validator_requires_string_compiler_provenance(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sdk_root(temporary)
            manifest = Path(temporary) / "musl-sdk.json"
            self.generate(root, manifest)
            contents = json.loads(manifest.read_text(encoding="utf-8"))
            contents["provenance"]["compiler_version"] = 15.3
            manifest.write_text(json.dumps(contents), encoding="utf-8")

            result = self.validate(root, manifest, check=False)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "provenance.compiler_version must be a string",
                result.stdout + result.stderr,
            )

    def test_validator_can_require_the_current_derivation(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sdk_root(temporary)
            manifest = Path(temporary) / "musl-sdk.json"
            build_id = "local:0123456789abcdef"
            self.generate(root, manifest, overrides={"BUILD_ID": build_id})

            self.validate(root, manifest, expected_build_id=build_id)
            result = self.validate(
                root,
                manifest,
                expected_build_id="local:different",
                check=False,
            )

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "build ID does not match the requested derivation",
                result.stdout + result.stderr,
            )

    def test_validator_rejects_manifest_root_mismatch(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sdk_root(temporary)
            manifest = Path(temporary) / "musl-sdk.json"
            self.generate(root, manifest)
            contents = json.loads(manifest.read_text(encoding="utf-8"))
            contents["layout"]["dynamic_loader_target"] = "other-libc.so"
            manifest.write_text(json.dumps(contents), encoding="utf-8")

            result = self.validate(root, manifest, check=False)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "manifest loader target does not match the SDK root",
                result.stdout + result.stderr,
            )

    def test_validator_rejects_fields_outside_the_schema(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = self.make_sdk_root(temporary)
            manifest = Path(temporary) / "musl-sdk.json"
            self.generate(root, manifest)
            contents = json.loads(manifest.read_text(encoding="utf-8"))
            contents["mutable_source_root"] = "/workspace/pedigree"
            manifest.write_text(json.dumps(contents), encoding="utf-8")

            result = self.validate(root, manifest, check=False)

            self.assertNotEqual(result.returncode, 0)
            self.assertIn(
                "manifest has unexpected members", result.stdout + result.stderr
            )


if __name__ == "__main__":
    unittest.main()
