import re
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MUSL = ROOT / "src/modules/subsys/posix/musl"


class MuslSyscallRoutingTests(unittest.TestCase):
    def test_native_musl_uses_the_raw_linux_syscall_abi(self):
        build_script = (ROOT / "scripts/build-musl-amd64.sh").read_text(
            encoding="utf-8"
        )
        x64_branch = build_script.split("    X64)", 1)[1].split("    *)", 1)[0]

        self.assertIn('cp "$upstream_snapshot/syscall_arch.h"', x64_branch)
        self.assertIn('cp "$upstream_snapshot/syscall_cp.s"', x64_branch)
        self.assertIn('cp "$upstream_snapshot/clone.s"', x64_branch)
        self.assertNotIn("pedigree_translate_syscall", x64_branch)
        self.assertFalse((MUSL / "clone-amd64.musl-s").exists())

        glue = (MUSL / "glue-musl.c").read_text(encoding="utf-8")
        hosted_bridge = glue.split("#if HOSTED", 1)[1].split("#endif", 1)[0]
        self.assertIn("long pedigree_translate_syscall", hosted_bridge)
        self.assertNotIn("posix_translate_syscall", hosted_bridge)
        self.assertNotRegex(glue, r"\bklog\s*\(")
        self.assertNotIn("POSIX_SYSLOG", glue)

    def test_hosted_musl_keeps_its_explicit_syscall_bridge(self):
        syscall_arch = (MUSL / "syscall_arch.h").read_text(encoding="utf-8")
        syscall_cp = (MUSL / "syscall_cp-amd64.musl-s").read_text(
            encoding="utf-8"
        )
        hosted_glue = (MUSL / "glue-musl.c").read_text(encoding="utf-8")
        hosted_clone = (MUSL / "clone-hosted-amd64.musl-s").read_text(
            encoding="utf-8"
        )

        self.assertIn("pedigree_translate_syscall", syscall_arch)
        self.assertRegex(syscall_cp, r"\bcall\s+pedigree_translate_syscall\b")
        self.assertIn("long pedigree_translate_syscall", hosted_glue)
        self.assertNotIn("posix_translate_syscall", hosted_glue)
        self.assertNotIn("#include <translate.h>", hosted_glue)
        self.assertIn("linuxCompat, which", hosted_glue)
        self.assertIn("call pedigree_translate_syscall", hosted_clone)
        self.assertIn("call pedigree_musl_thread_exit", hosted_clone)

        build_script = (ROOT / "scripts/build-musl-amd64.sh").read_text(
            encoding="utf-8"
        )
        hosted_branch = build_script.split("    HOSTED)", 1)[1].split(
            "    X64)", 1
        )[0]
        self.assertIn("syscall_arch.h", hosted_branch)
        self.assertIn("syscall_cp-amd64.musl-s", hosted_branch)

        modules_cmake = (ROOT / "src/modules/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        hosted_inputs = modules_cmake.split(
            "list(APPEND PEDIGREE_MUSL_PORT_INPUTS", 1
        )[1].split("else ()", 1)[0]
        self.assertIn("processor/Syscalls.h", hosted_inputs)

    def test_musl_build_config_is_independent_of_kernel_options(self):
        modules_cmake = (ROOT / "src/modules/CMakeLists.txt").read_text(
            encoding="utf-8"
        )

        self.assertIn(
            "PEDIGREE_CONFIG_INCLUDE_DIR=${PEDIGREE_MUSL_CONFIG_DIR}",
            modules_cmake,
        )
        self.assertNotIn("${CMAKE_BINARY_DIR}/config.h", modules_cmake)

        config = (MUSL / "config.h.in").read_text(encoding="utf-8")
        self.assertIn("#define HOSTED @PEDIGREE_MUSL_CONFIG_HOSTED@", config)
        self.assertIn("#define X64 @PEDIGREE_MUSL_CONFIG_X64@", config)

        builder_fingerprint = (
            ROOT / "scripts/ci/builder-fingerprint.sh"
        ).read_text(encoding="utf-8")
        self.assertNotIn(
            "src/system/include/pedigree/kernel/processor/x64/syscall-stubs.h",
            builder_fingerprint,
        )

    def test_kernel_is_the_native_syscall_translation_authority(self):
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")

        self.assertIn('#include "syscalls/translate.h"', manager)
        self.assertIn("long which = posix_translate_syscall(syscallNumber);", manager)
        self.assertRegex(
            manager,
            re.compile(
                r"if \(which < 0\).*?SYSCALL_ERROR\(Unimplemented\);.*?return -1;",
                re.DOTALL,
            ),
        )

    def test_raw_syscalls_select_linux_service_and_use_linux_errno(self):
        services = (
            ROOT / "src/system/include/pedigree/kernel/processor/Syscalls.h"
        ).read_text(encoding="utf-8")
        state = (
            ROOT / "src/system/include/pedigree/kernel/processor/x64/state.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/system/kernel/core/processor/x64/SyscallManager.cc"
        ).read_text(encoding="utf-8")

        self.assertRegex(services, r"\blinuxCompat\s*=\s*0\b")
        self.assertRegex(state, r"return \(\(m_Rax >> 16\) & 0xFFFF\);")
        self.assertRegex(state, r"return \(m_Rax & 0xFFFF\);")
        for parameter, register in enumerate(
            ("m_Rdi", "m_Rsi", "m_Rdx", "m_R10", "m_R8", "m_R9"), start=6
        ):
            self.assertRegex(
                state, rf"case {parameter}:\s+return {register};"
            )
        self.assertRegex(
            manager,
            re.compile(
                r"if \(serviceNumber == linuxCompat\).*?"
                r"setSyscallReturnValue\(-errno\)",
                re.DOTALL,
            ),
        )

    def test_hosted_state_exposes_linux_abi_parameter_slots(self):
        state = (
            ROOT / "src/system/include/pedigree/kernel/processor/hosted/state.h"
        ).read_text(encoding="utf-8")
        parameter_reader = state.split(
            "uintptr_t HostedSyscallState::getSyscallParameter", 1
        )[1].split(
            "void HostedSyscallState::setSyscallReturnValue", 1
        )[0]

        for native_slot, linux_slot, parameter in zip(
            range(6), range(6, 12), range(1, 7)
        ):
            with self.subTest(parameter=parameter):
                self.assertRegex(
                    parameter_reader,
                    rf"case {native_slot}:\s+case {linux_slot}:\s+return p{parameter};",
                )


if __name__ == "__main__":
    unittest.main()
