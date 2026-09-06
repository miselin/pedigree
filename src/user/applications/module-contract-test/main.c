/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/wait.h>

extern int delete_module(const char*, unsigned int);

static int expect_error(const char* name, unsigned int flags, int expected) {
  errno = 0;
  int result = delete_module(name, flags);
  if (result != -1 || errno != expected) {
    fprintf(stderr, "delete_module flags=%x returned %d errno=%d expected=%d\n", flags, result,
            errno, expected);
    return 1;
  }
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(30);
  puts("MODULE-CONTRACT: BEGIN");
  if (geteuid() != 0) {
    fputs("MODULE-CONTRACT: FAIL requires root\n", stderr);
    return 1;
  }
  char longName[64];
  memset(longName, 'x', sizeof(longName));
  longName[sizeof(longName) - 1] = 0;
  if (expect_error(NULL, 0, EFAULT) || expect_error("", 0, ENOENT) ||
      expect_error(longName, 0, ENOENT) || expect_error("missing-module", 0, ENOENT) ||
      expect_error("module-unload-fixture", 1, EINVAL) ||
      expect_error("module-unload-fixture", O_TRUNC, EOPNOTSUPP) ||
      expect_error("posix", O_NONBLOCK, EBUSY) || expect_error("linker", O_NONBLOCK, EBUSY) ||
      expect_error("vfs", O_NONBLOCK, EWOULDBLOCK)) {
    puts("MODULE-CONTRACT: FAIL input-and-policy");
    return 1;
  }
  puts("MODULE-CONTRACT: PASS input-and-policy");

  pid_t child = fork();
  if (!child) {
    if (setuid(65534))
      _exit(2);
    _exit(expect_error("module-unload-fixture", O_NONBLOCK, EPERM));
  }
  int status = 0;
  if (child < 0 || waitpid(child, &status, 0) != child || !WIFEXITED(status) ||
      WEXITSTATUS(status)) {
    puts("MODULE-CONTRACT: FAIL credentials");
    return 1;
  }
  puts("MODULE-CONTRACT: PASS credentials");

  errno = 0;
  if (delete_module("module-unload-fixture", O_NONBLOCK)) {
    fprintf(stderr, "MODULE-CONTRACT: FAIL unload errno=%d\n", errno);
    return 1;
  }
  puts("MODULE-CONTRACT: PASS unload");
  if (expect_error("module-unload-fixture", 0, ENOENT)) {
    puts("MODULE-CONTRACT: FAIL repeated-unload");
    return 1;
  }
  puts("MODULE-CONTRACT: PASS repeated-unload");
  puts("MODULE-CONTRACT: END PASS");
  return 0;
}
