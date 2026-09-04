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
        self.assertIn('cp "$upstream_snapshot/restore.s"', x64_branch)
        self.assertIn('cp "$upstream_snapshot/vfork.s"', x64_branch)
        self.assertIn(
            'cp "$upstream_snapshot/__set_thread_area.s"', x64_branch
        )
        self.assertIn('cp "$upstream_snapshot/__unmapself.s"', x64_branch)
        self.assertNotIn("pedigree_translate_syscall", x64_branch)
        self.assertNotIn("musl/ttyname.c", build_script)
        self.assertNotIn("musl/fb.h", build_script)
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
        self.assertIn('pedigree_cppflags="-I$SRCDIR/', hosted_branch)
        self.assertIn("-DHOSTED=1", hosted_branch)
        self.assertIn("rm -f src/signal/x86_64/restore.s", hosted_branch)
        self.assertIn("rm -f src/process/x86_64/vfork.s", hosted_branch)
        self.assertIn(
            "rm -f src/thread/x86_64/{__unmapself,__set_thread_area}.s",
            hosted_branch,
        )

        modules_cmake = (ROOT / "src/modules/CMakeLists.txt").read_text(
            encoding="utf-8"
        )
        hosted_inputs = modules_cmake.split(
            "list(APPEND PEDIGREE_MUSL_PORT_INPUTS", 1
        )[1].split("else ()", 1)[0]
        self.assertIn("processor/Syscalls.h", hosted_inputs)

    def test_native_musl_trampoline_syscalls_are_mapped(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(munmap, 11, POSIX_MUNMAP)",
            mappings,
        )
        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(rt_sigreturn, 15, PEDIGREE_SIGRET)",
            mappings,
        )
        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(exit, 60, POSIX_EXIT)",
            mappings,
        )
        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(vfork, 58, POSIX_FORK)",
            mappings,
        )
        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(arch_prctl, 158, POSIX_ARCH_PRCTL)",
            mappings,
        )

        build_script = (ROOT / "scripts/build-musl-amd64.sh").read_text(
            encoding="utf-8"
        )
        for symbol in (
            "__restore_rt",
            "vfork",
            "__set_thread_area",
            "__unmapself",
        ):
            with self.subTest(symbol=symbol):
                self.assertIn(f"--disassemble={symbol}", build_script)

    def test_linux_epoll_syscalls_are_mapped(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")

        expected = (
            "PEDIGREE_LINUX_AMD64_SYSCALL(epoll_create, 213, POSIX_EPOLL_CREATE)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(epoll_wait, 232, POSIX_EPOLL_WAIT)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(epoll_ctl, 233, POSIX_EPOLL_CTL)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(epoll_pwait, 281, POSIX_EPOLL_PWAIT)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(epoll_create1, 291, POSIX_EPOLL_CREATE1)",
        )
        for mapping in expected:
            with self.subTest(mapping=mapping):
                self.assertIn(mapping, mappings)

    def test_linux_epoll_pwait_uses_a_guarded_temporary_mask(self):
        source = (
            ROOT / "src/modules/subsys/posix/epoll-syscalls.cc"
        ).read_text(encoding="utf-8")
        helper, pwait = source.split("int posix_epoll_pwait", 1)
        helper = helper.rsplit("int epollWait", 1)[1]
        instance_wait = source.split("int EpollInstance::wait", 1)[1].split(
            "int posix_epoll_create1", 1
        )[0]

        self.assertIn(
            "LinuxKernelSigsetSize = sizeof(uint64_t)", source
        )
        self.assertIn("SIGKILL - 1", source)
        self.assertIn("SIGSTOP - 1", source)
        self.assertIn("Thread::TemporarySignalMask signalWait", helper)
        self.assertLess(
            helper.index("signalWait.finish()"),
            helper.index("PosixSubsystem::copyToUser"),
        )
        self.assertIn("if (!result && signalInterrupted)", helper)
        final_rescan = instance_wait.rindex(
            "ready = collectEvents(events, maxEvents, true)"
        )
        self.assertLess(
            final_rescan,
            instance_wait.index("SYSCALL_ERROR(Interrupted)", final_rescan),
        )
        self.assertIn("if (!signalMask)", pwait)
        self.assertLess(
            pwait.index("if (!signalMask)"),
            pwait.index("signalMaskSize != LinuxKernelSigsetSize"),
        )
        null_branch = pwait.split("if (!signalMask)", 1)[1].split("}", 1)[0]
        self.assertIn(
            "return epollWait(epollFd, events, maxEvents, "
            "timeoutMilliseconds, nullptr)",
            null_branch,
        )
        self.assertNotIn("signalMaskSize", null_branch)
        self.assertIn("PosixSubsystem::copyFromUser", pwait)
        self.assertRegex(
            pwait,
            re.compile(
                r"if \(!PosixSubsystem::copyFromUser\(.*?"
                r"SYSCALL_ERROR\(BadAddress\)",
                re.DOTALL,
            ),
        )
        self.assertIn("temporarySignalMask &= ~UnblockableSignals", pwait)
        self.assertNotIn("OperationNotSupported", pwait)

    def test_linux_eventfd_syscalls_are_mapped(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(eventfd, 284, POSIX_EVENTFD)",
            mappings,
        )
        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(eventfd2, 290, POSIX_EVENTFD2)",
            mappings,
        )

    def test_signal_return_accepts_iret_and_sysret_user_selectors(self):
        signal_source = (
            ROOT / "src/modules/subsys/posix/linux-amd64-signal.cc"
        ).read_text(encoding="utf-8")

        self.assertIn("IretUserCodeSegment = 0x1B", signal_source)
        self.assertIn("SysretUserCodeSegment = 0x2B", signal_source)
        self.assertEqual(signal_source.count("userCodeSegment("), 3)

    def test_linux_signal_frames_use_guarded_user_copies(self):
        signal_source = (
            ROOT / "src/modules/subsys/posix/linux-amd64-signal.cc"
        ).read_text(encoding="utf-8")
        delivery = signal_source.split(
            "LinuxAmd64Signal::DeliveryResult LinuxAmd64Signal::deliverSynchronous",
            1,
        )[1]
        delivery, sigreturn = delivery.split(
            "void LinuxAmd64Signal::sigreturn", 1
        )

        self.assertIn(
            "saveCurrentThreadFpuState(&savedFpstate, true)", delivery
        )
        self.assertIn("Fpstate fpstate = savedFpstate;", delivery)
        self.assertIn("ByteSet(fpstate.reserved3", delivery)
        self.assertNotIn("ByteSet(savedFpstate.reserved3", delivery)
        self.assertIn(
            "restoreCurrentThreadFpuState(&savedFpstate)", delivery
        )
        self.assertNotIn("restoreCurrentThreadFpuState(&fpstate)", delivery)

        self.assertEqual(delivery.count("PosixSubsystem::copyToUser"), 2)
        fpstate_copy = delivery.index(
            "copyToUser(reinterpret_cast<void*>(fpstateAddress)"
        )
        frame_copy = delivery.index(
            "copyToUser(reinterpret_cast<void*>(frameAddress)"
        )
        self.assertLess(fpstate_copy, frame_copy)
        for mutation in (
            "thread->setSignalMask(handlerMask",
            "alternate.inUse =",
            "state.setRegister(",
            "state.setInstructionPointer(",
            "state.setStackPointer(",
            "state.setFlags(",
        ):
            with self.subTest(mutation=mutation):
                self.assertLess(frame_copy, delivery.index(mutation))
        self.assertNotIn(
            "MemoryCopy(reinterpret_cast<void*>(frameAddress)", delivery
        )
        self.assertNotIn(
            "MemoryCopy(reinterpret_cast<void*>(fpstateAddress)", delivery
        )

        self.assertEqual(sigreturn.count("PosixSubsystem::copyFromUser"), 2)
        self.assertIn("userRegion(frameAddress, sizeof(RtSigframe)", sigreturn)
        self.assertIn("userRegion(context.fpstate, sizeof(Fpstate)", sigreturn)
        self.assertNotIn("MemoryCopy(&frame,", sigreturn)
        self.assertNotIn("MemoryCopy(&fpstate,", sigreturn)

    def test_musl_build_config_is_independent_of_kernel_options(self):
        build_script = (ROOT / "scripts/build-musl-amd64.sh").read_text(
            encoding="utf-8"
        )
        source_selection = build_script.split('case "$ARCH_TARGET" in', 1)[
            1
        ].split("esac", 1)[0]
        x64_branch = source_selection.split("    X64)", 1)[1].split(
            "    *)", 1
        )[0]
        self.assertIn("pedigree_cppflags=", x64_branch)
        self.assertNotIn("PEDIGREE_CONFIG_INCLUDE_DIR", x64_branch)
        self.assertNotIn("-DX64", x64_branch)

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
