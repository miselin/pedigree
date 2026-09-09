"""Run the real seed transaction with host files and an intercepted entropy ioctl.

Root identity is simulated; modes, file types, links, rename, and flock are real.
Recorded fsync completion proves ordering, not power-loss durability on Pedigree.
"""

import fcntl
import hashlib
import hmac
import os
from pathlib import Path
import select
import shlex
import subprocess
import tempfile
import unittest


HARNESS = r"""
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/file.h>
#include <sys/ioctl.h>
#include <sys/random.h>
#include <sys/stat.h>
#include <unistd.h>

static bool fileSynced = false, renamed = false, directorySynced = false;
static bool readInterrupted = false, writeInterrupted = false;

static bool option(const char* name) {
  const char* value = getenv("SEED_TEST_FAILURE");
  return value && !strcmp(value, name);
}

static void record(const std::string& event) {
  FILE* trace = fopen(getenv("SEED_TEST_TRACE"), "a");
  if (!trace)
    _exit(90);
  fprintf(trace, "%s\n", event.c_str());
  if (fclose(trace))
    _exit(91);
}

static std::string hexadecimal(const unsigned char* bytes, size_t length) {
  static const char digits[] = "0123456789abcdef";
  std::string result;
  for (size_t i = 0; i < length; ++i) {
    result += digits[bytes[i] >> 4];
    result += digits[bytes[i] & 15];
  }
  return result;
}

static uid_t test_geteuid() {
  return option("nonroot") ? 1000 : 0;
}

static int test_fstat(int fd, struct stat* st) {
  int result = ::fstat(fd, st);
  if (!result)
    st->st_uid = option("owner") ? 1000 : 0;
  return result;
}

static int test_fsync(int fd) {
  struct stat st;
  if (::fstat(fd, &st))
    return -1;
  const bool directory = S_ISDIR(st.st_mode);
  const char* event = directory ? "fsync-directory" : "fsync-file";
  record(std::string(event) + " begin");
  if (option(event)) {
    errno = EIO;
    return -1;
  }
  if (::fsync(fd))
    return -1;
  (directory ? directorySynced : fileSynced) = true;
  record(std::string(event) + " done");
  return 0;
}

static int test_renameat(int oldfd, const char* oldname, int newfd, const char* newname) {
  record("rename begin");
  if (!fileSynced)
    _exit(92);
  if (option("rename")) {
    errno = EIO;
    return -1;
  }
  if (::renameat(oldfd, oldname, newfd, newname))
    return -1;
  renamed = true;
  record("rename done");
  return 0;
}

static int test_flock(int fd, int flags) {
  if (::flock(fd, flags))
    return -1;
  const char* gate = getenv("SEED_TEST_LOCK_GATE");
  if (gate) {
    puts("transaction locked");
    fflush(stdout);
    for (size_t attempt = 0; access(gate, F_OK); ++attempt) {
      if (attempt == 1000)
        _exit(93);
      usleep(10000);
    }
  }
  return 0;
}

static ssize_t test_read(int fd, void* bytes, size_t count) {
  if (option("partial-io")) {
    if (!readInterrupted) {
      readInterrupted = true;
      errno = EINTR;
      return -1;
    }
    if (count > 7)
      count = 7;
  }
  return ::read(fd, bytes, count);
}

static ssize_t test_write(int fd, const void* bytes, size_t count) {
  if (option("write")) {
    errno = EIO;
    return -1;
  }
  if (option("partial-io")) {
    if (!writeInterrupted) {
      writeInterrupted = true;
      errno = EINTR;
      return -1;
    }
    if (count > 7)
      count = 7;
  }
  return ::write(fd, bytes, count);
}

static int test_ioctl(int, unsigned long command, void* data) {
  record("ioctl called");
  if (!fileSynced || !renamed || !directorySynced || command != 0x40085203UL)
    _exit(94);
  struct Request {
    int entropyBits, bytes;
    unsigned char seed[32];
  } request;
  memcpy(&request, data, sizeof(request));
  if (request.entropyBits != 256 || request.bytes != 32)
    _exit(95);
  FILE* saved = fopen(getenv("SEED_TEST_PATH"), "rb");
  unsigned char successor[33];
  if (!saved || fread(successor, 1, sizeof(successor), saved) != 32 || fclose(saved))
    _exit(96);
  if (hexadecimal(successor, 32) != getenv("SEED_TEST_SUCCESSOR"))
    _exit(97);
  record("kernel " + hexadecimal(request.seed, sizeof(request.seed)));
  record("successor " + hexadecimal(successor, 32));
  return 0;
}

static ssize_t test_getrandom(void*, size_t, unsigned) {
  errno = EAGAIN;
  return -1;
}

#define geteuid test_geteuid
#define fstat test_fstat
#define fsync test_fsync
#define renameat test_renameat
#define flock test_flock
#define read test_read
#define write test_write
#define ioctl test_ioctl
#define getrandom test_getrandom
#include "main.cc"
"""


class RandomSeedBootstrapTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.build = tempfile.TemporaryDirectory(prefix="pedigree-seed-bootstrap-build-")
        cls.addClassCleanup(cls.build.cleanup)
        temporary = Path(cls.build.name)
        repository = Path(__file__).resolve().parents[1]
        # macOS lacks this Linux header; --check is stubbed and never draws host entropy.
        (temporary / "sys").mkdir()
        (temporary / "sys/random.h").write_text("#pragma once\n#define GRND_NONBLOCK 1\n")
        harness = temporary / "bootstrap.cc"
        harness.write_text(HARNESS)
        cls.binary = temporary / "bootstrap"
        compiler = shlex.split(os.environ.get("CXX", "c++"))
        subprocess.run(
            [
                *compiler,
                "-std=c++17",
                "-O1",
                "-Wall",
                "-Wextra",
                "-Werror",
                "-fsanitize=address,undefined",
                f"-I{temporary}",
                f"-I{repository / 'src/system/include'}",
                f"-I{repository / 'src/user/applications/random-seed'}",
                str(harness),
                "-o",
                str(cls.binary),
            ],
            check=True,
            capture_output=True,
            text=True,
        )

    def setUp(self):
        temporary = tempfile.TemporaryDirectory(prefix="pedigree-seed-bootstrap-case-")
        self.addCleanup(temporary.cleanup)
        self.directory = Path(temporary.name)
        self.private = self.directory / "private"
        self.private.mkdir(mode=0o700)
        self.path = self.private / "random-seed"
        self.original = bytes(range(32))
        self.path.write_bytes(self.original)
        self.path.chmod(0o600)
        self.successor = hmac.digest(self.original, b"Pedigree saved seed v1", hashlib.sha256)
        self.kernel = hmac.digest(self.original, b"Pedigree kernel seed v1", hashlib.sha256)
        self.trace = self.directory / "trace"

    def environment(self, failure="", **extra):
        return {
            **os.environ,
            "SEED_TEST_FAILURE": failure,
            "SEED_TEST_PATH": str(self.path),
            "SEED_TEST_TRACE": str(self.trace),
            "SEED_TEST_SUCCESSOR": self.successor.hex(),
            **extra,
        }

    def run_bootstrap(self, failure="", path=None):
        return subprocess.run(
            [str(self.binary), str(path or self.path)],
            env=self.environment(failure),
            capture_output=True,
            text=True,
            timeout=15,
        )

    def events(self):
        return self.trace.read_text().splitlines() if self.trace.exists() else []

    def assert_rejected(self, failure="", message=None, path=None):
        result = self.run_bootstrap(failure, path)
        self.assertEqual(result.returncode, 1, result.stdout + result.stderr)
        self.assertIn("seed not activated", result.stderr)
        if message:
            self.assertIn(message, result.stderr)
        self.assertNotIn("ioctl called", self.events())
        return result

    def assert_success(self, failure=""):
        result = self.run_bootstrap(failure)
        self.assertEqual(result.returncode, 0, result.stdout + result.stderr)
        self.assertEqual(self.path.read_bytes(), self.successor)
        self.assertEqual(self.path.stat().st_mode & 0o777, 0o600)
        self.assertEqual(self.path.stat().st_nlink, 1)
        self.assertFalse(self.path.with_name(self.path.name + ".next").exists())
        self.assertEqual(
            self.events(),
            [
                "fsync-file begin", "fsync-file done", "rename begin", "rename done",
                "fsync-directory begin", "fsync-directory done", "ioctl called",
                "kernel " + self.kernel.hex(), "successor " + self.successor.hex(),
            ],
        )

    def test_success_derives_independent_domains_after_persisting_successor(self):
        self.assert_success()

    def test_partial_reads_writes_and_interrupted_transfers(self):
        self.assert_success("partial-io")

    def test_file_fsync_failure_does_not_rename_or_activate(self):
        self.assert_rejected("fsync-file", "persist successor")
        self.assertEqual(self.path.read_bytes(), self.original)
        self.assertEqual(self.events(), ["fsync-file begin"])

    def test_directory_fsync_failure_keeps_successor_but_never_activates(self):
        self.assert_rejected("fsync-directory", "persist seed replacement")
        self.assertEqual(self.path.read_bytes(), self.successor)
        self.assertEqual(self.events()[-1], "fsync-directory begin")

    def test_rename_failure_keeps_original_and_does_not_activate(self):
        self.assert_rejected("rename", "persist seed replacement")
        self.assertEqual(self.path.read_bytes(), self.original)
        self.assertEqual(self.events()[-1], "rename begin")

    def test_write_failure_keeps_original_and_does_not_activate(self):
        self.assert_rejected("write", "persist successor")
        self.assertEqual(self.path.read_bytes(), self.original)
        self.assertEqual(self.events(), [])

    def test_stale_successor_is_preserved_and_rejected(self):
        stale = self.path.with_name(self.path.name + ".next")
        stale.write_bytes(b"incomplete synthetic successor")
        self.assert_rejected(message="create successor")
        self.assertEqual(stale.read_bytes(), b"incomplete synthetic successor")
        self.assertEqual(self.path.read_bytes(), self.original)

    def test_exact_seed_length(self):
        for length in (0, 1, 31, 33, 64):
            with self.subTest(length=length):
                contents = bytes(range(length))
                self.path.write_bytes(contents)
                self.assert_rejected()
                self.assertEqual(self.path.read_bytes(), contents)

    def test_seed_mode_must_be_private(self):
        self.path.chmod(0o644)
        self.assert_rejected(message="open private seed file")

    def test_parent_mode_must_be_private(self):
        self.private.chmod(0o755)
        self.assert_rejected(message="open private seed directory")

    def test_lock_mode_must_be_private(self):
        lock = self.path.with_name(self.path.name + ".lock")
        lock.touch(mode=0o644)
        self.assert_rejected(message="lock seed transaction")

    def test_nonroot_and_untrusted_owner(self):
        self.assert_rejected("nonroot", "requires root")
        self.assert_rejected("owner", "open private seed directory")

    def test_seed_symlink_is_rejected(self):
        target = self.private / "target"
        self.path.rename(target)
        self.path.symlink_to(target.name)
        self.assert_rejected(message="open private seed file")
        self.assertEqual(target.read_bytes(), self.original)

    def test_parent_symlink_is_rejected(self):
        alias = self.directory / "alias"
        alias.symlink_to(self.private, target_is_directory=True)
        self.assert_rejected(message="open private seed directory", path=alias / self.path.name)

    def test_nonregular_and_hardlinked_seeds_are_rejected(self):
        self.path.unlink()
        self.path.mkdir(mode=0o700)
        self.assert_rejected(message="open private seed file")
        self.path.rmdir()
        self.path.write_bytes(self.original)
        self.path.chmod(0o600)
        os.link(self.path, self.private / "hardlink")
        self.assert_rejected(message="open private seed file")

    def test_missing_seed_is_rejected(self):
        self.path.unlink()
        self.assert_rejected(message="open private seed file")

    def test_concurrent_transaction_is_excluded_before_consuming_seed(self):
        gate = self.directory / "release-lock"
        first = subprocess.Popen(
            [str(self.binary), str(self.path)],
            env=self.environment(SEED_TEST_LOCK_GATE=str(gate)),
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        try:
            readable, _, _ = select.select([first.stdout], [], [], 10)
            self.assertTrue(readable, "first transaction did not acquire its lock")
            self.assertEqual(first.stdout.readline().strip(), "transaction locked")
            self.assert_rejected(message="lock seed transaction")
            self.assertEqual(self.path.read_bytes(), self.original)
            self.assertEqual(self.events(), [])
            gate.touch()
            stdout, stderr = first.communicate(timeout=10)
            self.assertEqual(first.returncode, 0, stdout + stderr)
            self.assertEqual(self.path.read_bytes(), self.successor)
            self.assertEqual(self.events().count("ioctl called"), 1)
        finally:
            if first.poll() is None:
                first.kill()
            first.communicate(timeout=5)
        # The lock is deliberately persistent, but must be released on process exit.
        with self.path.with_name(self.path.name + ".lock").open("r+b") as lock:
            fcntl.flock(lock, fcntl.LOCK_EX | fcntl.LOCK_NB)


if __name__ == "__main__":
    unittest.main()
