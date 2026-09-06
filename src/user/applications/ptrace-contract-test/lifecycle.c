#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

static int kill_stopped(void) {
  int failed = 0, status;
  struct tc_child child = TC_CHILD_INIT;
  struct user_regs_struct registers;
  siginfo_t info;
  CHECK(tc_spawn(&child, 1) == 0 && tc_stop(&child, SIGUSR1) == 0);
  pid_t pid = child.pid;
  CHECK(kill(pid, SIGKILL) == 0);
  CHECK(tc_wait(pid, &status, 10000) == 0);
  if (WIFEXITED(status) || WIFSIGNALED(status))
    child.pid = -1;
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  errno = 0;
  CHECK(wait4(pid, NULL, WNOHANG, NULL) == -1 && errno == ECHILD);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGS, pid, (void*)0, (void*)&registers) == -1 && errno == ESRCH);
  errno = 0;
  CHECK(ptrace(PTRACE_GETSIGINFO, pid, (void*)0, (void*)&info) == -1 && errno == ESRCH);
out:
  tc_close(&child);
  return failed;
}

static int tracee_exec(int raw_entry) {
  int failed = 0, status;
  struct tc_child child = TC_CHILD_INIT;
  struct user_regs_struct old, entry, repeated;
  struct tc_message message;
  struct tc_entry_record actual;
  siginfo_t info;
  CHECK(tc_spawn(&child, 1) == 0 && tc_stop(&child, SIGUSR1) == 0);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&old) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0 && tc_receive(child.report, 'A', NULL) == 0);
  CHECK(tc_send(&child, raw_entry ? 'J' : 'X', 0) == 0);
  CHECK(tc_wait_stop(&child, SIGTRAP) == 0);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&entry) == 0);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&repeated) == 0);
  CHECK(memcmp(&entry, &repeated, sizeof(entry)) == 0);
  CHECK(entry.rip && entry.rsp && entry.rip != old.rip);
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == 0);
  CHECK(info.si_signo == SIGTRAP && !info.si_errno);
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0);
  if (raw_entry) {
    CHECK(tc_read(child.report, &actual, sizeof(actual)) == 0);
    CHECK(actual.magic == TC_ENTRY_MAGIC_VALUE && actual.fs_result == 0);
    unsigned long stopped[27], resumed[27];
    memcpy(stopped, &entry, sizeof(stopped));
    memcpy(resumed, &actual.registers, sizeof(resumed));
    for (size_t i = 0; i < 27; ++i) {
      // orig_rax is ABI provenance; GS-base truth belongs to the core fixture.
      if (i == TC_ORIG_RAX / 8 || i == TC_REG_GS_BASE / 8)
        continue;
      if (stopped[i] != resumed[i])
        fprintf(stderr, "exec entry register[%zu] stop=%#lx resumed=%#lx\n", i, stopped[i],
                resumed[i]);
      CHECK(stopped[i] == resumed[i]);
    }
    CHECK(entry.orig_rax == SYS_execve);
  } else {
    CHECK(tc_receive(child.report, 'X', &message) == 0);
    CHECK(message.pid == child.pid && message.value == TC_IMAGE_TLS);
  }
  CHECK(tc_wait(child.pid, &status, 10000) == 0);
  if (WIFEXITED(status) || WIFSIGNALED(status))
    child.pid = -1;
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 0);
out:
  tc_close(&child);
  return failed;
}

int tc_exec_tracer(pid_t pid, int command, int report) {
  int failed = 0;
  struct tc_child child = {pid, command, report};
  struct user_regs_struct registers;
  siginfo_t info;
  alarm(20);
  CHECK(ptrace(PTRACE_GETREGS, pid, (void*)0, (void*)&registers) == 0);
  CHECK(registers.rip && registers.rsp);
  CHECK(ptrace(PTRACE_GETSIGINFO, pid, (void*)0, (void*)&info) == 0);
  CHECK(tc_signal_info(&info, SIGUSR1, SI_TKILL, pid, getuid()) == 0);
  CHECK(tc_resume(pid, PTRACE_CONT, 0) == 0 && tc_receive(report, 'A', NULL) == 0);
  CHECK(tc_finish(&child) == 0);
out:
  tc_close(&child);
  return failed;
}

static int tracer_exec(void) {
  int failed = 0;
  struct tc_child child = TC_CHILD_INIT;
  char pid[24], command[24], report[24];
  CHECK(tc_spawn(&child, 1) == 0 && tc_stop(&child, SIGUSR1) == 0);
  CHECK(fcntl(child.command, F_SETFD, 0) == 0 && fcntl(child.report, F_SETFD, 0) == 0);
  snprintf(pid, sizeof(pid), "%d", child.pid);
  snprintf(command, sizeof(command), "%d", child.command);
  snprintf(report, sizeof(report), "%d", child.report);
  execl(TC_EXECUTABLE, TC_EXECUTABLE, "--exec-tracer", pid, command, report, (char*)NULL);
  CHECK(0);
out:
  tc_close(&child);
  return failed;
}

int tc_lifecycle(void) {
  return kill_stopped() || tracee_exec(1) || tracee_exec(0) || tracer_exec();
}
