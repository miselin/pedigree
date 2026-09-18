import re
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


class InotifyRoutingTests(unittest.TestCase):
    def test_exact_musl_wrapper_and_linux_amd64_abi(self):
        wrapper = read_musl_source("src/linux/inotify.c")
        header = read_musl_source("include/sys/inotify.h")
        syscalls = read_musl_source("arch/x86_64/bits/syscall.h.in")

        self.assertIn("return inotify_init1(0);", wrapper)
        self.assertIn("__syscall(SYS_inotify_init1, flags)", wrapper)
        self.assertIn("r==-ENOSYS && !flags", wrapper)
        self.assertIn("syscall(SYS_inotify_add_watch, fd, pathname, mask)", wrapper)
        self.assertIn("syscall(SYS_inotify_rm_watch, fd, wd)", wrapper)

        self.assertIn("int wd;", header)
        self.assertIn("uint32_t mask, cookie, len;", header)
        self.assertIn("char name[];", header)
        self.assertIn("#define IN_NONBLOCK O_NONBLOCK", header)
        self.assertIn("#define IN_CLOEXEC O_CLOEXEC", header)
        self.assertIn("#define IN_MASK_CREATE   0x10000000", header)
        self.assertIn("#define IN_MASK_ADD      0x20000000", header)
        self.assertIn("#define IN_ISDIR         0x40000000", header)
        self.assertIn("#define IN_ONESHOT       0x80000000", header)

        for name, number in (
            ("inotify_init", 253),
            ("inotify_add_watch", 254),
            ("inotify_rm_watch", 255),
            ("inotify_init1", 294),
        ):
            self.assertRegex(
                syscalls,
                rf"#define\s+__NR_{re.escape(name)}\s+{number}\b",
            )

    def test_linux_syscalls_map_to_dedicated_posix_handlers(self):
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

        expected = (
            "PEDIGREE_LINUX_AMD64_SYSCALL(inotify_init, 253, POSIX_INOTIFY_INIT)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(inotify_add_watch, 254, POSIX_INOTIFY_ADD_WATCH)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(inotify_rm_watch, 255, POSIX_INOTIFY_RM_WATCH)",
            "PEDIGREE_LINUX_AMD64_SYSCALL(inotify_init1, 294, POSIX_INOTIFY_INIT1)",
        )
        for mapping in expected:
            with self.subTest(mapping=mapping):
                self.assertIn(mapping, mappings)
        for number in range(300, 304):
            self.assertIn(f" {number}", numbers)
        for case in (
            "POSIX_CASE(POSIX_INOTIFY_INIT)",
            "POSIX_CASE(POSIX_INOTIFY_INIT1)",
            "POSIX_CASE(POSIX_INOTIFY_ADD_WATCH)",
            "POSIX_CASE(POSIX_INOTIFY_RM_WATCH)",
        ):
            self.assertIn(case, manager)

    def test_queue_and_watch_semantics_cover_epoll_sensitive_edges(self):
        implementation = (
            ROOT / "src/modules/subsys/posix/inotify-syscalls.cc"
        ).read_text(encoding="utf-8")
        declarations = (
            ROOT / "src/modules/subsys/posix/inotify-syscalls.h"
        ).read_text(encoding="utf-8")
        epoll = (
            ROOT / "src/modules/subsys/posix/epoll-syscalls.cc"
        ).read_text(encoding="utf-8")
        poll = (
            ROOT / "src/modules/subsys/posix/poll-syscalls.cc"
        ).read_text(encoding="utf-8")

        self.assertIn("sizeof(LinuxInotifyEvent) == 16", declarations)
        self.assertIn("mask |= eventMask & ~LinuxInotify::MaskAdd", implementation)
        self.assertNotIn("updateMask(AcceptedMask", implementation)
        self.assertIn("!(last->mask & LinuxInotify::Ignored)", implementation)
        self.assertNotIn("last->cookie == cookie", implementation)
        self.assertIn("owner->eventsQueued();", implementation)
        self.assertIn("notifyReadiness(ReadyRead);", implementation)
        self.assertIn("!event.name.length() && isDirectory", implementation)
        self.assertIn("selected |= LinuxInotify::IsDirectory", implementation)
        self.assertIn("LinuxInotify::DeleteSelf | LinuxInotify::MoveSelf", implementation)
        self.assertIn("const bool deleted = event.mask & FileEvents::DeletedSelf", implementation)
        self.assertIn("reapInactiveWatches();", implementation)
        self.assertIn("watch->subscription.reset();", implementation)
        self.assertIn("LinuxInotify::QueueOverflow", implementation)
        self.assertIn("paddedNameLength", implementation)

        enqueue = re.search(
            r"bool enqueue\(.*?\n  int readOne\(", implementation, re.DOTALL
        )
        self.assertIsNotNone(enqueue)
        self.assertLess(
            enqueue.group(0).index("events.count() >= MaxQueuedEvents"),
            enqueue.group(0).index("events.rbegin()"),
        )

        remove_watch = re.search(
            r"int InotifyInstance::removeWatch.*?\nint InotifyInstance::readEvents",
            implementation,
            re.DOTALL,
        )
        self.assertIsNotNone(remove_watch)
        self.assertLess(
            remove_watch.group(0).index("subscription.reset()"),
            remove_watch.group(0).index("observerImpl->deactivate()"),
        )

        inotify_read = re.search(
            r"SharedPointer<InotifyInstance> inotify.*?if \(pFd->networkImpl\)",
            (
                ROOT / "src/modules/subsys/posix/file-syscalls.cc"
            ).read_text(encoding="utf-8"),
            re.DOTALL,
        )
        self.assertIsNotNone(inotify_read)
        self.assertNotIn("checkUserBuffer", inotify_read.group(0))
        self.assertIn("readEventsToUser", inotify_read.group(0))
        self.assertIn("readOne", implementation)
        self.assertIn("outcome = -1;", implementation)
        self.assertIn("getCurrentThread()->setErrno(0)", implementation)

        self.assertIn("SharedPointer<InotifyInstance> inotify", epoll)
        self.assertIn("return watch.inotify->queryReady();", epoll)
        self.assertIn("return inotify.get();", poll)
        self.assertIn("inotify->queryReady()", poll)

    def test_vfs_events_are_published_at_namespace_and_file_boundaries(self):
        file_header = (ROOT / "src/modules/system/vfs/File.h").read_text(
            encoding="utf-8"
        )
        file_source = (ROOT / "src/modules/system/vfs/File.cc").read_text(
            encoding="utf-8"
        )
        directory = (ROOT / "src/modules/system/vfs/Directory.cc").read_text(
            encoding="utf-8"
        )
        filesystem = (ROOT / "src/modules/system/vfs/Filesystem.cc").read_text(
            encoding="utf-8"
        )
        descriptors = (
            ROOT / "src/modules/subsys/posix/FileDescriptor.cc"
        ).read_text(encoding="utf-8")
        file_events = (ROOT / "src/modules/system/vfs/FileEvent.cc").read_text(
            encoding="utf-8"
        )

        self.assertIn("public ReadinessSource, public FileEventSource", file_header)
        self.assertIn("publishEvent(FileEvents::Modify)", file_source)
        self.assertIn("getNamespace(parent, childName)", file_source)
        self.assertIn("parent.get()->notifyFileEvent", file_source)
        self.assertIn("publishEvent(FileEvents::Created", directory)
        self.assertIn("directory->publishEvent(FileEvents::Removed", filesystem)
        self.assertIn("target->publishEvent(FileEvents::DeletedSelf)", filesystem)
        self.assertIn("FileEvents::CloseNoWrite", descriptors)
        self.assertIn("FileEvents::CloseWrite", descriptors)
        self.assertIn("target->admit()", file_events)
        self.assertIn("target->notifyAdmitted(*finalEvent)", file_events)
        self.assertLess(
            file_events.index("target->admit()"),
            file_events.index("target->notifyAdmitted(*finalEvent)"),
        )

    def test_target_probe_uses_public_musl_api_and_raw_legacy_number(self):
        probe = (
            ROOT / "src/user/applications/inotify-test/main.c"
        ).read_text(encoding="utf-8")

        self.assertIn("syscall(SYS_inotify_init)", probe)
        self.assertIn("public_fd = inotify_init()", probe)
        self.assertIn("inotify_init1(IN_NONBLOCK | IN_CLOEXEC)", probe)
        self.assertIn("IN_MODIFY | IN_ATTRIB | IN_MASK_ADD", probe)
        self.assertIn("IN_ATTRIB | IN_ISDIR", probe)
        self.assertIn("IN_MODIFY | IN_ONESHOT", probe)
        self.assertIn("epoll_wait", probe)
        self.assertIn("poll(&poll_state", probe)
        self.assertIn("bad_read_buffer", probe)
        self.assertIn("INOTIFY-TEST: PASS", probe)


if __name__ == "__main__":
    unittest.main()
