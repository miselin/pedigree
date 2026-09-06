import hashlib
import re
import shutil
import subprocess
import tempfile
import textwrap
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[1]
SYSCALL_DIR = ROOT / "src/modules/subsys/posix/syscalls"
MAPPING = SYSCALL_DIR / "linuxSyscallMappings-amd64.h"
TRANSLATE = SYSCALL_DIR / "translate.h"
CC = shutil.which("cc")


def load_mapping():
    pattern = re.compile(
        r"^PEDIGREE_LINUX_AMD64_SYSCALL\("
        r"([a-z0-9_]+),\s*(0x[0-9a-fA-F]+|[0-9]+),\s*([A-Z0-9_]+)\)$",
        re.MULTILINE,
    )
    return [
        (name, int(number, 0), target)
        for name, number, target in pattern.findall(MAPPING.read_text())
    ]


class PosixSyscallTranslationTests(unittest.TestCase):
    def test_owned_map_matches_pre_migration_musl_mapping(self):
        mapping = load_mapping()
        serialized = "".join(
            f"{name}={number}:{target}\n" for name, number, target in mapping
        )

        # Snapshot of the previous translate.h resolved against musl 1.2.6's
        # x86_64 bits/syscall.h, plus Pedigree's explicit vfork compatibility
        # route, the five epoll entry points, both eventfd entry points, and
        # ppoll, pselect6, clock_getres, clock_nanosleep, tkill, tgkill, and
        # rt_sigsuspend, pread64, pwrite64, preadv, pwritev, preadv2,
        # pwritev2, prlimit64, membarrier, faccessat2, dup3, getrusage,
        # the four inotify entry points, 18 IPC entry points, and nine signal,
        # timer and clock entry points, five signalfd/timerfd entry points,
        # three VM entry points, memfd_create, sendfile, copy_file_range,
        # splice, tee, vmsplice, five memory-lock entry points, remap_file_pages,
        # nine additional xattr entry points, four fanotify/file-handle entries,
        # four filesystem-ID/process-memory entries, three namespace entries,
        # ten scheduling entries, waitid, cooperative ptrace, four file sync/advice
        # entries, two clock adjustment entries, module removal, sync/syncfs, getsid, fallocate, renameat2, recvmmsg, init_module and execveat.
        # This locks both sides of all 272 mappings.
        self.assertEqual(len(mapping), 272)
        self.assertEqual(
            hashlib.sha256(serialized.encode()).hexdigest(),
            "9fac2e7d58d7d915803b1f2b2cb4015cbc33937206e555700beef82f96d5c8b6",
        )
        self.assertEqual(len({name for name, _, _ in mapping}), len(mapping))
        self.assertEqual(len({number for _, number, _ in mapping}), len(mapping))

    def test_vfork_keeps_the_existing_safe_fork_behavior(self):
        self.assertIn(("vfork", 58, "POSIX_FORK"), load_mapping())

    def test_getrusage_uses_the_existing_resource_accounting_handler(self):
        self.assertIn(("getrusage", 98, "POSIX_GETRUSAGE"), load_mapping())

    @unittest.skipUnless(CC, "requires a native C compiler")
    def test_every_owned_number_translates_without_libc_headers(self):
        self.assertNotIn("bits/syscall.h", TRANSLATE.read_text())

        source = textwrap.dedent(
            """\
            #include "translate.h"

            int main(void) {
            #define PEDIGREE_LINUX_AMD64_SYSCALL(name, number, target) \\
              if (posix_translate_syscall(number) != target) return 1;
            #include "linuxSyscallMappings-amd64.h"
            #undef PEDIGREE_LINUX_AMD64_SYSCALL

              if (posix_translate_syscall(-1) != -1) return 2;
              if (posix_translate_syscall(1024) != -1) return 3;
              return 0;
            }
            """
        )

        with tempfile.TemporaryDirectory() as temporary:
            temporary_path = Path(temporary)
            source_path = temporary_path / "translation.c"
            executable = temporary_path / "translation"
            source_path.write_text(source)
            result = subprocess.run(
                [
                    CC,
                    "-std=c11",
                    "-Wall",
                    "-Wextra",
                    "-Werror",
                    "-nostdinc",
                    "-DHOSTED=1",
                    "-I",
                    str(SYSCALL_DIR),
                    str(source_path),
                    "-o",
                    str(executable),
                ],
                capture_output=True,
                text=True,
            )
            self.assertEqual(
                result.returncode, 0, msg=result.stdout + result.stderr
            )

            result = subprocess.run(
                [str(executable)], capture_output=True, text=True
            )
            self.assertEqual(
                result.returncode, 0, msg=result.stdout + result.stderr
            )


if __name__ == "__main__":
    unittest.main()
