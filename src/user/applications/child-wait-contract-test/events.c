#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"

int cw_events(void) {
  int failed = 0, status;
  struct cw_child child = CW_CHILD_INIT, killed = CW_CHILD_INIT;
  siginfo_t info;
  CHECK(cw_spawn(&child, -1, (uid_t)-1, 47) == 0);
  CHECK(cw_send(child.command, 'S') == 0);
  for (int i = 0; i < 3; ++i) {
    CHECK(waitid(P_PID, child.pid, &info, WSTOPPED | WNOWAIT) == 0);
    CHECK(cw_info(&info, child.pid, getuid(), CLD_STOPPED, SIGSTOP) == 0);
  }
  memset(&info, 0xa5, sizeof(info));
  CHECK(waitid(P_PID, child.pid, &info, WEXITED | WNOHANG) == 0 && !cw_empty_info(&info));
  CHECK(waitpid(child.pid, &status, WUNTRACED) == child.pid);
  CHECK(WIFSTOPPED(status) && WSTOPSIG(status) == SIGSTOP);
  CHECK(waitid(P_PID, child.pid, &info, WSTOPPED | WNOHANG) == 0 && !cw_empty_info(&info));

  CHECK(kill(child.pid, SIGCONT) == 0 && cw_receive(child.report, 'C') == 0);
  for (int i = 0; i < 3; ++i) {
    CHECK(waitid(P_PID, child.pid, &info, WCONTINUED | WNOWAIT) == 0);
    CHECK(cw_info(&info, child.pid, getuid(), CLD_CONTINUED, SIGCONT) == 0);
  }
  CHECK(waitid(P_PID, child.pid, &info, WSTOPPED | WNOHANG) == 0 && !cw_empty_info(&info));
  CHECK(waitid(P_PID, child.pid, &info, WCONTINUED) == 0);
  CHECK(cw_info(&info, child.pid, getuid(), CLD_CONTINUED, SIGCONT) == 0);
  CHECK(waitpid(child.pid, &status, WCONTINUED | WNOHANG) == 0);

  CHECK(cw_send(child.command, 'S') == 0);
  CHECK(waitid(P_PID, child.pid, &info, WSTOPPED) == 0);
  CHECK(cw_info(&info, child.pid, getuid(), CLD_STOPPED, SIGSTOP) == 0);
  CHECK(kill(child.pid, SIGCONT) == 0 && cw_receive(child.report, 'C') == 0);
  CHECK(wait4(child.pid, &status, WCONTINUED, NULL) == child.pid && WIFCONTINUED(status));
  CHECK(cw_send(child.command, 'E') == 0);
  CHECK(waitid(P_PID, child.pid, &info, WEXITED | WNOWAIT) == 0);
  memset(&info, 0xa5, sizeof(info));
  CHECK(waitid(P_PID, child.pid, &info, WSTOPPED | WNOHANG) == -1 && errno == ECHILD);
  CHECK(!cw_empty_info(&info));
  memset(&info, 0xa5, sizeof(info));
  CHECK(waitid(P_PID, child.pid, &info, WCONTINUED | WNOHANG) == -1 && errno == ECHILD);
  CHECK(!cw_empty_info(&info));
  CHECK(waitid(P_PID, child.pid, &info, WEXITED) == 0);
  CHECK(cw_info(&info, child.pid, getuid(), CLD_EXITED, 47) == 0);
  child.pid = -1;

  CHECK(cw_spawn(&killed, -1, (uid_t)-1, 0) == 0);
  CHECK(kill(killed.pid, SIGTERM) == 0);
  CHECK(waitid(P_PID, killed.pid, &info, WEXITED | WNOWAIT) == 0);
  CHECK(cw_info(&info, killed.pid, getuid(), CLD_KILLED, SIGTERM) == 0);
  CHECK(waitpid(killed.pid, &status, 0) == killed.pid);
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM && !WCOREDUMP(status));
  killed.pid = -1;
out:
  cw_cleanup(&killed);
  cw_cleanup(&child);
  return failed;
}
