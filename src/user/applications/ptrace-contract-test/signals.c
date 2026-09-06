#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

static volatile sig_atomic_t child_notice = -1;

static void child_changed(int signal, siginfo_t* info, void* context) {
  (void)context;
  int saved = errno;
  struct tc_message message = {.kind = 'C',
                               .signal = signal,
                               .code = info->si_code,
                               .pid = info->si_pid,
                               .uid = info->si_uid,
                               .result = info->si_status};
  if (child_notice >= 0) {
    ssize_t ignored = write(child_notice, &message, sizeof(message));
    (void)ignored;
  }
  errno = saved;
}

static int parent_notification(void) {
  int failed = 0, installed = 0, mask_saved = 0;
  int report[2] = {-1, -1};
  struct tc_child child = TC_CHILD_INIT;
  struct tc_message message;
  struct sigaction action = {.sa_sigaction = child_changed, .sa_flags = SA_SIGINFO | SA_RESTART};
  struct sigaction old;
  sigset_t mask, previous;
  sigemptyset(&action.sa_mask);
  sigemptyset(&mask);
  sigaddset(&mask, SIGCHLD);
  CHECK(pipe2(report, O_CLOEXEC | O_NONBLOCK) == 0);
  child_notice = report[1];
  CHECK(sigaction(SIGCHLD, &action, &old) == 0);
  installed = 1;
  CHECK(sigprocmask(SIG_UNBLOCK, &mask, &previous) == 0);
  mask_saved = 1;
  CHECK(tc_spawn(&child, 1) == 0 && tc_stop(&child, SIGUSR1) == 0);
  CHECK(tc_receive(report[0], 'C', &message) == 0);
  CHECK(message.signal == SIGCHLD && message.code == CLD_TRAPPED && message.pid == child.pid &&
        message.uid == getuid() && message.result == SIGUSR1);
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0 && tc_receive(child.report, 'A', NULL) == 0);
  CHECK(tc_finish(&child) == 0);
out:
  tc_close(&child);
  child_notice = -1;
  if (installed && sigaction(SIGCHLD, &old, NULL))
    failed = 1;
  if (mask_saved && sigprocmask(SIG_SETMASK, &previous, NULL))
    failed = 1;
  if (report[0] >= 0)
    close(report[0]);
  if (report[1] >= 0)
    close(report[1]);
  return failed;
}

static int configure(struct tc_child* child, int operation, int signal) {
  struct tc_message message;
  if (tc_send(child, operation, signal) || tc_receive(child->report, operation, &message))
    return -1;
  return message.result ? -1 : 0;
}

static int handler(struct tc_child* child, int signal, int code, pid_t pid) {
  struct tc_message message;
  if (tc_receive(child->report, 'H', &message))
    return -1;
  if (message.signal == signal && message.code == code && message.pid == pid &&
      message.uid == getuid())
    return 0;
  fprintf(stderr, "handler actual=%d/%d/%d/%u expected=%d/%d/%d/%u\n", message.signal, message.code,
          message.pid, message.uid, signal, code, pid, getuid());
  return -1;
}

static int decisions(void) {
  int failed = 0, status;
  struct tc_child child = TC_CHILD_INIT;
  siginfo_t info;
  struct user_regs_struct registers;
  CHECK(tc_spawn(&child, 1) == 0);
  CHECK(tc_stop(&child, SIGUSR1) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0);
  CHECK(tc_receive(child.report, 'A', NULL) == 0);

  CHECK(tc_stop(&child, SIGUSR1) == 0);
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == 0);
  CHECK(tc_signal_info(&info, SIGUSR1, SI_TKILL, child.pid, getuid()) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, SIGUSR1) == 0);
  CHECK(handler(&child, SIGUSR1, SI_TKILL, child.pid) == 0);
  CHECK(tc_receive(child.report, 'A', NULL) == 0);

  CHECK(tc_stop(&child, SIGUSR1) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, SIGUSR2) == 0);
  CHECK(handler(&child, SIGUSR2, SI_USER, getpid()) == 0);
  CHECK(tc_receive(child.report, 'A', NULL) == 0);

  CHECK(configure(&child, 'B', SIGUSR2) == 0 && tc_stop(&child, SIGUSR1) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, SIGUSR2) == 0);
  CHECK(tc_receive(child.report, 'A', NULL) == 0);
  CHECK(tc_send(&child, 'U', SIGUSR2) == 0);
  CHECK(tc_wait_stop(&child, SIGUSR2) == 0);
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == 0);
  CHECK(tc_signal_info(&info, SIGUSR2, SI_USER, getpid(), getuid()) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, SIGUSR2) == 0);
  CHECK(handler(&child, SIGUSR2, SI_USER, getpid()) == 0);
  CHECK(tc_receive(child.report, 'U', NULL) == 0);

  CHECK(configure(&child, 'I', SIGUSR2) == 0 && tc_stop(&child, SIGUSR2) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, SIGUSR2) == 0);
  CHECK(tc_receive(child.report, 'A', NULL) == 0);
  CHECK(configure(&child, 'F', SIGCHLD) == 0 && tc_stop(&child, SIGCHLD) == 0);
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == 0);
  CHECK(tc_signal_info(&info, SIGCHLD, SI_TKILL, child.pid, getuid()) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, SIGCHLD) == 0);
  CHECK(tc_receive(child.report, 'A', NULL) == 0);

  CHECK(tc_stop(&child, SIGUSR1) == 0 && tc_resume(child.pid, PTRACE_DETACH, 0) == 0);
  CHECK(tc_receive(child.report, 'A', NULL) == 0);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&registers) == -1 && errno == ESRCH);
  CHECK(tc_send(&child, 'S', SIGUSR1) == 0);
  CHECK(handler(&child, SIGUSR1, SI_TKILL, child.pid) == 0);
  CHECK(tc_receive(child.report, 'A', NULL) == 0);
  CHECK(wait4(child.pid, &status, WNOHANG, NULL) == 0);
  CHECK(tc_finish(&child) == 0);
out:
  tc_close(&child);
  return failed;
}

static int default_fatal(void) {
  int failed = 0, status;
  struct tc_child child = TC_CHILD_INIT;
  siginfo_t info;
  CHECK(tc_spawn(&child, 1) == 0 && tc_stop(&child, SIGTERM) == 0);
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == 0);
  CHECK(tc_signal_info(&info, SIGTERM, SI_TKILL, child.pid, getuid()) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, SIGTERM) == 0);
  CHECK(tc_wait(child.pid, &status, 10000) == 0);
  if (WIFEXITED(status) || WIFSIGNALED(status))
    child.pid = -1;
  CHECK(WIFSIGNALED(status) && WTERMSIG(status) == SIGTERM);
out:
  tc_close(&child);
  return failed;
}

static int group_stop(void) {
  int failed = 0;
  struct tc_child child = TC_CHILD_INIT;
  siginfo_t info, untouched;
  struct user_regs_struct first, second;
  CHECK(tc_spawn(&child, 1) == 0 && tc_stop(&child, SIGSTOP) == 0);
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == 0);
  CHECK(tc_signal_info(&info, SIGSTOP, SI_TKILL, child.pid, getuid()) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, SIGSTOP) == 0);
  CHECK(tc_wait_stop(&child, SIGSTOP) == 0);
  memset(&info, 0xa5, sizeof(info));
  untouched = info;
  errno = 0;
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == -1 && errno == EINVAL);
  CHECK(memcmp(&info, &untouched, sizeof(info)) == 0);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&first) == 0);
  CHECK(kill(child.pid, SIGCONT) == 0);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&second) == 0);
  CHECK(memcmp(&first, &second, sizeof(first)) == 0);
  errno = 0;
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == -1 && errno == EINVAL);
  for (unsigned i = 0; i < 2; ++i) {
    CHECK(waitid(P_PID, child.pid, &info, WCONTINUED | WNOWAIT) == 0);
    CHECK(info.si_pid == child.pid && info.si_code == CLD_CONTINUED && info.si_status == SIGCONT);
  }
  CHECK(waitid(P_PID, child.pid, &info, WCONTINUED) == 0 && info.si_pid == child.pid);
  CHECK(waitid(P_PID, child.pid, &info, WCONTINUED | WNOHANG) == 0 && !info.si_pid);
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0);
  CHECK(tc_wait_stop(&child, SIGCONT) == 0);
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == 0);
  CHECK(tc_signal_info(&info, SIGCONT, SI_USER, getpid(), getuid()) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0 && tc_receive(child.report, 'A', NULL) == 0);
  CHECK(tc_finish(&child) == 0);
out:
  tc_close(&child);
  return failed;
}

static int group_cont_alone(void) {
  int failed = 0;
  struct tc_child child = TC_CHILD_INIT;
  siginfo_t info;
  CHECK(tc_spawn(&child, 1) == 0 && tc_stop(&child, SIGSTOP) == 0);
  CHECK(tc_resume(child.pid, PTRACE_CONT, SIGSTOP) == 0 && tc_wait_stop(&child, SIGSTOP) == 0);
  errno = 0;
  CHECK(ptrace(PTRACE_GETSIGINFO, child.pid, (void*)0, (void*)&info) == -1 && errno == EINVAL);
  // Ordinary CONT resumes a group stop; retaining it would require deferred LISTEN.
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0 && tc_receive(child.report, 'A', NULL) == 0);
  CHECK(tc_finish(&child) == 0);
out:
  tc_close(&child);
  return failed;
}

int tc_signals(void) {
  return parent_notification() || decisions() || default_fatal() || group_stop() ||
         group_cont_alone();
}
