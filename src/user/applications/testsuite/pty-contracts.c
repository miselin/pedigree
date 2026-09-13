/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pty.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>

#include <sys/ioctl.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));

static void require(int condition, const char* operation) {
  if (!condition) {
    printf("PTY-CONTRACT: FAIL %s errno=%d\n", operation, errno);
    fail();
  }
}

static void make_raw(int descriptor) {
  struct termios attributes;
  require(tcgetattr(descriptor, &attributes) == 0, "tcgetattr");
  cfmakeraw(&attributes);
  require(tcsetattr(descriptor, TCSANOW, &attributes) == 0, "tcsetattr");
}

static void exchange_byte(int writer, int reader, char expected, const char* operation) {
  char received = 0;
  require(write(writer, &expected, 1) == 1, operation);
  errno = 0;
  ssize_t amount = read(reader, &received, 1);
  require(amount == 1 && received == expected, operation);
}

static void read_exact(int descriptor, char* buffer, size_t length, const char* operation) {
  size_t total = 0;
  while (total < length) {
    ssize_t amount = read(descriptor, buffer + total, length - total);
    require(amount > 0, operation);
    total += (size_t)amount;
  }
}

static void test_posix_openpt_contract(void) {
  int master = posix_openpt(O_RDWR | O_NOCTTY | O_CLOEXEC);
  require(master >= 0, "posix_openpt");
  require(grantpt(master) == 0, "grantpt");

  char path[64];
  require(ptsname_r(master, path, sizeof(path)) == 0, "ptsname_r");

  int locked = 1;
  require(ioctl(master, TIOCSPTLCK, &locked) == 0, "lock PTY slave");
  errno = 0;
  int slave = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
  require(slave < 0 && errno == EIO, "locked PTY slave rejection");

  locked = 0;
  require(unlockpt(master) == 0, "unlockpt");
  slave = open(path, O_RDWR | O_NOCTTY | O_CLOEXEC);
  require(slave >= 0, "unlocked PTY slave");
  make_raw(slave);
  exchange_byte(master, slave, 'm', "master-to-slave");
  exchange_byte(slave, master, 's', "slave-to-master");

  close(slave);
  close(master);
}

static void test_openpty_contract(void) {
  int master = -1;
  int slave = -1;
  char path[64] = {0};
  struct winsize size = {.ws_row = 25, .ws_col = 80};
  require(openpty(&master, &slave, path, NULL, &size) == 0, "openpty");
  require(master >= 0 && slave >= 0 && path[0] == '/', "openpty outputs");
  make_raw(slave);
  exchange_byte(master, slave, 'o', "openpty master-to-slave");
  exchange_byte(slave, master, 'p', "openpty slave-to-master");
  close(slave);
  close(master);
}

static void test_fork_openpty_contract(void) {
  int master = -1;
  int slave = -1;
  require(openpty(&master, &slave, NULL, NULL, NULL) == 0, "fork openpty");

  pid_t child = fork();
  require(child >= 0, "fork openpty fork");
  if (child == 0) {
    close(master);
    const char message[] = "fork-openpty-ok";
    int result =
        write(slave, message, sizeof(message) - 1) == (ssize_t)(sizeof(message) - 1) ? 0 : 111;
    _exit(result);
  }

  close(slave);
  char output[sizeof("fork-openpty-ok") - 1];
  read_exact(master, output, sizeof(output), "fork openpty output");
  require(!memcmp(output, "fork-openpty-ok", sizeof(output)), "fork openpty output contents");

  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "fork openpty child status");
  close(master);
}

static void test_forkpty_contract(void) {
  int master = -1;
  char path[64] = {0};
  pid_t child = forkpty(&master, path, NULL, NULL);
  require(child >= 0, "forkpty");
  if (child == 0) {
    const char message[] = "forkpty-ok";
    int result =
        write(STDOUT_FILENO, message, sizeof(message) - 1) == (ssize_t)(sizeof(message) - 1) ? 0
                                                                                             : 111;
    _exit(result);
  }

  char output[sizeof("forkpty-ok") - 1];
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "forkpty child status");
  read_exact(master, output, sizeof(output), "forkpty output");
  require(!memcmp(output, "forkpty-ok", sizeof(output)), "forkpty output contents");
  close(master);
}

void test_pty_contracts(void) {
  printf("Testing PTY contracts...");
  fflush(stdout);
  test_posix_openpt_contract();
  test_openpty_contract();
  test_fork_openpty_contract();
  test_forkpty_contract();
  printf("OK\n");
}
