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
        self.assertIn("scheduler.serviceUserReturnWork(*returnState)", restore)


if __name__ == "__main__":
    unittest.main()
