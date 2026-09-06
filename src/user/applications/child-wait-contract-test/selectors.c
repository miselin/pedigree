#define _GNU_SOURCE
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

int cw_selectors(void) {
  int failed = 0;
  struct cw_child leader = CW_CHILD_INIT, member = CW_CHILD_INIT, own = CW_CHILD_INIT;
  struct cw_child any = CW_CHILD_INIT, numeric = CW_CHILD_INIT;
  siginfo_t info;
  const uid_t uid = getuid();
  CHECK(setpgid(0, 0) == 0 && getpgrp() == getpid());
  CHECK(cw_spawn(&leader, 0, (uid_t)-1, 21) == 0);
  CHECK(cw_spawn(&member, leader.pid, (uid_t)-1, 22) == 0);
  CHECK(cw_spawn(&own, -1, (uid_t)-1, 23) == 0);
  CHECK(getpgid(leader.pid) == leader.pid && getpgid(member.pid) == leader.pid);
  CHECK(getpgid(own.pid) == getpgrp());
  const pid_t other_group = leader.pid;

  memset(&info, 0xa5, sizeof(info));
  CHECK(waitid(P_PID, own.pid, &info, WEXITED | WNOHANG) == 0 && !cw_empty_info(&info));
  CHECK(cw_send(leader.command, 'E') == 0);
  CHECK(waitid(P_PID, leader.pid, &info, WEXITED | WNOWAIT) == 0);
  CHECK(cw_info(&info, leader.pid, uid, CLD_EXITED, 21) == 0);
  CHECK(waitid(P_PGID, leader.pid, &info, WEXITED) == 0);
  CHECK(cw_info(&info, leader.pid, uid, CLD_EXITED, 21) == 0);
  leader.pid = -1;
  CHECK(cw_send(member.command, 'E') == 0);
  CHECK(waitid(P_PGID, other_group, &info, WEXITED) == 0);
  CHECK(cw_info(&info, member.pid, uid, CLD_EXITED, 22) == 0);
  member.pid = -1;
  CHECK(cw_send(own.command, 'E') == 0);
  CHECK(waitid(P_PGID, 0, &info, WEXITED) == 0);
  CHECK(cw_info(&info, own.pid, uid, CLD_EXITED, 23) == 0);
  own.pid = -1;

  CHECK(cw_spawn(&any, -1, (uid_t)-1, 0x1ab) == 0 && cw_send(any.command, 'E') == 0);
  CHECK(waitid(P_ALL, (id_t)UINT_MAX, &info, WEXITED) == 0);
  CHECK(cw_info(&info, any.pid, uid, CLD_EXITED, 0xab) == 0);
  any.pid = -1;

  if (geteuid() == 0) {
    CHECK(cw_spawn(&numeric, -1, 17001, 31) == 0);
    CHECK(cw_send(numeric.command, 'E') == 0);
    CHECK(waitid(P_PID, numeric.pid, &info, WEXITED) == 0);
    CHECK(cw_info(&info, numeric.pid, 17001, CLD_EXITED, 31) == 0);
    numeric.pid = -1;
  } else {
    puts("CHILD-WAIT-CONTRACT: SKIP distinct numeric real UID (requires root)");
  }
out:
  cw_cleanup(&numeric);
  cw_cleanup(&any);
  cw_cleanup(&own);
  cw_cleanup(&member);
  cw_cleanup(&leader);
  return failed;
}
