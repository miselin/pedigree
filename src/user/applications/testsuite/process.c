/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
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

void test_process(void) {
  printf("Testing process compatibility...\n");
  test_proc_self_fd();
  test_vfork();
  test_signal_return();
  test_default_signal_termination();
}
