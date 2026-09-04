/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));

static volatile sig_atomic_t signalHandled = 0;

static void handleSignal(int signalNumber) {
  if (signalNumber == SIGUSR1)
    signalHandled = 1;
}

static void status(const char* message) {
  puts(message);
  fflush(stdout);
}

static void test_proc_self_fd(void) {
  status("Testing /proc/self/fd readlink...");

  int fd = open("/dev/null", O_RDONLY);
  if (fd < 0)
    fail();

  char procname[64];
  snprintf(procname, sizeof(procname), "/proc/self/fd/%d", fd);

  char target[PATH_MAX];
  ssize_t length = readlink(procname, target, sizeof(target));
  if (length <= 0 || (size_t)length >= sizeof(target))
    fail();
  target[length] = '\0';

  struct stat pathStat;
  struct stat descriptorStat;
  if (stat(target, &pathStat) || fstat(fd, &descriptorStat) ||
      pathStat.st_dev != descriptorStat.st_dev || pathStat.st_ino != descriptorStat.st_ino)
    fail();

  char truncated[2];
  if (readlink(procname, truncated, sizeof(truncated)) != (ssize_t)sizeof(truncated) ||
      memcmp(target, truncated, sizeof(truncated)))
    fail();

  close(fd);
  errno = 0;
  if (readlink(procname, target, sizeof(target)) != -1 || errno != ENOENT)
    fail();

  if (isatty(STDIN_FILENO)) {
    char tty[PATH_MAX];
    if (ttyname_r(STDIN_FILENO, tty, sizeof(tty)))
      fail();
  }

  status("OK");
}

static void test_vfork(void) {
  status("Testing vfork syscall compatibility...");

  pid_t child = vfork();
  if (child < 0)
    fail();
  if (!child)
    _exit(0);

  int statusCode = 0;
  if (waitpid(child, &statusCode, 0) != child || !WIFEXITED(statusCode) || WEXITSTATUS(statusCode))
    fail();

  status("OK");
}

static void test_signal_return(void) {
  status("Testing signal handler return...");

  struct sigaction action = {0};
  action.sa_handler = handleSignal;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, 0) || kill(getpid(), SIGUSR1) ||
      !signalHandled || signal(SIGUSR1, SIG_DFL) == SIG_ERR)
    fail();

  status("OK");
}

static void test_default_signal_termination(void) {
  status("Testing default signal termination status...");

  pid_t child = fork();
  if (child < 0)
    fail();
  if (!child) {
    if (signal(SIGUSR1, SIG_DFL) == SIG_ERR || kill(getpid(), SIGUSR1))
      _exit(126);
    _exit(127);
  }

  int statusCode = 0;
  if (waitpid(child, &statusCode, 0) != child || !WIFSIGNALED(statusCode) ||
      WTERMSIG(statusCode) != SIGUSR1)
    fail();

  status("OK");
}

static void test_thread_signal_syscalls(void) {
  status("Testing thread-directed signal syscalls...");

  struct sigaction action = {0};
  struct sigaction previousAction = {0};
  action.sa_handler = handleSignal;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, &previousAction))
    fail();

  long tid = syscall(SYS_gettid);
  if (tid <= 0 || syscall(SYS_tkill, tid, 0) || syscall(SYS_tgkill, getpid(), tid, 0))
    fail();

  signalHandled = 0;
  if (syscall(SYS_tkill, tid, SIGUSR1))
    fail();
  for (size_t attempt = 0; attempt < 1000 && !signalHandled; ++attempt)
    sched_yield();
  if (!signalHandled)
    fail();

  signalHandled = 0;
  if (syscall(SYS_tgkill, getpid(), tid, SIGUSR1))
    fail();
  for (size_t attempt = 0; attempt < 1000 && !signalHandled; ++attempt)
    sched_yield();
  if (!signalHandled)
    fail();

  signalHandled = 0;
  if (raise(SIGUSR1) || !signalHandled)
    fail();

  errno = 0;
  if (syscall(SYS_tkill, 0, 0) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_tgkill, 0, tid, 0) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_tgkill, getpid(), 0, 0) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_tkill, INT_MAX, 32) != -1 || errno != ESRCH)
    fail();
  errno = 0;
  if (syscall(SYS_tkill, tid, 32) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_tgkill, getpid(), INT_MAX, 32) != -1 || errno != ESRCH)
    fail();
  errno = 0;
  if (syscall(SYS_tgkill, INT_MAX, tid, 32) != -1 || errno != ESRCH)
    fail();

  if (sigaction(SIGUSR1, &previousAction, 0))
    fail();
  status("OK");
}

static void test_sigsuspend(void) {
  status("Testing sigsuspend temporary mask...");

  struct sigaction action = {0};
  struct sigaction previousAction = {0};
  action.sa_handler = handleSignal;
  if (sigemptyset(&action.sa_mask) || sigaction(SIGUSR1, &action, &previousAction))
    fail();

  sigset_t blocked;
  sigset_t original;
  sigset_t temporary;
  if (sigemptyset(&blocked) || sigaddset(&blocked, SIGUSR1) ||
      sigprocmask(SIG_BLOCK, &blocked, &original))
    fail();
  temporary = original;
  if (sigdelset(&temporary, SIGUSR1))
    fail();

  signalHandled = 0;
  if (kill(getpid(), SIGUSR1))
    fail();
  errno = 0;
  const int result = sigsuspend(&temporary);
  const int suspendError = errno;

  sigset_t restored;
  if (sigprocmask(SIG_SETMASK, 0, &restored) || sigprocmask(SIG_SETMASK, &original, 0) ||
      sigaction(SIGUSR1, &previousAction, 0))
    fail();
  if (result != -1 || suspendError != EINTR || !signalHandled ||
      sigismember(&restored, SIGUSR1) != 1)
    fail();

  status("OK");
}

void test_process(void) {
  printf("Testing process compatibility...\n");
  test_proc_self_fd();
  test_vfork();
  test_signal_return();
  test_default_signal_termination();
  test_thread_signal_syscalls();
  test_sigsuspend();
}
