import tarfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]


def read_musl_source(relative_path: str) -> str:
    source = ROOT / "build/src/modules/musl-1.2.6" / relative_path
    if source.exists():
        return source.read_text(encoding="utf-8")

    archive_path = ROOT / "build/src/modules/musl-1.2.6.tar.gz"
    if not archive_path.exists():
        raise unittest.SkipTest("the configured musl 1.2.6 source is unavailable")
    with tarfile.open(archive_path, "r:gz") as archive:
        member = archive.extractfile(f"musl-1.2.6/{relative_path}")
        if member is None:
            raise AssertionError(f"musl source is missing {relative_path}")
        return member.read().decode("utf-8")


class LinuxAsyncSignalTests(unittest.TestCase):
    def test_exact_musl_uses_linux_rt_restorer_and_ucontext_registers(self):
        restore = read_musl_source("src/signal/x86_64/restore.s")
        sigaction = read_musl_source("src/signal/sigaction.c")
        signal_header = read_musl_source("arch/x86_64/bits/signal.h")

        self.assertIn("mov $15, %rax", restore)
        self.assertIn("syscall", restore)
        self.assertIn("ksa.flags |= SA_RESTORER", sigaction)
        self.assertIn("? __restore_rt : __restore", sigaction)
        self.assertIn("enum { REG_RAX = 13 };", signal_header)
        self.assertIn("typedef struct __ucontext", signal_header)

    def test_kernel_frame_prefix_matches_linux_amd64_layout(self):
        abi = (
            ROOT / "src/modules/subsys/posix/linux-amd64-signal-abi.h"
        ).read_text(encoding="utf-8")

        self.assertIn("sizeof(Ucontext) == 304", abi)
        self.assertIn("sizeof(Siginfo) == 128", abi)
        self.assertIn("sizeof(RtSigframe) == 440", abi)
        self.assertIn("sizeof(Fpstate) == 512", abi)
        self.assertIn("offsetof(RtSigframe, info) == 312", abi)

    def test_async_delivery_requires_an_exact_return_context(self):
        event_header = (
            ROOT / "src/modules/subsys/posix/linux-amd64-signal.h"
        ).read_text(encoding="utf-8")
        scheduler = (
            ROOT / "src/system/kernel/core/process/PerProcessorScheduler.cc"
        ).read_text(encoding="utf-8")
        thread = (ROOT / "src/system/kernel/core/process/Thread.cc").read_text(
            encoding="utf-8"
        )

        self.assertIn("class AsyncEvent final : public SignalEvent", event_header)
        self.assertIn("requiresExactUserReturnState() const override", event_header)
        self.assertIn("EventSelection::WithoutExactUserReturn", scheduler)
        self.assertIn("deliverAtUserReturn(*interruptState)", scheduler)
        self.assertIn("deliverAtUserReturn(*syscallState)", scheduler)
        self.assertIn("markDeferredUserReturnSignalInterruption", thread)

    def test_bundled_musl_private_signal_contract_is_explicit(self):
        pthread_impl = read_musl_source("src/internal/pthread_impl.h")
        sigaction = read_musl_source("src/signal/sigaction.c")
        pthread_cancel = read_musl_source("src/thread/pthread_cancel.c")
        synccall = read_musl_source("src/thread/synccall.c")
        membarrier = read_musl_source("src/linux/membarrier.c")
        sigrtmin = read_musl_source("src/signal/sigrtmin.c")

        self.assertIn("#define SIGTIMER 32", pthread_impl)
        self.assertIn("#define SIGCANCEL 33", pthread_impl)
        self.assertIn("#define SIGSYNCCALL 34", pthread_impl)
        self.assertIn("return 35;", sigrtmin)
        self.assertIn("if (sig-32U < 3 || sig-1U >= _NSIG-1)", sigaction)
        self.assertIn("__libc_sigaction(SIGCANCEL, &sa, 0)", pthread_cancel)
        self.assertIn("__syscall(SYS_tkill, self->tid, SIGCANCEL)", pthread_cancel)
        self.assertIn("__libc_sigaction(SIGSYNCCALL, &sa, 0)", synccall)
        self.assertIn("__syscall(SYS_tkill, td->tid, SIGSYNCCALL)", synccall)
        self.assertIn("cmd == MEMBARRIER_CMD_PRIVATE_EXPEDITED", membarrier)
        self.assertIn("__libc_sigaction(SIGSYNCCALL, &sa, 0)", membarrier)

    def test_private_signal_kernel_range_and_raw_sigaction_abi_are_bounded(self):
        subsystem = (
            ROOT / "src/modules/subsys/posix/PosixSubsystem.h"
        ).read_text(encoding="utf-8")
        signals = (
            ROOT / "src/modules/subsys/posix/signal-syscalls.cc"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")

        self.assertIn("LinuxPrivateSignalFirst = 32", subsystem)
        self.assertIn("MaximumSupportedSignal = 64", subsystem)
        self.assertNotIn("lookup(sig % 32)", subsystem)
        self.assertIn("sizeof(LinuxAmd64KernelSigaction) == 32", signals)

        dispatch = manager.split("POSIX_CASE(POSIX_SIGACTION)", 1)[1].split(
            "case POSIX_SIGNAL:", 1
        )[0]
        self.assertIn("argument(3) != sizeof(uint64_t)", dispatch)
        self.assertIn("posix_linux_amd64_sigaction", dispatch)

        raw_sigaction = signals.split(
            "int posix_linux_amd64_sigaction", 1
        )[1].split("uintptr_t posix_signal", 1)[0]
        self.assertIn(
            "posix_sigaction_impl(sig, nativeActPtr, nativeOldPtr, true)",
            raw_sigaction,
        )
        self.assertNotIn("sig >= 32", raw_sigaction)
        self.assertNotIn("sig <= 64", raw_sigaction)

        tkill = signals.split("int posix_tkill", 1)[1].split(
            "int posix_tgkill", 1
        )[0]
        tgkill = signals.split("int posix_tgkill", 1)[1].split(
            "int posix_kill", 1
        )[0]
        process_kill = signals.split("int posix_kill", 1)[1].split(
            "int posix_killpg", 1
        )[0]
        self.assertIn("MaximumSupportedSignal", tkill)
        self.assertIn("MaximumSupportedSignal", tgkill)
        self.assertIn("MaximumSupportedSignal", process_kill)
        self.assertIn("sig >= 32 && sig <= 34", process_kill)

    def test_target_probe_uses_public_musl_cancellation_and_synccall_paths(self):
        probe = (
            ROOT / "src/user/applications/private-signal-test/main.c"
        ).read_text(encoding="utf-8")

        self.assertIn("sizeof(struct kernel_sigaction) == 32", probe)
        self.assertIn("kernel_sigset_size = 8", probe)
        self.assertIn("sigaction(signal, 0, &public_action)", probe)
        self.assertIn("first_realtime_signal = 35", probe)
        self.assertIn("last_realtime_signal = 64", probe)
        self.assertIn("first_unsupported_signal = 65", probe)
        self.assertIn("SIGRTMIN != first_realtime_signal", probe)
        self.assertIn("SIGRTMAX != last_realtime_signal", probe)
        self.assertIn("run_bounded(realtime_signal_contract)", probe)
        self.assertIn("sigaddset(&public_set, signal)", probe)
        self.assertIn("sigdelset(&public_set, signal)", probe)
        self.assertIn("sigfillset(&public_set)", probe)
        self.assertIn("sigismember(&public_set, signal)", probe)
        self.assertIn("SYS_rt_sigaction", probe)
        self.assertIn("pthread_cancel(thread)", probe)
        self.assertIn("result != PTHREAD_CANCELED", probe)
        self.assertIn("membarrier(MEMBARRIER_CMD_PRIVATE_EXPEDITED, 0)", probe)
        self.assertIn("PRIVATE-SIGNAL-TEST: PASS", probe)

    def test_target_probe_validates_siginfo_restorer_and_ucontext_edit(self):
        process_test = (
            ROOT / "src/user/applications/testsuite/process.c"
        ).read_text(encoding="utf-8")

        self.assertIn("action.sa_flags = SA_SIGINFO", process_test)
        self.assertIn("info->si_code != SI_TKILL", process_test)
        self.assertIn("context->uc_mcontext.gregs[REG_RAX]", process_test)
        self.assertIn("signalFrameCalls != 2", process_test)
        self.assertIn("resumed != signalFrameResumeValue", process_test)

        syscall_manager = (
            ROOT / "src/system/kernel/core/processor/x64/SyscallManager.cc"
        ).read_text(encoding="utf-8")
        restore = syscall_manager.split("case RestoreProcessorState:", 1)[1].split(
            "case JumpToUserspace:", 1
        )[0]
        self.assertRegex(
            restore,
            r"serviceUserReturnWork\(\s*\*returnState,\s*"
            r"UserReturnFrame::Origin::SignalRestore\s*\)",
        )


if __name__ == "__main__":
    unittest.main()
