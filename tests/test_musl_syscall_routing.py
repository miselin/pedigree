import re
import tarfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
MUSL = ROOT / "src/modules/subsys/posix/musl"
HOSTED_MUSL_CMAKE = ROOT / "build-etc/cmake/PedigreeHostedMusl.cmake"


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

        hosted_musl_cmake = HOSTED_MUSL_CMAKE.read_text(
            encoding="utf-8"
        )
        hosted_inputs = hosted_musl_cmake.split(
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
            "PEDIGREE_LINUX_AMD64_SYSCALL(vfork, 58, POSIX_VFORK)",
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

    def test_linux_clone3_is_mapped_and_dispatched(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")
        numbers = (
            ROOT / "src/modules/subsys/posix/syscalls/posixSyscallNumbers.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(clone3, 435, POSIX_CLONE3)",
            mappings,
        )
        self.assertIn("#define POSIX_CLONE3 413", numbers)
        self.assertIn('#include "clone3-syscalls.h"', manager)
        self.assertRegex(
            manager,
            r"case POSIX_CLONE3:\s+return posix_clone3\(state, "
            r"reinterpret_cast<const LinuxCloneArgs\*>\(p1\), p2\);",
        )

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

    def test_linux_dup3_is_mapped_and_dispatched(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")
        numbers = (
            ROOT / "src/modules/subsys/posix/syscalls/posixSyscallNumbers.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")
        declarations = (
            ROOT / "src/modules/subsys/posix/file-syscalls.h"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(dup3, 292, POSIX_DUP3)",
            mappings,
        )
        self.assertIn("#define POSIX_DUP3 295", numbers)
        self.assertRegex(
            manager,
            r"case POSIX_DUP3:\s+return posix_dup3\(static_cast<int>\(p1\), "
            r"static_cast<int>\(p2\), static_cast<int>\(p3\)\);",
        )
        self.assertIn(
            "int posix_dup3(int oldfd, int newfd, int flags);",
            declarations,
        )

    def test_linux_positional_io_syscalls_are_mapped_and_dispatched(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")
        numbers = (
            ROOT / "src/modules/subsys/posix/syscalls/posixSyscallNumbers.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")
        declarations = (
            ROOT / "src/modules/subsys/posix/file-syscalls.h"
        ).read_text(encoding="utf-8")

        expected_mappings = (
            "PEDIGREE_LINUX_AMD64_SYSCALL(pread64, 17, POSIX_PREAD64)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(pwrite64, 18, POSIX_PWRITE64)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(preadv, 295, POSIX_PREADV)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(pwritev, 296, POSIX_PWRITEV)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(preadv2, 327, POSIX_PREADV2)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(pwritev2, 328, POSIX_PWRITEV2)",
        )
        for mapping in expected_mappings:
            with self.subTest(mapping=mapping):
                self.assertIn(mapping, mappings)
        self.assertIn("#define POSIX_PREAD64 290", numbers)
        self.assertIn("#define POSIX_PWRITE64 291", numbers)
        self.assertIn("#define POSIX_PREADV 296", numbers)
        self.assertIn("#define POSIX_PWRITEV 297", numbers)
        self.assertIn("#define POSIX_PREADV2 298", numbers)
        self.assertIn("#define POSIX_PWRITEV2 299", numbers)

        pread = manager.split("case POSIX_PREAD64:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_pread64", pread)
        self.assertIn("static_cast<int>(p1)", pread)
        self.assertIn("reinterpret_cast<char*>(p2)", pread)
        self.assertIn("static_cast<size_t>(p3)", pread)
        self.assertIn("static_cast<off_t>(p4)", pread)

        pwrite = manager.split("case POSIX_PWRITE64:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_pwrite64", pwrite)
        self.assertIn("static_cast<int>(p1)", pwrite)
        self.assertIn("reinterpret_cast<const char*>(p2)", pwrite)
        self.assertIn("static_cast<size_t>(p3)", pwrite)
        self.assertIn("static_cast<off_t>(p4)", pwrite)

        self.assertIn("off_t linuxAmd64VectorOffset", manager)
        self.assertIn("static_cast<uint64_t>(high) << 32U", manager)
        self.assertIn("static_cast<uint64_t>(low) & 0xFFFFFFFFULL", manager)
        for syscall in ("PREADV", "PWRITEV", "PREADV2", "PWRITEV2"):
            dispatch = manager.split(f"case POSIX_{syscall}:", 1)[1].split(
                "case ", 1
            )[0]
            self.assertIn(f"return posix_{syscall.lower()}", dispatch)
            self.assertIn("linuxAmd64VectorOffset(p4, p5)", dispatch)
            if syscall.endswith("2"):
                self.assertIn("static_cast<int>(p6)", dispatch)

        for declaration in (
            "ssize_t posix_preadv(int fd, const struct iovec* iov, int iovcnt, off_t offset);",
            "ssize_t posix_pwritev(int fd, const struct iovec* iov, int iovcnt, off_t offset);",
            "ssize_t posix_preadv2(int fd, const struct iovec* iov, int iovcnt, off_t offset, int flags);",
            "ssize_t posix_pwritev2(int fd, const struct iovec* iov, int iovcnt, off_t offset, int flags);",
        ):
            with self.subTest(declaration=declaration):
                self.assertIn(declaration, declarations)

    def test_bundled_musl_positional_vectors_use_the_split_linux_abi(self):
        source_root = ROOT / "build/src/modules/musl-1.2.6"
        archive_path = ROOT / "build/src/modules/musl-1.2.6.tar.gz"

        def load_source(relative_path):
            source_path = source_root / relative_path
            if source_path.exists():
                return source_path.read_text(encoding="utf-8")
            if not archive_path.exists():
                self.skipTest("the configured musl source archive is not present")
            with tarfile.open(archive_path, "r:gz") as archive:
                member = archive.extractfile(f"musl-1.2.6/{relative_path}")
                self.assertIsNotNone(member)
                return member.read().decode("utf-8")

        preadv = load_source("src/unistd/preadv.c")
        pwritev = load_source("src/unistd/pwritev.c")
        preadv2 = load_source("src/linux/preadv2.c")
        pwritev2 = load_source("src/linux/pwritev2.c")

        for source in (preadv, pwritev, preadv2, pwritev2):
            self.assertIn("(long)(ofs), (long)(ofs>>32)", source)
        self.assertIn("syscall_cp(SYS_preadv, fd, iov, count", preadv)
        self.assertIn("__syscall_cp(SYS_pwritev2, fd, iov, count", pwritev)
        self.assertIn("RWF_NOAPPEND", pwritev)
        self.assertIn("fcntl(fd, F_GETFL) & O_APPEND", pwritev)
        self.assertIn("syscall_cp(SYS_pwritev, fd, iov, count", pwritev)
        self.assertIn("if (ofs==-1) return readv(fd, iov, count);", preadv2)
        self.assertIn("syscall_cp(SYS_preadv2, fd, iov, count", preadv2)
        self.assertIn("if (ofs==-1) return writev(fd, iov, count);", pwritev2)
        self.assertIn("syscall_cp(SYS_pwritev2, fd, iov, count", pwritev2)

    def test_linux_resource_compatibility_syscalls_are_mapped_and_dispatched(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")
        numbers = (
            ROOT / "src/modules/subsys/posix/syscalls/posixSyscallNumbers.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")

        expected_mappings = (
            "PEDIGREE_LINUX_AMD64_SYSCALL(prlimit64, 302, POSIX_PRLIMIT64)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(membarrier, 324, POSIX_MEMBARRIER)",
        )
        for mapping in expected_mappings:
            with self.subTest(mapping=mapping):
                self.assertIn(mapping, mappings)

        self.assertIn("#define POSIX_PRLIMIT64 292", numbers)
        self.assertIn("#define POSIX_MEMBARRIER 293", numbers)

        prlimit = manager.split("case POSIX_PRLIMIT64:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_prlimit64", prlimit)
        self.assertIn("reinterpret_cast<const LinuxRlimit64*>(p3)", prlimit)
        self.assertIn("reinterpret_cast<LinuxRlimit64*>(p4)", prlimit)

        membarrier = manager.split("case POSIX_MEMBARRIER:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_membarrier", membarrier)

    def test_linux_faccessat2_is_mapped_and_dispatched_with_flags(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")
        numbers = (
            ROOT / "src/modules/subsys/posix/syscalls/posixSyscallNumbers.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(faccessat2, 439, POSIX_FACCESSAT2)",
            mappings,
        )
        self.assertIn("#define POSIX_FACCESSAT2 294", numbers)

        faccessat = manager.split("case POSIX_FACCESSAT:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn(
            "return posix_faccessat(p1, reinterpret_cast<const char*>(p2), p3, 0)",
            faccessat,
        )

        faccessat2 = manager.split("case POSIX_FACCESSAT2:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_faccessat", faccessat2)
        self.assertIn("reinterpret_cast<const char*>(p2)", faccessat2)
        self.assertIn("p3, p4", faccessat2)

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

    def test_linux_ppoll_uses_the_linux_wait_abi_and_dispatches(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")
        wait_abi = (
            ROOT / "src/modules/subsys/posix/linux-wait-abi.h"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(ppoll, 271, POSIX_PPOLL)",
            mappings,
        )
        self.assertIn("case POSIX_PPOLL:", manager)
        dispatch = manager.split("case POSIX_PPOLL:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_ppoll", dispatch)
        self.assertIn("static_cast<unsigned int>(p2)", dispatch)
        self.assertIn("reinterpret_cast<LinuxKernelTimespec*>(p3)", dispatch)
        self.assertIn("reinterpret_cast<const uint64_t*>(p4)", dispatch)
        self.assertIn("static_cast<size_t>(p5)", dispatch)

        self.assertRegex(
            wait_abi,
            re.compile(
                r"struct LinuxKernelTimespec\s*\{\s*"
                r"int64_t tv_sec;\s*int64_t tv_nsec;\s*\};",
                re.DOTALL,
            ),
        )
        self.assertIn("offsetof(LinuxKernelTimespec, tv_sec) == 0", wait_abi)
        self.assertIn("offsetof(LinuxKernelTimespec, tv_nsec) == 8", wait_abi)
        self.assertIn(
            "static_assert(sizeof(LinuxKernelTimespec) == 16", wait_abi
        )

        poll_source = (
            ROOT / "src/modules/subsys/posix/poll-syscalls.cc"
        ).read_text(encoding="utf-8")
        ppoll = poll_source.split("int posix_ppoll", 1)[1]
        self.assertLess(
            ppoll.index("copyFromUser(&timeoutSnapshot"),
            ppoll.index("Thread::TemporarySignalMask signalWait"),
        )
        self.assertLess(
            ppoll.index("copyFromUser(&temporarySignalMask"),
            ppoll.index("Thread::TemporarySignalMask signalWait"),
        )
        self.assertLess(
            ppoll.index("Thread::TemporarySignalMask signalWait"),
            ppoll.index("return ppollWithDeadline"),
        )

        finish = poll_source.split("bool finishPpoll", 1)[1].split(
            "int ppollWithDeadline", 1
        )[0]
        self.assertLess(
            finish.index("temporarySignalMask->finish()"),
            finish.index("copyPpollTimeoutRemainder"),
        )
        helper = poll_source.split("int ppollWithDeadline", 1)[1].split(
            "}  // namespace", 1
        )[0]
        terminal = helper.split("int result = pollWithDeadline", 1)[1]
        self.assertLess(
            terminal.index("copyPollReventsToUser"),
            terminal.index("finishPpoll("),
        )

        copyout = poll_source.split("bool copyPollReventsToUser", 1)[1].split(
            "}  // namespace", 1
        )[0]
        self.assertIn("&userFds[i].revents", copyout)
        self.assertIn("sizeof(snapshot[i].revents)", copyout)
        self.assertNotIn("sizeof(struct pollfd)", copyout)
        poll_entry = poll_source.split("int posix_poll(", 1)[1].split(
            "namespace {", 1
        )[0]
        self.assertIn("copyPollReventsToUser(fds, snapshot, nfds)", poll_entry)
        self.assertIn("copyPollReventsToUser(fds, snapshot, nfds)", helper)

    def test_bundled_musl_ppoll_uses_the_five_argument_raw_abi(self):
        hosted_musl_cmake = HOSTED_MUSL_CMAKE.read_text(encoding="utf-8")
        self.assertIn('set(MUSL_VERSION "1.2.6")', hosted_musl_cmake)

        source_path = (
            ROOT
            / "build/src/modules/musl-1.2.6/src/select/ppoll.c"
        )
        if source_path.exists():
            source = source_path.read_text(encoding="utf-8")
        else:
            archive_path = (
                ROOT / "build/src/modules/musl-1.2.6.tar.gz"
            )
            if not archive_path.exists():
                self.skipTest("the configured musl source archive is not present")
            with tarfile.open(archive_path, "r:gz") as archive:
                member = archive.extractfile(
                    "musl-1.2.6/src/select/ppoll.c"
                )
                self.assertIsNotNone(member)
                source = member.read().decode("utf-8")

        self.assertIn("int ppoll(struct pollfd *fds, nfds_t n", source)
        self.assertIn("syscall_cp(SYS_ppoll, fds, n,", source)
        self.assertIn("mask, _NSIG/8", source)

    def test_linux_pselect6_uses_the_six_argument_raw_abi(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")
        numbers = (
            ROOT / "src/modules/subsys/posix/syscalls/posixSyscallNumbers.h"
        ).read_text(encoding="utf-8")
        wait_abi = (
            ROOT / "src/modules/subsys/posix/linux-wait-abi.h"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(pselect6, 270, POSIX_PSELECT6)",
            mappings,
        )
        self.assertIn("#define POSIX_PSELECT6 284", numbers)
        self.assertIn("case POSIX_PSELECT6:", manager)
        dispatch = manager.split("case POSIX_PSELECT6:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_pselect6", dispatch)
        self.assertIn("static_cast<int>(p1)", dispatch)
        for parameter in ("p2", "p3", "p4"):
            self.assertIn(f"reinterpret_cast<fd_set*>({parameter})", dispatch)
        self.assertIn("reinterpret_cast<LinuxKernelTimespec*>(p5)", dispatch)
        self.assertIn(
            "reinterpret_cast<const LinuxPselectSigsetArgument*>(p6)",
            dispatch,
        )

        self.assertRegex(
            wait_abi,
            re.compile(
                r"struct LinuxPselectSigsetArgument\s*\{\s*"
                r"uintptr_t signalMask;\s*size_t signalMaskSize;\s*\};",
                re.DOTALL,
            ),
        )
        self.assertIn(
            "static_assert(sizeof(LinuxPselectSigsetArgument) == 16",
            wait_abi,
        )

        select_source = (
            ROOT / "src/modules/subsys/posix/select-syscalls.cc"
        ).read_text(encoding="utf-8")
        entry = select_source.split("int posix_pselect6", 1)[1]
        outer_argument = entry.index("importPselectSignalArgument")
        timeout = entry.index("importPselectTimespec")
        pointed_mask = entry.index("importPselectSignalMask")
        arm_mask = entry.index("Thread::TemporarySignalMask signalWait")
        wait = entry.index("selectWithDeadline", arm_mask)
        restore_mask = entry.index("signalWait.finish()", wait)
        timeout_copyout = entry.index(
            "copyPselectTimespecRemainder", restore_mask
        )
        self.assertLess(outer_argument, timeout)
        self.assertLess(timeout, pointed_mask)
        self.assertLess(pointed_mask, arm_mask)
        self.assertLess(arm_mask, wait)
        self.assertLess(wait, restore_mask)
        self.assertLess(restore_mask, timeout_copyout)

        select_helper = select_source.split("int selectWithDeadline", 1)[
            1
        ].split("}  // namespace", 1)[0]
        input_copy = select_helper.index("PosixSubsystem::copyFromUser")
        poll = select_helper.index("posix_poll_safe")
        read_output = select_helper.index(
            "PosixSubsystem::copyToUser(readfds", poll
        )
        write_output = select_helper.index(
            "PosixSubsystem::copyToUser(writefds", read_output
        )
        error_output = select_helper.index(
            "PosixSubsystem::copyToUser(errorfds", write_output
        )
        self.assertLess(input_copy, poll)
        self.assertLess(poll, read_output)
        self.assertLess(read_output, write_output)
        self.assertLess(write_output, error_output)
        self.assertIn("const size_t bitmapBytes = selectBitmapBytes(nfds)", select_helper)
        self.assertNotIn("sizeof(fd_set)", select_helper)

        argument_import = select_source.split(
            "bool importPselectSignalArgument", 1
        )[1].split("bool importPselectSignalMask", 1)[0]
        mask_import = select_source.split(
            "bool importPselectSignalMask", 1
        )[1].split("int selectWithDeadline", 1)[0]
        self.assertIn("copyFromUser(&snapshot, userArgument", argument_import)
        self.assertLess(
            mask_import.index("if (!argument.signalMask)"),
            mask_import.index("argument.signalMaskSize != LinuxKernelSigsetSize"),
        )
        self.assertIn("temporaryMask &= ~UnblockableSignals", mask_import)

    def test_bundled_musl_pselect_uses_the_six_argument_argpack(self):
        hosted_musl_cmake = HOSTED_MUSL_CMAKE.read_text(encoding="utf-8")
        self.assertIn('set(MUSL_VERSION "1.2.6")', hosted_musl_cmake)

        source_path = (
            ROOT
            / "build/src/modules/musl-1.2.6/src/select/pselect.c"
        )
        if source_path.exists():
            source = source_path.read_text(encoding="utf-8")
        else:
            archive_path = ROOT / "build/src/modules/musl-1.2.6.tar.gz"
            if not archive_path.exists():
                self.skipTest("the configured musl source archive is not present")
            with tarfile.open(archive_path, "r:gz") as archive:
                member = archive.extractfile(
                    "musl-1.2.6/src/select/pselect.c"
                )
                self.assertIsNotNone(member)
                source = member.read().decode("utf-8")

        self.assertIn("syscall_arg_t data[2]", source)
        self.assertIn("{ (uintptr_t)mask, _NSIG/8 }", source)
        self.assertIn("syscall_cp(SYS_pselect6, n, rfds, wfds, efds,", source)
        self.assertRegex(
            source,
            re.compile(
                r"syscall_cp\(SYS_pselect6, n, rfds, wfds, efds,\s*"
                r"ts \? .*? : 0, data\);",
                re.DOTALL,
            ),
        )

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

    def test_linux_clock_wait_syscalls_use_the_amd64_time_abi(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")
        numbers = (
            ROOT / "src/modules/subsys/posix/syscalls/posixSyscallNumbers.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")
        wait_abi = (
            ROOT / "src/modules/subsys/posix/linux-wait-abi.h"
        ).read_text(encoding="utf-8")
        source = (
            ROOT / "src/modules/subsys/posix/clock-syscalls.cc"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(clock_getres, 229, POSIX_CLOCK_GETRES)",
            mappings,
        )
        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(clock_nanosleep, 230, POSIX_CLOCK_NANOSLEEP)",
            mappings,
        )
        self.assertIn("#define POSIX_CLOCK_GETRES 285", numbers)
        self.assertIn("#define POSIX_CLOCK_NANOSLEEP 286", numbers)
        self.assertRegex(
            wait_abi,
            re.compile(
                r"struct LinuxKernelTimespec\s*\{\s*"
                r"int64_t tv_sec;\s*int64_t tv_nsec;\s*\};",
                re.DOTALL,
            ),
        )

        getres_dispatch = manager.split("case POSIX_CLOCK_GETRES:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_clock_getres", getres_dispatch)
        self.assertIn("if (linuxAbi)", getres_dispatch)
        self.assertIn("reinterpret_cast<LinuxKernelTimespec*>(p2)", getres_dispatch)
        self.assertIn("return posix_clock_getres_native", getres_dispatch)
        self.assertIn("reinterpret_cast<struct timespec*>(p2)", getres_dispatch)

        nanosleep_dispatch = manager.split(
            "case POSIX_CLOCK_NANOSLEEP:", 1
        )[1].split("case ", 1)[0]
        self.assertIn("return posix_clock_nanosleep", nanosleep_dispatch)
        self.assertIn("static_cast<int>(p2)", nanosleep_dispatch)
        self.assertIn(
            "reinterpret_cast<const LinuxKernelTimespec*>(p3)",
            nanosleep_dispatch,
        )
        self.assertIn(
            "reinterpret_cast<LinuxKernelTimespec*>(p4)", nanosleep_dispatch
        )

        native_getres = source.split("int posix_clock_getres_native", 1)[1].split(
            "int posix_clock_getres(clockid_t", 1
        )[0]
        self.assertIn("const struct timespec result = {0, 1}", native_getres)

        getres = source.split("int posix_clock_getres(clockid_t", 1)[1].split(
            "int posix_clock_nanosleep", 1
        )[0]
        self.assertLess(
            getres.index("supportedReadClock"),
            getres.index("if (!resolution)"),
        )
        self.assertIn("const LinuxKernelTimespec result = {0, 1}", getres)

        nanosleep = source.split("int posix_clock_nanosleep", 1)[1].split(
            "int posix_sigaltstack", 1
        )[0]
        self.assertLess(
            nanosleep.index("supportedSleepClock"),
            nanosleep.index("copyFromUser(&requested"),
        )
        self.assertIn("flags & TIMER_ABSTIME", nanosleep)
        self.assertIn("if (!absolute && remainder)", nanosleep)

    def test_bundled_musl_clock_waits_use_raw_linux_syscalls(self):
        source_root = ROOT / "build/src/modules/musl-1.2.6"
        archive_path = ROOT / "build/src/modules/musl-1.2.6.tar.gz"

        def load_source(relative_path):
            source_path = source_root / relative_path
            if source_path.exists():
                return source_path.read_text(encoding="utf-8")
            if not archive_path.exists():
                self.skipTest("the configured musl source archive is not present")
            with tarfile.open(archive_path, "r:gz") as archive:
                member = archive.extractfile(f"musl-1.2.6/{relative_path}")
                self.assertIsNotNone(member)
                return member.read().decode("utf-8")

        getres = load_source("src/time/clock_getres.c")
        nanosleep = load_source("src/time/clock_nanosleep.c")
        self.assertIn("syscall(SYS_clock_getres, clk, ts)", getres)
        self.assertIn(
            "__syscall_cp(SYS_clock_nanosleep, clk, flags, req, rem)",
            nanosleep,
        )
        self.assertIn("if (clk == CLOCK_REALTIME && !flags)", nanosleep)

    def test_linux_thread_signal_syscalls_are_mapped_and_dispatched(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")
        numbers = (
            ROOT / "src/modules/subsys/posix/syscalls/posixSyscallNumbers.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")
        source = (
            ROOT / "src/modules/subsys/posix/signal-syscalls.cc"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(tkill, 200, POSIX_TKILL)",
            mappings,
        )
        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(tgkill, 234, POSIX_TGKILL)",
            mappings,
        )
        self.assertIn("#define POSIX_TKILL 287", numbers)
        self.assertIn("#define POSIX_TGKILL 288", numbers)

        tkill_dispatch = manager.split("case POSIX_TKILL:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_tkill", tkill_dispatch)
        self.assertIn("static_cast<int>(p1)", tkill_dispatch)
        self.assertIn("static_cast<int>(p2)", tkill_dispatch)
        tgkill_dispatch = manager.split("case POSIX_TGKILL:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_tgkill", tgkill_dispatch)
        for parameter in ("p1", "p2", "p3"):
            self.assertIn(f"static_cast<int>({parameter})", tgkill_dispatch)

        tkill = source.split("int posix_tkill", 1)[1].split(
            "int posix_tgkill", 1
        )[0]
        self.assertLess(
            tkill.index("acquireThreadById"),
            tkill.index("sig < 0"),
        )
        self.assertNotIn("doThreadKill", tkill)
        tgkill = source.split("int posix_tgkill", 1)[1].split(
            "int posix_kill", 1
        )[0]
        self.assertLess(
            tgkill.index("acquireThreadById"),
            tgkill.index("sig < 0"),
        )
        self.assertIn("callerProcess != process.get()", tgkill)
        self.assertIn("canSignalProcess(caller, target, sig)", tgkill)
        self.assertNotIn("doThreadKill", tgkill)

        delivery = source.split("static int queueThreadSignal", 2)[2].split(
            "int posix_tkill", 1
        )[0]
        self.assertIn("SignalDeliveryResult::Unavailable", delivery)
        self.assertNotIn("SignalDeliveryResult::Rejected", delivery)

    def test_bundled_musl_raise_and_pthread_kill_use_tkill(self):
        source_root = ROOT / "build/src/modules/musl-1.2.6"
        archive_path = ROOT / "build/src/modules/musl-1.2.6.tar.gz"

        def load_source(relative_path):
            source_path = source_root / relative_path
            if source_path.exists():
                return source_path.read_text(encoding="utf-8")
            if not archive_path.exists():
                self.skipTest("the configured musl source archive is not present")
            with tarfile.open(archive_path, "r:gz") as archive:
                member = archive.extractfile(f"musl-1.2.6/{relative_path}")
                self.assertIsNotNone(member)
                return member.read().decode("utf-8")

        raise_source = load_source("src/signal/raise.c")
        pthread_kill_source = load_source("src/thread/pthread_kill.c")
        self.assertIn("syscall(SYS_tkill", raise_source)
        self.assertIn("__syscall(SYS_tkill", pthread_kill_source)

    def test_linux_rt_sigsuspend_uses_the_raw_two_argument_abi(self):
        mappings = (
            ROOT
            / "src/modules/subsys/posix/syscalls/linuxSyscallMappings-amd64.h"
        ).read_text(encoding="utf-8")
        numbers = (
            ROOT / "src/modules/subsys/posix/syscalls/posixSyscallNumbers.h"
        ).read_text(encoding="utf-8")
        manager = (
            ROOT / "src/modules/subsys/posix/PosixSyscallManager.cc"
        ).read_text(encoding="utf-8")
        source = (
            ROOT / "src/modules/subsys/posix/signal-syscalls.cc"
        ).read_text(encoding="utf-8")

        self.assertIn(
            "PEDIGREE_LINUX_AMD64_SYSCALL(rt_sigsuspend, 130, "
            "POSIX_RT_SIGSUSPEND)",
            mappings,
        )
        self.assertIn("#define POSIX_RT_SIGSUSPEND 289", numbers)
        dispatch = manager.split("case POSIX_RT_SIGSUSPEND:", 1)[1].split(
            "case ", 1
        )[0]
        self.assertIn("return posix_rt_sigsuspend", dispatch)
        self.assertIn("reinterpret_cast<const uint64_t*>(p1)", dispatch)
        self.assertIn("static_cast<size_t>(p2)", dispatch)

        entry = source.split("int posix_rt_sigsuspend", 1)[1].split(
            "size_t posix_alarm", 1
        )[0]
        size_check = entry.index("signalMaskSize != KernelSigsetSize")
        guarded_copy = entry.index("PosixSubsystem::copyFromUser")
        arm_mask = entry.index("Thread::TemporarySignalMask signalWait")
        wait = entry.index("thread->waitForEventOrSignalInterruption()")
        finish = entry.index("signalWait.finish()")
        interrupted = entry.index("SYSCALL_ERROR(Interrupted)")
        self.assertLess(size_check, guarded_copy)
        self.assertLess(guarded_copy, arm_mask)
        self.assertLess(arm_mask, wait)
        self.assertLess(wait, finish)
        self.assertLess(finish, interrupted)
        self.assertIn("SIGKILL - 1", entry)
        self.assertIn("SIGSTOP - 1", entry)
        self.assertIn("temporaryMask &= ~UnblockableSignals", entry)
        self.assertIn("!thread->waitForEventOrSignalInterruption()", entry)
        self.assertIn("getUnwindState() == Thread::Continue", entry)

        source_root = ROOT / "build/src/modules/musl-1.2.6"
        archive_path = ROOT / "build/src/modules/musl-1.2.6.tar.gz"
        source_path = source_root / "src/signal/sigsuspend.c"
        if source_path.exists():
            musl_source = source_path.read_text(encoding="utf-8")
        elif archive_path.exists():
            with tarfile.open(archive_path, "r:gz") as archive:
                member = archive.extractfile(
                    "musl-1.2.6/src/signal/sigsuspend.c"
                )
                self.assertIsNotNone(member)
                musl_source = member.read().decode("utf-8")
        else:
            self.skipTest("the configured musl source archive is not present")

        self.assertIn("int sigsuspend(const sigset_t *mask)", musl_source)
        self.assertIn(
            "return syscall_cp(SYS_rt_sigsuspend, mask, _NSIG/8);",
            musl_source,
        )

    def test_signal_return_accepts_iret_and_sysret_user_selectors(self):
        signal_source = (
            ROOT / "src/modules/subsys/posix/linux-amd64-signal.cc"
        ).read_text(encoding="utf-8")

        self.assertIn("IretUserCodeSegment = 0x1B", signal_source)
        self.assertIn("SysretUserCodeSegment = 0x2B", signal_source)
        self.assertEqual(signal_source.count("userCodeSegment("), 4)

    def test_linux_signal_frames_use_guarded_user_copies(self):
        signal_source = (
            ROOT / "src/modules/subsys/posix/linux-amd64-signal.cc"
        ).read_text(encoding="utf-8")
        bad_frame = signal_source.split("void badFrame()", 1)[1].split(
            "}", 1
        )[0]
        self.assertIn("deferSignalExit(SIGSEGV)", bad_frame)
        self.assertNotIn("requestProcessExit", bad_frame)

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
            "thread->commitSignalHandlerMask(handlerMask",
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

        hosted_musl_cmake = HOSTED_MUSL_CMAKE.read_text(
            encoding="utf-8"
        )

        self.assertIn(
            "PEDIGREE_CONFIG_INCLUDE_DIR=${PEDIGREE_MUSL_CONFIG_DIR}",
            hosted_musl_cmake,
        )
        self.assertNotIn("${CMAKE_BINARY_DIR}/config.h", hosted_musl_cmake)

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
