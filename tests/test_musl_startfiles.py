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
    def _build_probe(self, arch_target):
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
                    include(PedigreeMuslLink)
                    add_executable(probe-app main.c)
                    pedigree_use_musl_sdk_startfiles(probe-app)
                    add_library(probe-shared SHARED shared.c)
                    pedigree_use_musl_sdk_startfiles(probe-shared)
                    """
                )
            )

            configure = subprocess.run(
                [CMAKE, "-S", str(source), "-B", str(build)],
                capture_output=True,
                text=True,
            )
            build_result = subprocess.run(
                [CMAKE, "--build", str(build), "--verbose"],
                capture_output=True,
                text=True,
            )

        output = (
            configure.stdout
            + configure.stderr
            + build_result.stdout
            + build_result.stderr
        )
        self.assertEqual(configure.returncode, 0, msg=output)
        self.assertEqual(build_result.returncode, 0, msg=output)
        return output, f"-B{sdk_prefix.as_posix()}/lib/"

    def test_x64_user_links_prefer_fresh_musl_crt_objects(self):
        output, startfile_option = self._build_probe(arch_target="X64")
        self.assertEqual(output.count(startfile_option), 2, msg=output)

    def test_hosted_links_keep_compiler_startfile_search(self):
        output, startfile_option = self._build_probe(arch_target="HOSTED")
        self.assertNotIn(startfile_option, output)

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
