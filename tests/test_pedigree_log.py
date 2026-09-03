import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
USER = ROOT / "src/user"
MUSL = ROOT / "src/modules/subsys/posix/musl"
PEDIGREE_C = ROOT / "src/modules/subsys/pedigree-c"


class PedigreeLogBoundaryTests(unittest.TestCase):
    def test_musl_does_not_provide_the_pedigree_log_api(self):
        self.assertFalse((MUSL / "klog.h").exists())

        glue = (MUSL / "glue-musl.c").read_text(encoding="utf-8")
        self.assertNotRegex(glue, r"\bklog\s*\(")
        self.assertNotIn("POSIX_SYSLOG", glue)

        build_script = (ROOT / "scripts/build-musl-amd64.sh").read_text(
            encoding="utf-8"
        )
        modules_cmake = (ROOT / "src/modules/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        self.assertNotIn("musl/klog.h", build_script)
        self.assertNotIn("subsys/posix/musl/klog.h", modules_cmake)

    def test_userspace_calls_the_namespaced_library_api(self):
        old_includes = []
        old_calls = []
        new_calls = 0
        for source in USER.rglob("*"):
            if source.suffix not in {".c", ".cc", ".cpp", ".h", ".hpp"}:
                continue
            contents = source.read_text(encoding="utf-8")
            if "<sys/klog.h>" in contents:
                old_includes.append(source.relative_to(ROOT).as_posix())
            if re.search(r"\bklog\s*\(", contents):
                old_calls.append(source.relative_to(ROOT).as_posix())
            new_calls += len(re.findall(r"\bpedigree_log\s*\(", contents))

        self.assertEqual(old_includes, [])
        self.assertEqual(old_calls, [])
        self.assertGreater(new_calls, 200)

    def test_libpedigree_c_owns_the_api_for_target_and_hosted_builds(self):
        header = (PEDIGREE_C / "include/pedigree/log.h").read_text(
            encoding="utf-8"
        )
        implementation = (PEDIGREE_C / "pedigree-log.c").read_text(
            encoding="utf-8"
        )
        modules_cmake = (ROOT / "src/modules/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        hosted_cmake = (ROOT / "src/user/hosted-smoke/CMakeLists.txt").read_text(
            encoding="utf-8"
        )

        self.assertIn("int pedigree_log", header)
        self.assertIn("format(printf, 2, 3)", header)
        self.assertRegex(implementation, r"char\s+message\s*\[1024\]")
        self.assertNotRegex(implementation, r"static\s+char\s+message")
        self.assertIn("POSIX_SYSLOG", implementation)
        self.assertIn("subsys/pedigree-c/pedigree-log.c", modules_cmake)
        self.assertIn("add_library(pedigree-c-user OBJECT", modules_cmake)
        self.assertIn('OSX_ARCHITECTURES ""', modules_cmake)
        self.assertIn("add_custom_target(pedigree-c-sdk", modules_cmake)
        self.assertIn("usr/include/pedigree/log.h", modules_cmake)
        self.assertIn("libpedigree-c.so", modules_cmake)
        self.assertIn("PEDIGREE_C_SDK_HEADER", modules_cmake)
        self.assertIn("PEDIGREE_C_SDK_LIBRARY", modules_cmake)
        self.assertNotIn(
            'if (NOT PEDIGREE_ARCH_TARGET STREQUAL "HOSTED")\n'
            "    add_library(pedigree-c-user",
            modules_cmake,
        )
        self.assertIn(
            "target_link_libraries(${target} PRIVATE pedigree-c-user",
            hosted_cmake,
        )
        self.assertIn('OSX_ARCHITECTURES ""', hosted_cmake)

        root_cmake = (ROOT / "CMakeLists.txt").read_text(encoding="utf-8")
        self.assertIn("TARGET pedigree-c-sdk", root_cmake)
        self.assertIn("${PEDIGREE_C_SDK_ROOT}", root_cmake)
        self.assertIn("${PEDIGREE_C_SDK_HEADER}", root_cmake)
        self.assertIn("${PEDIGREE_C_SDK_LIBRARY}", root_cmake)


if __name__ == "__main__":
    unittest.main()
