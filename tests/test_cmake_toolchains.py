import json
import os
import shlex
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
CMAKE = shutil.which("cmake")


@unittest.skipUnless(os.name == "posix" and CMAKE, "requires CMake on POSIX")
class CMakeToolchainTests(unittest.TestCase):
    def _prepare_target_fixture(self, temporary_path):
        source = temporary_path / "source"
        build = temporary_path / "build"
        source.mkdir()
        (source / "build-etc").symlink_to(
            ROOT / "build-etc", target_is_directory=True
        )
        (source / "scripts").symlink_to(
            ROOT / "scripts", target_is_directory=True
        )
        (source / "src").symlink_to(ROOT / "src", target_is_directory=True)

        cmake_lists = (ROOT / "CMakeLists.txt").read_text()
        validation_end = "    add_subdirectory(src/modules)\n"
        self.assertEqual(cmake_lists.count(validation_end), 1)
        cmake_lists = cmake_lists.replace(
            validation_end, "    return()\n\n" + validation_end, 1
        )
        (source / "CMakeLists.txt").write_text(cmake_lists)

        return source, build

    def _target_configure_command(self, source, build, *extra_arguments):
        return [
            CMAKE,
            "-S",
            str(source),
            "-B",
            str(build),
            "-DCMAKE_TOOLCHAIN_FILE="
            + str(ROOT / "build-etc/cmake/pedigree_amd64.cmake"),
            "-DBUILD_TESTING=OFF",
            "-DPEDIGREE_BUILD_HDD_IMAGE=OFF",
            "-DPEDIGREE_BUILD_ISO=OFF",
            "-DPEDIGREE_BUILD_KEYMAPS=OFF",
            "-DPEDIGREE_BUILD_TRANSLATIONS=OFF",
            "-DPEDIGREE_BUILD_USER_DIR=OFF",
            "-DPEDIGREE_REGENERATE_KEYMAP_SOURCES=OFF",
            *extra_arguments,
        ]

    def test_cross_toolchain_loads_pedigree_platform(self):
        compiler = ROOT / "compilers/dir/bin/x86_64-pedigree-gcc"
        if not compiler.is_file():
            self.skipTest("Pedigree cross toolchain is not installed")

        with tempfile.TemporaryDirectory() as temporary:
            source = Path(temporary) / "source"
            build = Path(temporary) / "build"
            source.mkdir()
            (source / "probe.c").write_text("int probe(void) { return 0; }\n")
            (source / "CMakeLists.txt").write_text(
                textwrap.dedent(
                    """\
                    cmake_minimum_required(VERSION 3.21)
                    project(PedigreePlatformProbe LANGUAGES C)
                    if (NOT CMAKE_SYSTEM_NAME STREQUAL "Pedigree")
                        message(FATAL_ERROR "wrong system: ${CMAKE_SYSTEM_NAME}")
                    endif ()
                    if (NOT CMAKE_CROSSCOMPILING OR NOT PEDIGREE OR NOT UNIX)
                        message(FATAL_ERROR "Pedigree cross platform was not loaded")
                    endif ()
                    if (NOT PEDIGREE_ARCH_TARGET STREQUAL "X64" OR
                        NOT PEDIGREE_COMPILER_TARGET STREQUAL "x86_64-pedigree")
                        message(FATAL_ERROR "amd64 target profile was not loaded")
                    endif ()
                    if (NOT CMAKE_SHARED_LIBRARY_CREATE_C_FLAGS MATCHES "-shared")
                        message(FATAL_ERROR "Pedigree ELF rules were not loaded")
                    endif ()
                    add_library(probe STATIC probe.c)
                    """
                )
            )

            result = subprocess.run(
                [
                    CMAKE,
                    "-S",
                    str(source),
                    "-B",
                    str(build),
                    "-DCMAKE_TOOLCHAIN_FILE="
                    + str(ROOT / "build-etc/cmake/pedigree_amd64.cmake"),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                result.returncode, 0, msg=result.stdout + result.stderr
            )

    def test_selfhost_toolchain_selects_native_tools(self):
        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            tool_bin = temporary_path / "tools/bin"
            tool_bin.mkdir(parents=True)
            libgcc_directory = temporary_path / "gcc-runtime"
            libstdcxx_directory = temporary_path / "cxx-runtime"
            libgcc_directory.mkdir()
            libstdcxx_directory.mkdir()
            (libgcc_directory / "libgcc.a").touch()
            (libstdcxx_directory / "libstdc++.a").touch()
            tool = (
                "#!/bin/sh\n"
                'case "$1" in\n'
                "  -dumpmachine) echo x86_64-pedigree ;;\n"
                "  -print-libgcc-file-name) "
                f"echo {libgcc_directory.as_posix()}/libgcc.a ;;\n"
                "  -print-file-name=libstdc++.a) "
                f"echo {libstdcxx_directory.as_posix()}/libstdc++.a ;;\n"
                "esac\n"
                "exit 0\n"
            )
            for name in (
                "gcc",
                "g++",
                "nasm",
                "addr2line",
                "ar",
                "gcc-ar",
                "gcc-ranlib",
                "ld",
                "nm",
                "objcopy",
                "objdump",
                "ranlib",
                "readelf",
                "strip",
            ):
                path = tool_bin / name
                path.write_text(tool)
                path.chmod(0o755)

            probe = temporary_path / "probe.cmake"
            toolchain = ROOT / "build-etc/cmake/pedigree_selfhost.cmake"
            gcc = tool_bin / "gcc"
            gxx = tool_bin / "g++"
            probe.write_text(
                textwrap.dedent(
                    f"""\
                    set(CMAKE_HOST_SYSTEM_NAME Pedigree)
                    set(PEDIGREE_NATIVE_TOOL_ROOT "{tool_bin.parent.as_posix()}"
                        CACHE PATH "")
                    include("{toolchain.as_posix()}")
                    if (DEFINED CMAKE_SYSTEM_NAME OR DEFINED CMAKE_SYSROOT)
                        message(FATAL_ERROR "selfhost toolchain forced cross state")
                    endif ()
                    if (NOT PEDIGREE_C_COMPILER STREQUAL "{gcc.as_posix()}" OR
                        NOT PEDIGREE_CXX_COMPILER STREQUAL "{gxx.as_posix()}")
                        message(FATAL_ERROR "native compilers were not selected")
                    endif ()
                    if (NOT PEDIGREE_ARCH_TARGET STREQUAL "X64" OR
                        NOT PEDIGREE_COMPILER_TARGET STREQUAL "x86_64-pedigree")
                        message(FATAL_ERROR "amd64 target profile was not loaded")
                    endif ()
                    set(expected_runtime_directories
                        "{libgcc_directory.as_posix()}"
                        "{libstdcxx_directory.as_posix()}")
                    if (NOT PEDIGREE_NATIVE_RUNTIME_LIBRARY_DIRS STREQUAL
                        expected_runtime_directories)
                        message(FATAL_ERROR
                            "wrong native runtime directories: "
                            "${{PEDIGREE_NATIVE_RUNTIME_LIBRARY_DIRS}}")
                    endif ()
                    set(CMAKE_SYSTEM_PROCESSOR x64-mach_pc)
                    include("{(ROOT / 'build-etc/cmake/Platform/Pedigree-Determine.cmake').as_posix()}")
                    if (NOT CMAKE_SYSTEM_PROCESSOR STREQUAL "x86_64")
                        message(FATAL_ERROR "Pedigree processor was not normalized")
                    endif ()
                    """
                )
            )

            result = subprocess.run(
                [CMAKE, "-P", str(probe)], capture_output=True, text=True
            )
            self.assertEqual(
                result.returncode, 0, msg=result.stdout + result.stderr
            )

    def test_cross_build_owns_incremental_native_configdb_generator(self):
        compiler = ROOT / "compilers/dir/bin/x86_64-pedigree-gcc"
        if not compiler.is_file():
            self.skipTest("Pedigree cross toolchain is not installed")

        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            source, build = self._prepare_target_fixture(temporary_path)
            configure_result = subprocess.run(
                self._target_configure_command(source, build),
                capture_output=True,
                text=True,
            )
            first_build_result = subprocess.run(
                [
                    CMAKE,
                    "--build",
                    str(build),
                    "--target",
                    "configdb",
                    "--parallel",
                    "2",
                    "--verbose",
                ],
                capture_output=True,
                text=True,
            )
            first_output = (
                configure_result.stdout
                + configure_result.stderr
                + first_build_result.stdout
                + first_build_result.stderr
            )
            self.assertEqual(configure_result.returncode, 0, msg=first_output)
            self.assertEqual(
                first_build_result.returncode,
                0,
                msg=first_output,
            )

            config_db = build / "config.db"
            staged_generators = list(
                (build / "host-tools/bin").glob("*/pedigree-configdb")
            )
            self.assertEqual(len(staged_generators), 1, msg=first_output)
            self.assertTrue(staged_generators[0].is_file(), msg=first_output)
            self.assertGreater(config_db.stat().st_size, 0)

            host_caches = list(
                (build / "host-tools/build").glob("**/CMakeCache.txt")
            )
            self.assertTrue(host_caches, msg=first_output)
            host_cache = "\n".join(
                cache.read_text(errors="replace") for cache in host_caches
            )
            parent_cache = (build / "CMakeCache.txt").read_text(
                errors="replace"
            )
            self.assertNotIn(compiler.resolve().as_posix(), host_cache)
            self.assertNotIn("CMAKE_CXX_COMPILER", host_cache)
            self.assertNotIn(
                "PEDIGREE_BUILD_HOST_CXX_COMPILER", parent_cache
            )
            self.assertNotIn("musl/usr/include", first_output)
            self.assertNotIn("Python", first_output)
            self.assertNotIn("CMP0148", first_output)

            config_db_mtime = config_db.stat().st_mtime_ns
            cmake_lists = source / "CMakeLists.txt"
            cmake_lists.write_text(cmake_lists.read_text() + "\n")
            second_build_result = subprocess.run(
                [
                    CMAKE,
                    "--build",
                    str(build),
                    "--target",
                    "configdb",
                    "--parallel",
                    "2",
                    "--verbose",
                ],
                capture_output=True,
                text=True,
            )
            second_output = (
                second_build_result.stdout + second_build_result.stderr
            )
            second_config_db_mtime = config_db.stat().st_mtime_ns

        self.assertEqual(
            second_build_result.returncode,
            0,
            msg=second_output,
        )
        self.assertIn("Pedigree build role: TARGET", second_output)
        self.assertEqual(config_db_mtime, second_config_db_mtime)
        self.assertNotIn("Python", second_output)
        self.assertNotIn("CMP0148", second_output)

    def test_nested_host_compiler_switch_uses_keyed_stage(self):
        compiler = ROOT / "compilers/dir/bin/x86_64-pedigree-gcc"
        if not compiler.is_file():
            self.skipTest("Pedigree cross toolchain is not installed")

        native_cc = shutil.which("cc") or shutil.which("clang")
        if not native_cc:
            self.skipTest("requires a native C compiler")
        native_cc = Path(native_cc).resolve()

        if shutil.which("ninja"):
            generator_arguments = ("-G", "Ninja")
            rules_path = Path("build.ninja")
        elif shutil.which("make"):
            generator_arguments = ("-G", "Unix Makefiles")
            rules_path = Path("CMakeFiles/configdb.dir/build.make")
        else:
            self.skipTest("requires Ninja or Make to inspect generated rules")

        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            source, build = self._prepare_target_fixture(temporary_path)
            wrapped_cc = temporary_path / "wrapped-cc"
            wrapped_cc.write_text(
                "#!/bin/sh\n"
                f"exec {shlex.quote(str(native_cc))} \"$@\"\n"
            )
            wrapped_cc.chmod(0o755)

            def configure(host_compiler):
                result = subprocess.run(
                    self._target_configure_command(
                        source,
                        build,
                        *generator_arguments,
                        "-DPEDIGREE_BUILD_HOST_C_COMPILER:FILEPATH="
                        + str(host_compiler),
                    ),
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(
                    result.returncode,
                    0,
                    msg=result.stdout + result.stderr,
                )

            def build_configdb():
                result = subprocess.run(
                    [
                        CMAKE,
                        "--build",
                        str(build),
                        "--target",
                        "configdb",
                        "--parallel",
                        "2",
                        "--verbose",
                    ],
                    capture_output=True,
                    text=True,
                )
                self.assertEqual(
                    result.returncode,
                    0,
                    msg=result.stdout + result.stderr,
                )
                return result.stdout + result.stderr

            configure(native_cc)
            build_configdb()
            stage_a_generators = list(
                (build / "host-tools/bin").glob("*/pedigree-configdb")
            )
            self.assertEqual(len(stage_a_generators), 1)
            stage_a_generator = stage_a_generators[0]

            (build / "config.db").unlink()
            configure(wrapped_cc)
            stage_b_output = build_configdb()
            staged_generators = set(
                (build / "host-tools/bin").glob("*/pedigree-configdb")
            )
            self.assertEqual(len(staged_generators), 2, msg=stage_b_output)
            stage_b_generator = (staged_generators - {stage_a_generator}).pop()
            self.assertNotEqual(
                stage_a_generator.parent, stage_b_generator.parent
            )

            rules = (build / rules_path).read_text(errors="replace")
            self.assertIn(str(stage_b_generator), rules)
            self.assertNotIn(str(stage_a_generator), rules)
            self.assertIn(str(stage_b_generator), stage_b_output)
            self.assertNotIn(str(stage_a_generator), stage_b_output)

            (build / "config.db").unlink()
            configure(native_cc)
            rules = (build / rules_path).read_text(errors="replace")
            self.assertIn(str(stage_a_generator), rules)
            self.assertNotIn(str(stage_b_generator), rules)
            stage_a_output = build_configdb()
            self.assertIn(str(stage_a_generator), stage_a_output)
            self.assertNotIn(str(stage_b_generator), stage_a_output)
            self.assertGreater((build / "config.db").stat().st_size, 0)

    def test_musl_headers_are_target_only_and_ordered_for_make_and_ninja(self):
        root_cmake = (ROOT / "CMakeLists.txt").read_text()
        modules_cmake = (ROOT / "src/modules/CMakeLists.txt").read_text()
        kernel_cmake = (ROOT / "src/system/kernel/CMakeLists.txt").read_text()
        user_cmake = (ROOT / "src/user/CMakeLists.txt").read_text()
        self.assertIn(
            "target_link_libraries(libkeymap PRIVATE pedigree_musl_headers)",
            root_cmake,
        )
        self.assertIn(
            'if (PEDIGREE_ARCH_TARGET STREQUAL "X64")\n'
            "    link_libraries(pedigree_musl_headers)",
            modules_cmake,
        )
        self.assertIn(
            'if (PEDIGREE_ARCH_TARGET STREQUAL "X64")\n'
            "    link_libraries(pedigree_musl_headers)",
            kernel_cmake,
        )
        self.assertIn(
            'if (PEDIGREE_ARCH_TARGET STREQUAL "X64")\n'
            "    link_libraries(pedigree_musl_headers)",
            user_cmake,
        )

        generators = []
        if shutil.which("make"):
            generators.append("Unix Makefiles")
        if shutil.which("ninja"):
            generators.append("Ninja")
        if not generators:
            self.skipTest("requires Make or Ninja")

        for generator in generators:
            with self.subTest(generator=generator):
                with tempfile.TemporaryDirectory() as temporary:
                    temporary_path = Path(temporary)
                    source = temporary_path / "source"
                    build = temporary_path / "build"
                    project_include = source / "project-include"
                    source.mkdir()
                    project_include.mkdir()
                    (project_include / "project.h").write_text(
                        "#define PROJECT_HEADER 1\n"
                    )
                    for name in (
                        "libkeymap",
                        "module",
                        "kernel",
                    ):
                        (source / f"{name}.c").write_text(
                            "#include <project.h>\n"
                            "#include <fresh-musl.h>\n"
                            f"int {name}(void) {{ return PROJECT_HEADER; }}\n"
                        )
                    (source / "user_app.c").write_text(
                        "#include <project.h>\n"
                        "#include <fresh-musl.h>\n"
                        "#include <sdk-choice.h>\n"
                        "#include <fallback-only.h>\n"
                        "int main(void) { return PROJECT_HEADER - "
                        "SDK_CHOICE + FALLBACK_ONLY; }\n"
                    )
                    (source / "user_library.cc").write_text(
                        "#include <project.h>\n"
                        "#include <fresh-musl.h>\n"
                        "int user_library() { return PROJECT_HEADER; }\n"
                    )
                    for name in ("host_generator", "hosted_kernel"):
                        (source / f"{name}.c").write_text(
                            "#include <project.h>\n"
                            f"int {name}(void) {{ return PROJECT_HEADER; }}\n"
                        )

                    (source / "CMakeLists.txt").write_text(
                        textwrap.dedent(
                            f"""\
                            cmake_minimum_required(VERSION 3.21)
                            project(MuslHeaderBoundary LANGUAGES C CXX)
                            set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
                            list(APPEND CMAKE_MODULE_PATH
                                "{(ROOT / 'build-etc/cmake').as_posix()}")
                            set(PEDIGREE_MUSL_PREFIX_ROOT
                                "${{CMAKE_BINARY_DIR}}/musl/usr")
                            set(PEDIGREE_SELF_HOSTED ON)
                            set(PEDIGREE_MUSL_INSTALLED_INCLUDE_DIR
                                "${{CMAKE_BINARY_DIR}}/installed/usr/include")
                            file(MAKE_DIRECTORY
                                "${{PEDIGREE_MUSL_INSTALLED_INCLUDE_DIR}}")
                            file(WRITE
                                "${{PEDIGREE_MUSL_INSTALLED_INCLUDE_DIR}}/sdk-choice.h"
                                "#error installed libc header won precedence\n")
                            file(WRITE
                                "${{PEDIGREE_MUSL_INSTALLED_INCLUDE_DIR}}/fallback-only.h"
                                "#define FALLBACK_ONLY 0\n")
                            file(WRITE "${{CMAKE_BINARY_DIR}}/sdk-choice.h"
                                "#define SDK_CHOICE 1\n")
                            include(PedigreeMuslLink)
                            pedigree_define_musl_sdk_headers()

                            add_library(host_generator STATIC host_generator.c)
                            add_library(hosted_kernel STATIC hosted_kernel.c)
                            target_include_directories(host_generator PRIVATE
                                "${{CMAKE_SOURCE_DIR}}/project-include")
                            target_include_directories(hosted_kernel PRIVATE
                                "${{CMAKE_SOURCE_DIR}}/project-include")

                            add_library(libkeymap STATIC libkeymap.c)
                            target_include_directories(libkeymap PRIVATE
                                "${{CMAKE_SOURCE_DIR}}/project-include")
                            target_link_libraries(
                                libkeymap PRIVATE pedigree_musl_headers)

                            link_libraries(pedigree_musl_headers)
                            add_library(module STATIC module.c)
                            add_library(kernel STATIC kernel.c)
                            add_executable(user_app user_app.c)
                            add_library(user_library SHARED user_library.cc)
                            target_include_directories(module PRIVATE
                                "${{CMAKE_SOURCE_DIR}}/project-include")
                            target_include_directories(kernel PRIVATE
                                "${{CMAKE_SOURCE_DIR}}/project-include")
                            target_include_directories(user_app PRIVATE
                                "${{CMAKE_SOURCE_DIR}}/project-include")
                            target_include_directories(user_library PRIVATE
                                "${{CMAKE_SOURCE_DIR}}/project-include")

                            add_custom_command(
                                OUTPUT
                                    "${{PEDIGREE_MUSL_PREFIX_ROOT}}/include/fresh-musl.h"
                                    "${{PEDIGREE_MUSL_PREFIX_ROOT}}/include/sdk-choice.h"
                                COMMAND ${{CMAKE_COMMAND}} -E make_directory
                                    "${{PEDIGREE_MUSL_PREFIX_ROOT}}/include"
                                COMMAND ${{CMAKE_COMMAND}} -E touch
                                    "${{PEDIGREE_MUSL_PREFIX_ROOT}}/include/fresh-musl.h"
                                COMMAND ${{CMAKE_COMMAND}} -E copy
                                    "${{CMAKE_BINARY_DIR}}/sdk-choice.h"
                                    "${{PEDIGREE_MUSL_PREFIX_ROOT}}/include/sdk-choice.h")
                            add_custom_target(libc DEPENDS
                                "${{PEDIGREE_MUSL_PREFIX_ROOT}}/include/fresh-musl.h"
                                "${{PEDIGREE_MUSL_PREFIX_ROOT}}/include/sdk-choice.h")
                            add_dependencies(pedigree_musl_headers libc)
                            """
                        )
                    )

                    configure_result = subprocess.run(
                        [
                            CMAKE,
                            "-S",
                            str(source),
                            "-B",
                            str(build),
                            "-G",
                            generator,
                        ],
                        capture_output=True,
                        text=True,
                    )
                    build_result = subprocess.run(
                        [
                            CMAKE,
                            "--build",
                            str(build),
                            "--parallel",
                            "2",
                        ],
                        capture_output=True,
                        text=True,
                    )
                    output = (
                        configure_result.stdout
                        + configure_result.stderr
                        + build_result.stdout
                        + build_result.stderr
                    )
                    self.assertEqual(
                        configure_result.returncode, 0, msg=output
                    )
                    self.assertEqual(build_result.returncode, 0, msg=output)

                    database = json.loads(
                        (build / "compile_commands.json").read_text()
                    )
                    commands = {}
                    for entry in database:
                        filename = Path(entry["file"]).name
                        command = entry.get("arguments")
                        if command is None:
                            command = shlex.split(entry["command"])
                        commands[filename] = command

                    fresh_flag = (
                        "-isystem" + (build / "musl/usr/include").as_posix()
                    )
                    fallback_flag = (
                        "-idirafter"
                        + (build / "installed/usr/include").as_posix()
                    )
                    project_flag = "-I" + project_include.as_posix()
                    for filename in (
                        "libkeymap.c",
                        "module.c",
                        "kernel.c",
                        "user_app.c",
                        "user_library.cc",
                    ):
                        command = commands[filename]
                        self.assertEqual(command.count(fresh_flag), 1)
                        self.assertLess(
                            command.index(project_flag),
                            command.index(fresh_flag),
                        )
                        self.assertEqual(command.count(fallback_flag), 1)
                        self.assertLess(
                            command.index(fresh_flag),
                            command.index(fallback_flag),
                        )
                    for filename in ("host_generator.c", "hosted_kernel.c"):
                        self.assertFalse(
                            any(
                                "musl/usr/include" in argument
                                for argument in commands[filename]
                            )
                        )

    def test_imported_mode_rejects_stale_host_utilities_export(self):
        compiler = ROOT / "compilers/dir/bin/x86_64-pedigree-gcc"
        if not compiler.is_file():
            self.skipTest("Pedigree cross toolchain is not installed")

        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            source, build = self._prepare_target_fixture(temporary_path)
            host_utilities = temporary_path / "HostUtilities.cmake"
            host_utilities.write_text(
                textwrap.dedent(
                    f"""\
                    add_executable(host-pedigree-initrd-builder IMPORTED)
                    set_target_properties(
                        host-pedigree-initrd-builder PROPERTIES
                        IMPORTED_LOCATION "{Path(CMAKE).as_posix()}")
                    """
                )
            )
            result = subprocess.run(
                self._target_configure_command(
                    source,
                    build,
                    "-DPEDIGREE_HOST_TOOLS_MODE=IMPORTED",
                    "-DIMPORT_EXECUTABLES=" + str(host_utilities),
                ),
                capture_output=True,
                text=True,
            )

        output = result.stdout + result.stderr
        normalized_output = " ".join(output.split())
        self.assertNotEqual(result.returncode, 0, msg=output)
        self.assertIn("host-pedigree-configdb", normalized_output)
        self.assertIn(
            "Reconfigure and rebuild the HOST_TOOLS tree", normalized_output
        )
        self.assertIn("-DPEDIGREE_HOST_TOOLS_MODE=NESTED", normalized_output)
        self.assertNotIn("falling back to Python", normalized_output)
        self.assertNotIn("CMP0148", output)


if __name__ == "__main__":
    unittest.main()
