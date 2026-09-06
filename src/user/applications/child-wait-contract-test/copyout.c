#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/syscall.h>

static int exited(struct cw_child* child, int status, struct rusage* usage, siginfo_t* info) {
  memset(usage, 0xa5, sizeof(*usage));
  return cw_spawn(child, -1, (uid_t)-1, status) || cw_send(child->command, 'B') ||
                 cw_receive(child->report, 'B') || cw_send(child->command, 'E') ||
                 cw_raw_waitid(P_PID, child->pid, info, WEXITED | WNOWAIT, usage)
             ? -1
             : 0;
}

static int fault_case(int use_wait4, int first_output, int no_wait) {
  int failed = 0, status = 0x7777;
  struct cw_child child = CW_CHILD_INIT;
  struct rusage usage, expected;
  siginfo_t info;
  void* bad = mmap(NULL, cw_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  CHECK(exited(&child, 67, &expected, &info) == 0 && !cw_usage(&expected));
  memset(&info, 0xa5, sizeof(info));
  memset(&usage, 0xa5, sizeof(usage));
  errno = 0;
  long result;
  if (use_wait4) {
    result =
        syscall(SYS_wait4, child.pid, first_output ? bad : &status, 0, first_output ? &usage : bad);
  } else {
    result = cw_raw_waitid(P_PID, child.pid, first_output ? &info : bad,
                           WEXITED | (no_wait ? WNOWAIT : 0), first_output ? bad : &usage);
  }
  CHECK(result == -1 && errno == EFAULT);
  if (use_wait4) {
    if (first_output)
      CHECK(cw_bytes(&usage, sizeof(usage), 0xa5) == 0);
    else
      CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 67);
  } else if (first_output) {
    CHECK(cw_bytes(&info, sizeof(info), 0xa5) == 0);
  } else {
    CHECK(memcmp(&usage, &expected, CW_RUSAGE_BYTES) == 0 && !cw_usage(&usage));
  }
  if (no_wait) {
    CHECK(waitid(P_PID, child.pid, &info, WEXITED | WNOWAIT) == 0);
    CHECK(cw_info(&info, child.pid, getuid(), CLD_EXITED, 67) == 0);
    CHECK(wait4(child.pid, &status, 0, NULL) == child.pid && WEXITSTATUS(status) == 67);
  } else {
    CHECK(waitid(P_PID, child.pid, &info, WEXITED | WNOHANG) == -1 && errno == ECHILD);
    CHECK(!cw_empty_info(&info));
  }
  child.pid = -1;
out:
  cw_cleanup(&child);
  if (bad != MAP_FAILED)
    munmap(bad, cw_page);
  return failed;
}

static int optional_and_overlap(void) {
  int failed = 0;
  struct cw_child child = CW_CHILD_INIT;
  struct rusage usage, expected_usage;
  siginfo_t info, expected_info;
  union {
    struct rusage usage;
    siginfo_t info;
    int status;
  } overlap, expected;
  CHECK(exited(&child, 73, &expected_usage, &info) == 0);
  CHECK(!cw_usage(&expected_usage));
  CHECK(cw_info(&info, child.pid, getuid(), CLD_EXITED, 73) == 0);
  memset(&usage, 0xa5, sizeof(usage));
  CHECK(cw_raw_waitid(P_PID, child.pid, NULL, WEXITED | WNOWAIT, &usage) == 0);
  CHECK(memcmp(&usage, &expected_usage, CW_RUSAGE_BYTES) == 0 && !cw_usage(&usage));

  memset(&info, 0xa5, sizeof(info));
  expected_info = info;
  expected_info.si_signo = SIGCHLD;
  expected_info.si_errno = 0;
  expected_info.si_code = CLD_EXITED;
  expected_info.si_pid = child.pid;
  expected_info.si_uid = getuid();
  expected_info.si_status = 73;
  CHECK(waitid(P_PID, child.pid, &info, WEXITED | WNOWAIT) == 0);
  CHECK(memcmp(&info, &expected_info, sizeof(info)) == 0);

  memset(&overlap, 0xa5, sizeof(overlap));
  memset(&expected, 0xa5, sizeof(expected));
  memcpy(&expected, &expected_usage, CW_RUSAGE_BYTES);
  expected.info.si_signo = SIGCHLD;
  expected.info.si_errno = 0;
  expected.info.si_code = CLD_EXITED;
  expected.info.si_pid = child.pid;
  expected.info.si_uid = getuid();
  expected.info.si_status = 73;
  CHECK(cw_raw_waitid(P_PID, child.pid, &overlap.info, WEXITED | WNOWAIT, &overlap.usage) == 0);
  CHECK(memcmp(&overlap, &expected, CW_RUSAGE_BYTES) == 0);
  CHECK(cw_bytes((const unsigned char*)&overlap + CW_RUSAGE_BYTES,
                 sizeof(overlap) - CW_RUSAGE_BYTES, 0xa5) == 0);

  /* wait4's later rusage output overwrites an aliased status output. */
  CHECK(syscall(SYS_wait4, child.pid, &overlap.status, 0, &overlap.usage) == child.pid);
  CHECK(memcmp(&overlap.usage, &expected_usage, CW_RUSAGE_BYTES) == 0 && !cw_usage(&overlap.usage));
  child.pid = -1;
  cw_cleanup(&child);
  CHECK(exited(&child, 74, &usage, &info) == 0);
  CHECK(cw_raw_waitid(P_PID, child.pid, NULL, WEXITED, NULL) == 0);
  CHECK(waitpid(child.pid, NULL, WNOHANG) == -1 && errno == ECHILD);
  child.pid = -1;
out:
  cw_cleanup(&child);
  return failed;
}

int cw_copyout(void) {
  for (int no_wait = 0; no_wait < 2; ++no_wait)
    for (int first = 0; first < 2; ++first)
      if (fault_case(0, first, no_wait))
        return 1;
  for (int first = 0; first < 2; ++first)
    if (fault_case(1, first, 0))
      return 1;
  return optional_and_overlap();
}
