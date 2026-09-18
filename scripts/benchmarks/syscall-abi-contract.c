/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <posixSyscallNumbers.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#if !defined(__x86_64__)
#error This contract tests the Pedigree x64 syscall register ABIs.
#endif
_Static_assert(SYS_fchmodat == 268, "Expected Linux amd64 fchmodat numbering");

#define RBX_SENTINEL 0x6258a19bL
#define UNUSED 0x7193a5c7L
static volatile sig_atomic_t child_pid;
static int active_abi;

struct result {
  long value, rbx;
};

static struct result raw_linux(long number, long a1, long a2, long a3, long a4, long a5, long a6) {
  long rbx = RBX_SENTINEL;
  register long r10 __asm__("r10") = a4;
  register long r8 __asm__("r8") = a5;
  register long r9 __asm__("r9") = a6;
  __asm__ volatile("syscall"
                   : "+a"(number), "+b"(rbx)
                   : "D"(a1), "S"(a2), "d"(a3), "r"(r10), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory", "cc");
  return (struct result){number, rbx};
}

static struct result raw_native(long number, long a1, long a2, long a3, long a4, long a5, long a6) {
  number = (1L << 16) | number;
  register long r8 __asm__("r8") = a5;
  register long r9 __asm__("r9") = a6;
  // The native ABI returns errno in the same register used for argument one.
  __asm__ volatile("syscall"
                   : "+a"(number), "+b"(a1)
                   : "d"(a2), "S"(a3), "D"(a4), "r"(r8), "r"(r9)
                   : "rcx", "r11", "memory", "cc");
  return (struct result){number, a1};
}

static struct result invoke(int native, long linux_number, long native_number, long a1, long a2,
                            long a3, long a4, long a5, long a6) {
  active_abi = native;
  errno = EDOM;
  return native ? raw_native(native_number, a1, a2, a3, a4, a5, a6)
                : raw_linux(linux_number, a1, a2, a3, a4, a5, a6);
}

static void fail(const char* operation) {
  fprintf(stderr, "ABI-CONTRACT FAIL abi=%s operation=%s errno=%d\n",
          active_abi ? "native" : "linux", operation, errno);
  if (child_pid > 0)
    kill(child_pid, SIGKILL);
  _exit(1);
}

static void require(int condition, const char* operation) {
  if (!condition)
    fail(operation);
}

static void expect(struct result result, long value, int error, const char* operation) {
  const long expected_value = error ? (active_abi ? -1 : -error) : value;
  const long expected_rbx = active_abi ? error : RBX_SENTINEL;
  if (result.value != expected_value || result.rbx != expected_rbx || errno != EDOM) {
    fprintf(stderr, "ABI-CONTRACT result=%ld rbx=%ld expected=%ld expected_rbx=%ld\n", result.value,
            result.rbx, expected_value, expected_rbx);
    fail(operation);
  }
}

static void timeout_handler(int signal_number) {
  (void)signal_number;
  static const char message[] = "ABI-CONTRACT FAIL operation=timeout\n";
  (void)raw_linux(SYS_write, STDERR_FILENO, (long)message, sizeof(message) - 1, 0, 0, 0);
  if (child_pid > 0)
    (void)raw_linux(SYS_kill, child_pid, SIGKILL, 0, 0, 0, 0);
  _exit(124);
}

static void zero_arguments(int native, long pid, long uid) {
  expect(invoke(native, SYS_getpid, POSIX_GETPID, 0x1111, 0x2222, 0x3333, 0x4444, 0x5555, 0x6666),
         pid, 0, "zero-getpid");
  expect(invoke(native, SYS_getuid, POSIX_GETUID, 0x7777, 0x8888, 0x9999, 0xaaaa, 0xbbbb, 0xcccc),
         uid, 0, "zero-getuid");
}

static void pipe_arguments(int native) {
  int descriptors[2] = {-1, -1};
  expect(invoke(native, SYS_pipe, POSIX_PIPE, (long)descriptors, UNUSED, UNUSED + 1, UNUSED + 2,
                UNUSED + 3, UNUSED + 4),
         0, 0, "one-pipe");
  require(descriptors[0] >= 0 && descriptors[1] >= 0 && descriptors[0] != descriptors[1],
          "pipe-descriptors");
  static const char payload[] = "three-register-ABI-7b192f";
  enum { Length = sizeof(payload) - 1 };
  unsigned char received[Length + 2];
  memset(received, 0xd7, sizeof(received));
  expect(invoke(native, SYS_write, POSIX_WRITE, descriptors[1], (long)payload, Length, UNUSED,
                UNUSED + 1, UNUSED + 2),
         Length, 0, "three-write");
  expect(invoke(native, SYS_read, POSIX_READ, descriptors[0], (long)(received + 1), Length,
                UNUSED + 3, UNUSED + 4, UNUSED + 5),
         Length, 0, "three-read");
  require(
      received[0] == 0xd7 && received[Length + 1] == 0xd7 && !memcmp(received + 1, payload, Length),
      "three-contents-and-guards");
  for (unsigned i = 0; i < 2; ++i)
    expect(invoke(native, SYS_close, POSIX_CLOSE, descriptors[i], UNUSED, UNUSED + 1, UNUSED + 2,
                  UNUSED + 3, UNUSED + 4),
           0, 0, "one-close");
  expect(invoke(native, SYS_close, POSIX_CLOSE, -1, UNUSED, UNUSED + 1, UNUSED + 2, UNUSED + 3,
                UNUSED + 4),
         0, EBADF, "one-close-error");
  expect(invoke(native, 0xffff, 0xffff, 0x123, 0x456, 0x789, 0xabc, 0xdef, 0x135), 0, ENOSYS,
         "unmapped-number");
}

static void mmap_arguments(int native, int fd, long page_size) {
  // A real descriptor and second-page offset make arguments five and six observable.
  struct result result = invoke(native, SYS_mmap, POSIX_MMAP, 0, page_size, PROT_READ | PROT_WRITE,
                                MAP_PRIVATE, fd, page_size);
  expect(result, result.value, 0, "six-mmap-result");
  require(result.value && (unsigned long)result.value < (unsigned long)-4095, "six-mmap-address");
  volatile unsigned char* mapping = (void*)result.value;
  for (long i = 0; i < page_size; ++i)
    require(mapping[i] == 0x93, "six-mmap-offset-contents");
  mapping[page_size - 1] = 0x5a;
  require(mapping[page_size - 1] == 0x5a, "six-mmap-write-permission");
  expect(invoke(native, SYS_munmap, POSIX_MUNMAP, result.value, page_size, UNUSED, UNUSED + 1,
                UNUSED + 2, UNUSED + 3),
         0, 0, "two-munmap");
}

static void chmod_arguments(int fd, const char* path) {
  struct stat status;
  const long invalid_flags = 0x40000000;
  expect(invoke(1, SYS_fchmodat, POSIX_FCHMODAT, AT_FDCWD, (long)path, 0640, 0, UNUSED, UNUSED + 1),
         0, 0, "native-fchmodat");
  require(fstat(fd, &status) == 0 && (status.st_mode & 0777) == 0640, "native-fchmodat-mode");
  expect(invoke(1, SYS_fchmodat, POSIX_FCHMODAT, AT_FDCWD, (long)path, 0600, invalid_flags, UNUSED,
                UNUSED + 1),
         0, EINVAL, "native-fchmodat-rejects-flags");
  expect(invoke(0, SYS_fchmodat, POSIX_FCHMODAT, AT_FDCWD, (long)path, 0614, invalid_flags, UNUSED,
                UNUSED + 1),
         0, 0, "linux-fchmodat-ignores-argument-four");
  require(fstat(fd, &status) == 0 && (status.st_mode & 0777) == 0614, "linux-fchmodat-mode");
  puts("ABI-CONTRACT PASS phase=fchmodat");
}

static void thread_ids(void) {
  pid_t pid = fork();
  require(pid >= 0, "tid-fork");
  if (!pid) {
    child_pid = 0;
    alarm(10);
    const long own_pid = getpid();
    require(own_pid > 1, "child-pid");
    // A fresh process has local thread ID 1; Linux exposes its global process ID.
    for (unsigned i = 0; i < 32; ++i) {
      expect(invoke(0, SYS_gettid, POSIX_GETTID, UNUSED, UNUSED + 1, UNUSED + 2, UNUSED + 3,
                    UNUSED + 4, UNUSED + 5),
             own_pid, 0, "linux-global-tid");
      expect(invoke(1, SYS_gettid, POSIX_GETTID, UNUSED, UNUSED + 1, UNUSED + 2, UNUSED + 3,
                    UNUSED + 4, UNUSED + 5),
             1, 0, "native-local-tid");
    }
    _exit(0);
  }
  child_pid = pid;
  int status;
  pid_t waited;
  do {
    waited = waitpid(pid, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited == pid)
    child_pid = 0;
  require(waited == pid && WIFEXITED(status) && WEXITSTATUS(status) == 0, "tid-child-status");
  puts("ABI-CONTRACT PASS phase=thread-ids");
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  puts("ABI-CONTRACT BEGIN");
  require(signal(SIGALRM, timeout_handler) != SIG_ERR, "alarm-handler");
  alarm(30);
  const long pid = getpid(), uid = getuid(), page_size = sysconf(_SC_PAGESIZE);
  require(page_size >= 4096 && page_size <= 65536, "page-size");
  unsigned char* page = malloc((size_t)page_size);
  require(page != NULL, "fixture-buffer");
  char path[] = "/tmp/abi-contract-XXXXXX";
  int fd = mkstemp(path);
  require(fd >= 0, "fixture-open");
  for (unsigned i = 0; i < 2; ++i) {
    memset(page, i ? 0x93 : 0x26, (size_t)page_size);
    for (size_t done = 0; done < (size_t)page_size;) {
      ssize_t count = write(fd, page + done, (size_t)page_size - done);
      if (count < 0 && errno == EINTR)
        continue;
      require(count > 0, "fixture-write");
      done += (size_t)count;
    }
  }
  free(page);
  for (int native = 0; native < 2; ++native) {
    zero_arguments(native, pid, uid);
    pipe_arguments(native);
    zero_arguments(native, pid, uid);
    mmap_arguments(native, fd, page_size);
    printf("ABI-CONTRACT PASS phase=registers abi=%s\n", native ? "native" : "linux");
  }
  chmod_arguments(fd, path);
  require(close(fd) == 0 && unlink(path) == 0, "fixture-cleanup");
  thread_ids();
  alarm(0);
  puts("ABI-CONTRACT PASS END");
  return 0;
}
