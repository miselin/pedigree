#define _GNU_SOURCE
#include <elf.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/uio.h>

static int peek(pid_t pid, siginfo_t* info) {
  int64_t now = tc_now(), end = now + INT64_C(10000000000);
  while (now >= 0 && now < end) {
    if (waitid(P_PID, pid, info, WSTOPPED | WNOWAIT | WNOHANG))
      return -1;
    if (info->si_pid)
      return 0;
    sched_yield();
    now = tc_now();
  }
  if (now >= 0)
    errno = ETIMEDOUT;
  return -1;
}

int tc_inspect(void) {
  int failed = 0;
  struct tc_child child = TC_CHILD_INIT;
  siginfo_t event;
  struct {
    siginfo_t info;
    uint64_t tail[2];
  } first, second;
  struct user_regs_struct registers, saved;
  CHECK(tc_spawn(&child, 1) == 0 && tc_send(&child, 'S', SIGUSR1) == 0);
  for (unsigned i = 0; i < 3; ++i) {
    memset(&event, 0xa5, sizeof(event));
    CHECK(peek(child.pid, &event) == 0);
    CHECK(event.si_signo == SIGCHLD && !event.si_errno && event.si_code == CLD_TRAPPED &&
          event.si_pid == child.pid && event.si_uid == getuid() && event.si_status == SIGUSR1);
  }
  CHECK(tc_wait_stop(&child, SIGUSR1) == 0);
  memset(&event, 0xa5, sizeof(event));
  CHECK(waitid(P_PID, child.pid, &event, WSTOPPED | WNOHANG) == 0);
  CHECK(!event.si_signo && !event.si_errno && !event.si_code && !event.si_pid && !event.si_uid &&
        !event.si_status);

  memset(&first, 0xa5, sizeof(first));
  memset(&second, 0x5a, sizeof(second));
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&first.info) == 0);
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&second.info) == 0);
  CHECK(tc_signal_info(&first.info, SIGUSR1, SI_TKILL, child.pid, getuid()) == 0);
  CHECK(memcmp(&first.info, &second.info, sizeof(siginfo_t)) == 0);
  CHECK(first.tail[0] == UINT64_C(0xa5a5a5a5a5a5a5a5) &&
        first.tail[1] == UINT64_C(0xa5a5a5a5a5a5a5a5));
  CHECK(second.tail[0] == UINT64_C(0x5a5a5a5a5a5a5a5a) &&
        second.tail[1] == UINT64_C(0x5a5a5a5a5a5a5a5a));
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&registers) == 0);
  saved = registers;
  const size_t lengths[] = {0, 8, TC_REG_SIZE, TC_REG_SIZE + 32, SIZE_MAX & ~(size_t)7};
  for (size_t i = 0; i < sizeof(lengths) / sizeof(lengths[0]); ++i) {
    unsigned char data[TC_REG_SIZE + 32];
    memset(data, 0xa5, sizeof(data));
    struct iovec vector = {.iov_base = lengths[i] ? data + 1 : NULL, .iov_len = lengths[i]};
    void* original = vector.iov_base;
    CHECK(ptrace(PTRACE_GETREGSET, child.pid, (void*)(uintptr_t)NT_PRSTATUS, (void*)&vector) == 0);
    size_t copied = lengths[i] > TC_REG_SIZE ? TC_REG_SIZE : lengths[i];
    CHECK(vector.iov_base == original && vector.iov_len == copied);
    CHECK(memcmp(data + 1, &registers, copied) == 0 && data[0] == 0xa5);
    for (size_t j = copied + 1; j < sizeof(data); ++j)
      CHECK(data[j] == 0xa5);
  }
  struct iovec vector = {.iov_base = &registers, .iov_len = TC_REG_SIZE};
  CHECK(ptrace(PTRACE_GETREGSET, child.pid, (void*)(uintptr_t)(UINT64_C(0x100000000) | NT_PRSTATUS),
               (void*)&vector) == 0);
  CHECK(memcmp(&registers, &saved, TC_REG_SIZE) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0 && tc_receive(child.report, 'A', NULL) == 0);
  CHECK(memcmp(&registers, &saved, TC_REG_SIZE) == 0);
  CHECK(tc_finish(&child) == 0);
out:
  tc_close(&child);
  return failed;
}
