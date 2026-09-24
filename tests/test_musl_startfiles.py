import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
CMAKE = shutil.which("cmake")


@unittest.skipUnless(CMAKE, "requires CMake")
class MuslStartfileTests(unittest.TestCase):
    def _configure_probe(self, arch_target, loader_arch="x86_64"):
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            source = temporary_path / "source"
            build = temporary_path / "build"
            sdk_prefix = temporary_path / "musl/usr"
            source.mkdir()
            (sdk_prefix / "lib").mkdir(parents=True)
            (source / "main.c").write_text("int main(void) { return 0; }\n")
            (source / "shared.c").write_text("int answer(void) { return 42; }\n")
            (source / "CMakeLists.txt").write_text(
                textwrap.dedent(
                    f"""\
                    cmake_minimum_required(VERSION 3.21)
                    project(MuslStartfileProbe LANGUAGES C)
                    list(APPEND CMAKE_MODULE_PATH
                        "{(ROOT / 'build-etc/cmake').as_posix()}")
                    set(PEDIGREE_ARCH_TARGET {arch_target})
                    set(PEDIGREE_MUSL_PREFIX_ROOT "{sdk_prefix.as_posix()}")
                    set(PEDIGREE_MUSL_DYNAMIC_LOADER "/lib/ld-musl-{loader_arch}.so.1")
                    include(PedigreeMuslLink)
                    add_executable(probe-app main.c)
                    pedigree_use_musl_sdk_startfiles(probe-app)
                    add_library(probe-shared SHARED shared.c)
                    pedigree_use_musl_sdk_startfiles(probe-shared)
                    get_target_property(app_options probe-app LINK_OPTIONS)
                    get_target_property(shared_options probe-shared LINK_OPTIONS)
                    file(WRITE "${{CMAKE_BINARY_DIR}}/link-options.txt"
                        "${{app_options}}\n${{shared_options}}\n")
                    """
                )
            )

            configure = subprocess.run(
                [CMAKE, "-S", str(source), "-B", str(build)],
                capture_output=True,
                text=True,
                check=False,
            )
            self.assertEqual(configure.returncode, 0, msg=configure.stdout + configure.stderr)
            output = (build / "link-options.txt").read_text()
        return output, f"-B{sdk_prefix.as_posix()}/lib/"

    def test_target_user_links_use_sdk_crt_and_executable_interpreter(self):
        for target, loader in (("X64", "x86_64"), ("ARM64", "aarch64"), ("ARMV7", "armhf")):
            with self.subTest(target=target):
                output, startfile_option = self._configure_probe(target, loader)
                self.assertEqual(output.count(startfile_option), 2, msg=output)
                app_options, shared_options = output.splitlines()
                interpreter = f"-Wl,--dynamic-linker,/lib/ld-musl-{loader}.so.1"
                self.assertIn(interpreter, app_options)
                self.assertNotIn("dynamic-linker", shared_options)

    def test_hosted_links_keep_compiler_startfile_search(self):
        output, startfile_option = self._configure_probe(arch_target="HOSTED")
        self.assertNotIn(startfile_option, output)
        self.assertNotIn("dynamic-linker", output)

    def test_user_targets_apply_the_startfile_policy(self):
        user_cmake = (ROOT / "src/user/CMakeLists.txt").read_text()
        modules_cmake = (ROOT / "src/modules/CMakeLists.txt").read_text()

        for target in ("app-${name}", "lib${name}"):
            self.assertIn(
                f"pedigree_use_musl_sdk_startfiles({target})", user_cmake
            )
        for target in ("native-user", "pedigree-c-user"):
            self.assertIn(
                f"pedigree_use_musl_sdk_startfiles({target})", modules_cmake
            )


if __name__ == "__main__":
    unittest.main()
