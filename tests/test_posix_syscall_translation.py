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
TRANSLATE = SYSCALL_DIR / "translate.h"
CC = shutil.which("cc")


def load_mapping(architecture="amd64"):
    pattern = re.compile(
        rf"^PEDIGREE_LINUX_{architecture.upper()}_SYSCALL\("
        r"([a-z0-9_]+),\s*(0x[0-9a-fA-F]+|[0-9]+),\s*([A-Z0-9_]+)\)$",
        re.MULTILINE,
    )
    return [
        (name, int(number, 0), target)
        for name, number, target in pattern.findall(
            (SYSCALL_DIR / f"linuxSyscallMappings-{architecture}.h").read_text()
        )
    ]


class PosixSyscallTranslationTests(unittest.TestCase):
    def test_owned_map_matches_pre_migration_musl_mapping(self):
        mapping = load_mapping()
        serialized = "".join(
            f"{name}={number}:{target}\n" for name, number, target in mapping
        )

        # Snapshot of the previous translate.h resolved against musl 1.2.6's
        # x86_64 bits/syscall.h, plus Pedigree's explicit vfork compatibility
        # route, clone3, the five epoll entry points, both eventfd entry points, and
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
        # entries, two clock adjustment entries, module removal, sync/syncfs,
        # getsid, fallocate, renameat2, recvmmsg, init_module, execveat, acct,
        # vhangup, quotactl, swapon/swapoff, sysinfo, personality, pivot_root,
        # truncate, lchown, utimensat, statx, fchmodat2 and mknodat.
        # This locks both sides of all 287 mappings.
        self.assertEqual(len(mapping), 287)
        self.assertEqual(
            hashlib.sha256(serialized.encode()).hexdigest(),
            "5a98e1652bfca347cba268446c7a1e8e270a92bc943621040a9c7f9994a056bd",
        )
        self.assertEqual(len({name for name, _, _ in mapping}), len(mapping))
        self.assertEqual(len({number for _, number, _ in mapping}), len(mapping))

    def test_vfork_has_a_distinct_shared_vm_route(self):
        self.assertIn(("vfork", 58, "POSIX_VFORK"), load_mapping())

    def test_getrusage_uses_the_existing_resource_accounting_handler(self):
        self.assertIn(("getrusage", 98, "POSIX_GETRUSAGE"), load_mapping())

    def test_job_control_syscalls_have_linux_numbers(self):
        for architecture, numbers in (
            (
                "amd64",
                {
                    "setpgid": 109,
                    "getpgrp": 111,
                    "setsid": 112,
                    "getpgid": 121,
                    "getsid": 124,
                },
            ),
            ("arm64", {"setpgid": 154, "getpgid": 155, "getsid": 156, "setsid": 157}),
            (
                "armv7",
                {
                    "setpgid": 57,
                    "getpgrp": 65,
                    "setsid": 66,
                    "getpgid": 132,
                    "getsid": 147,
                },
            ),
        ):
            with self.subTest(architecture=architecture):
                mapping = load_mapping(architecture)
                for name, number in numbers.items():
                    self.assertIn((name, number, f"POSIX_{name.upper()}"), mapping)

    def test_alpine_login_syscalls_have_linux_numbers(self):
        for architecture, expected in (
            (
                "arm64",
                (
                    ("fchmod", 52, "POSIX_FCHMOD"),
                    ("fchown", 55, "POSIX_FCHOWN"),
                    ("ppoll", 73, "POSIX_PPOLL"),
                    ("setgid", 144, "POSIX_SETGID"),
                    ("setuid", 146, "POSIX_SETUID"),
                    ("setgroups", 159, "POSIX_SETGROUPS"),
                ),
            ),
            (
                "armv7",
                (
                    ("access", 33, "POSIX_ACCESS"),
                    ("umask", 60, "POSIX_UMASK"),
                    ("readlink", 85, "POSIX_READLINK"),
                    ("fchmod", 94, "POSIX_FCHMOD"),
                    ("poll", 168, "POSIX_POLL"),
                    ("setgroups32", 206, "POSIX_SETGROUPS"),
                    ("fchown32", 207, "POSIX_FCHOWN"),
                    ("setuid32", 213, "POSIX_SETUID"),
                    ("setgid32", 214, "POSIX_SETGID"),
                ),
            ),
        ):
            with self.subTest(architecture=architecture):
                mapping = load_mapping(architecture)
                for entry in expected:
                    self.assertIn(entry, mapping)

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

        for architecture in ("amd64", "arm64", "armv7"):
            with (
                self.subTest(architecture=architecture),
                tempfile.TemporaryDirectory() as temporary,
            ):
                temporary_path = Path(temporary)
                source_path = temporary_path / "translation.c"
                executable = temporary_path / "translation"
                source_path.write_text(
                    source.replace("AMD64", architecture.upper()).replace(
                        "amd64", architecture
                    )
                )
                result = subprocess.run(
                    [
                        CC,
                        "-std=c11",
                        "-Wall",
                        "-Wextra",
                        "-Werror",
                        "-nostdinc",
                        "-DHOSTED=1",
                        f"-D{architecture.upper()}=1",
                        "-I",
                        str(SYSCALL_DIR),
                        str(source_path),
                        "-o",
                        str(executable),
                    ],
                    capture_output=True,
                    text=True,
                    check=False,
                )
                self.assertEqual(
                    result.returncode, 0, msg=result.stdout + result.stderr
                )

                result = subprocess.run(
                    [str(executable)], capture_output=True, text=True, check=False
                )
                self.assertEqual(
                    result.returncode, 0, msg=result.stdout + result.stderr
                )


if __name__ == "__main__":
    unittest.main()
