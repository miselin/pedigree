#define _GNU_SOURCE
#include <elf.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/uio.h>

static int admission(void) {
  int failed = 0;
  struct tc_child child = TC_CHILD_INIT;
  struct tc_message message;
  struct user_regs_struct registers;
  CHECK(tc_spawn(&child, 0) == 0);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)0) == -1 && errno == ESRCH);
  CHECK(tc_send(&child, 'T', 0) == 0 && tc_receive(child.report, 'T', &message) == 0);
  CHECK(message.result == 0);
  CHECK(tc_send(&child, 'T', 0) == 0 && tc_receive(child.report, 'T', &message) == 0);
  CHECK(message.result == -1 && message.error == EPERM);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)0) == -1 && errno == ESRCH);
  CHECK(tc_stop(&child, SIGUSR1) == 0);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)UINTPTR_MAX, (void*)&registers) == 0);
  CHECK(tc_resume(child.pid, PTRACE_DETACH, 0) == 0 && tc_receive(child.report, 'A', NULL) == 0);
  CHECK(tc_finish(&child) == 0);
  tc_close(&child);

  CHECK(tc_spawn(&child, 0) == 0);
  CHECK(tc_send(&child, 'M', 0) == 0 && tc_receive(child.report, 'M', &message) == 0);
  CHECK(message.result == -1 && message.error == EOPNOTSUPP);
  CHECK(tc_finish(&child) == 0);
  tc_close(&child);

  CHECK(tc_spawn(&child, 0) == 0);
  CHECK(tc_send(&child, 'D', 0) == 0 && tc_receive(child.report, 'D', &message) == 0);
  CHECK(message.result == 0);
  CHECK(tc_send(&child, 'T', 0) == 0 && tc_receive(child.report, 'T', &message) == 0);
  CHECK(message.result == -1 && message.error == EPERM);
  CHECK(tc_finish(&child) == 0);
  tc_close(&child);

  CHECK(tc_spawn(&child, 1) == 0);
  CHECK(tc_send(&child, 'D', 0) == 0 && tc_receive(child.report, 'D', &message) == 0);
  CHECK(message.result == 0 && tc_stop(&child, SIGUSR1) == 0);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&registers) == -1 && errno == EPERM);
  errno = 0;
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == -1 && errno == EPERM);
out:
  tc_close(&child);
  return failed;
}

static int copyout(void) {
  int failed = 0;
  struct tc_child child = TC_CHILD_INIT;
  void* inaccessible = MAP_FAILED;
  struct iovec* readonly = MAP_FAILED;
  struct user_regs_struct original, after;
  unsigned char output[TC_REG_SIZE + 32];
  struct iovec vector;
  CHECK(tc_spawn(&child, 1) == 0 && tc_stop(&child, SIGUSR1) == 0);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&original) == 0);
  inaccessible = mmap(NULL, tc_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  readonly = mmap(NULL, tc_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(inaccessible != MAP_FAILED && readonly != MAP_FAILED);
  void* bad[] = {NULL, inaccessible};
  for (size_t i = 0; i < sizeof(bad) / sizeof(bad[0]); ++i) {
    errno = 0;
    CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, bad[i]) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, bad[i]) == -1 && errno == EFAULT);
    errno = 0;
    CHECK(ptrace(PTRACE_GETREGSET, child.pid, (void*)(uintptr_t)NT_PRSTATUS, bad[i]) == -1 &&
          errno == EFAULT);
  }
  memset(output, 0xa5, sizeof(output));
  vector = (struct iovec){.iov_base = output, .iov_len = 7};
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGSET, child.pid, (void*)(uintptr_t)NT_PRSTATUS, (void*)&vector) == -1 &&
        errno == EINVAL);
  CHECK(vector.iov_base == output && vector.iov_len == 7);
  vector.iov_len = sizeof(output);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGSET, child.pid, (void*)0, (void*)&vector) == -1 && errno == EINVAL);
  CHECK(vector.iov_base == output && vector.iov_len == sizeof(output));
  for (size_t i = 0; i < sizeof(output); ++i)
    CHECK(output[i] == 0xa5);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGSET, child.pid, (void*)0, inaccessible) == -1 && errno == EFAULT);

  vector = (struct iovec){.iov_base = inaccessible, .iov_len = sizeof(output)};
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGSET, child.pid, (void*)(uintptr_t)NT_PRSTATUS, (void*)&vector) == -1 &&
        errno == EFAULT);
  CHECK(vector.iov_base == inaccessible && vector.iov_len == sizeof(output));
  vector.iov_len = 0;
  CHECK(ptrace(PTRACE_GETREGSET, child.pid, (void*)(uintptr_t)NT_PRSTATUS, (void*)&vector) == 0);
  CHECK(vector.iov_base == inaccessible && !vector.iov_len);

  *readonly = (struct iovec){.iov_base = output, .iov_len = sizeof(output)};
  CHECK(mprotect(readonly, tc_page, PROT_READ) == 0);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGSET, child.pid, (void*)(uintptr_t)NT_PRSTATUS, (void*)readonly) == -1 &&
        errno == EFAULT);
  CHECK(memcmp(output, &original, TC_REG_SIZE) == 0);
  for (size_t i = TC_REG_SIZE; i < sizeof(output); ++i)
    CHECK(output[i] == 0xa5);
  CHECK(readonly->iov_base == output && readonly->iov_len == sizeof(output));
  CHECK(mprotect(readonly, tc_page, PROT_READ | PROT_WRITE) == 0);
  *readonly = (struct iovec){.iov_base = NULL, .iov_len = 0};
  CHECK(mprotect(readonly, tc_page, PROT_READ) == 0);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGSET, child.pid, (void*)(uintptr_t)NT_PRSTATUS, (void*)readonly) == -1 &&
        errno == EFAULT);

  const uintptr_t invalid_signals[] = {65, UINTPTR_MAX, UINT64_C(0x10000000a)};
  for (size_t i = 0; i < sizeof(invalid_signals) / sizeof(invalid_signals[0]); ++i) {
    errno = 0;
    CHECK(ptrace(PTRACE_CONT, child.pid, (void*)0, (void*)invalid_signals[i]) == -1 &&
          errno == EIO);
    errno = 0;
    CHECK(ptrace(PTRACE_DETACH, child.pid, (void*)0, (void*)invalid_signals[i]) == -1 &&
          errno == EIO);
  }
  errno = 0;
  CHECK(ptrace(0x7fffffff, child.pid, (void*)0, (void*)0) == -1 && errno == EIO);
  errno = 0;
  CHECK(ptrace(PTRACE_GETFPREGS, child.pid, (void*)0, (void*)output) == -1 && errno == EOPNOTSUPP);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGS, (pid_t)0, (void*)0, (void*)0) == -1 && errno == ESRCH);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGS, (pid_t)-1, (void*)0, (void*)0) == -1 && errno == ESRCH);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&after) == 0);
  CHECK(memcmp(&original, &after, sizeof(original)) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0 && tc_receive(child.report, 'A', NULL) == 0);
  CHECK(tc_finish(&child) == 0);
out:
  if (readonly != MAP_FAILED)
    munmap(readonly, tc_page);
  if (inaccessible != MAP_FAILED)
    munmap(inaccessible, tc_page);
  tc_close(&child);
  return failed;
}

static int replacement_quota(void) {
  int failed = 0;
  struct tc_child child = TC_CHILD_INIT;
  struct tc_message message;
  struct user_regs_struct before, after;
  siginfo_t info;
  const int realtime = SIGRTMIN;
  CHECK(tc_spawn(&child, 1) == 0);
  CHECK(tc_send(&child, 'B', realtime) == 0 && tc_receive(child.report, 'B', &message) == 0);
  CHECK(message.result == 0);
  for (int i = 0; i < 16; ++i) {
    union sigval value = {.sival_int = i + 100};
    CHECK(sigqueue(child.pid, realtime, value) == 0);
  }
  union sigval excess = {.sival_int = 999};
  errno = 0;
  CHECK(sigqueue(child.pid, realtime, excess) == -1 && errno == EAGAIN);
  CHECK(tc_stop(&child, SIGUSR1) == 0);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&before) == 0);
  errno = 0;
  CHECK(tc_resume(child.pid, PTRACE_CONT, realtime) == -1 && errno == EAGAIN);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&after) == 0);
  CHECK(memcmp(&before, &after, sizeof(before)) == 0);
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == 0);
  CHECK(tc_signal_info(&info, SIGUSR1, SI_TKILL, child.pid, getuid()) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0 && tc_receive(child.report, 'A', NULL) == 0);
  CHECK(tc_send(&child, 'Q', realtime) == 0 && tc_receive(child.report, 'Q', &message) == 0);
  CHECK(message.result == 16);
  CHECK(tc_finish(&child) == 0);
out:
  tc_close(&child);
  return failed;
}

int tc_errors(void) {
  return admission() || copyout() || replacement_quota();
}
