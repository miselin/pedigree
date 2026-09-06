#define _GNU_SOURCE
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/syscall.h>

int cw_errors(void) {
  int failed = 0, status = 0x5a5a;
  siginfo_t info;
  struct rusage usage;
  struct cw_child child = CW_CHILD_INIT;
  void* bad = mmap(NULL, cw_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  CHECK(cw_spawn(&child, -1, (uid_t)-1, 9) == 0);
  const struct {
    int type;
    id_t id;
    int options;
  } invalid[] = {{99, 0, WEXITED},
                 {P_PID, 0, WEXITED},
                 {P_PID, (id_t)-1, WEXITED},
                 {P_PGID, (id_t)-1, WEXITED},
                 {P_ALL, 0, 0},
                 {P_ALL, 0, WNOHANG},
                 {P_ALL, 0, WEXITED | 0x1000}};
  for (size_t i = 0; i < sizeof(invalid) / sizeof(invalid[0]); ++i) {
    memset(&info, 0xa5, sizeof(info));
    errno = 0;
    CHECK(cw_raw_waitid(invalid[i].type, invalid[i].id, &info, invalid[i].options, NULL) == -1);
    CHECK(errno == EINVAL && !cw_empty_info(&info));
  }
  errno = 0;
  CHECK(cw_raw_waitid(99, 0, bad, WEXITED, NULL) == -1 && errno == EFAULT);
  CHECK(cw_raw_waitid(3, 0, &info, WEXITED, NULL) == -1 && errno == EOPNOTSUPP);
  CHECK(cw_raw_waitid(3, (id_t)-1, &info, WEXITED, NULL) == -1 && errno == EINVAL);
  CHECK(cw_raw_waitid(P_ALL, 0, &info, WEXITED | 0x80000000u, NULL) == -1 && errno == EOPNOTSUPP);
  CHECK(syscall(SYS_wait4, child.pid, &status, WNOWAIT, NULL) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_wait4, child.pid, &status, WEXITED, NULL) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_wait4, INT_MIN, &status, 0, NULL) == -1 && errno == ESRCH);
  CHECK(status == 0x5a5a);
  memset(&usage, 0xa5, sizeof(usage));
  memset(&info, 0xa5, sizeof(info));
  CHECK(cw_raw_waitid(P_PID, child.pid, &info, WEXITED | WNOHANG, &usage) == 0);
  CHECK(!cw_empty_info(&info) && cw_bytes(&usage, sizeof(usage), 0xa5) == 0);
  CHECK(cw_raw_waitid(P_PID, child.pid, NULL, WEXITED | WNOHANG, bad) == 0);
  CHECK(syscall(SYS_wait4, child.pid, bad, WNOHANG, bad) == 0);

  CHECK(cw_send(child.command, 'E') == 0);
  CHECK(waitid(P_PID, child.pid, &info, WEXITED) == 0);
  pid_t retired = child.pid;
  child.pid = -1;
  memset(&info, 0xa5, sizeof(info));
  CHECK(waitid(P_PID, retired, &info, WEXITED) == -1 && errno == ECHILD);
  CHECK(!cw_empty_info(&info));
  CHECK(cw_raw_waitid(P_PID, retired, bad, WEXITED, NULL) == -1 && errno == EFAULT);
  CHECK(cw_raw_waitid(P_ALL, 0, NULL, WEXITED | WNOHANG, bad) == -1 && errno == ECHILD);
out:
  cw_cleanup(&child);
  if (bad != MAP_FAILED)
    munmap(bad, cw_page);
  return failed;
}
