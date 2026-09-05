/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <termios.h>
#include <unistd.h>

#include <sys/epoll.h>
#include <sys/ioctl.h>

extern void fail(void) __attribute__((noreturn));

static void fail_operation(const char* operation) {
  printf("\nepoll PTY: %s failed (errno=%d)\n", operation, errno);
  fail();
}

static void expect_read_edges(int writer, int reader, char first, char second) {
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0)
    fail_operation("create read epoll");

  struct epoll_event requested = {.events = EPOLLIN | EPOLLET, .data.u64 = 0x505459};
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, reader, &requested))
    fail_operation("register reader");

  struct epoll_event result = {0};
  if (epoll_wait(epoll_fd, &result, 1, 0) != 0)
    fail_operation("initial read readiness");

  if (write(writer, &first, 1) != 1 || epoll_wait(epoll_fd, &result, 1, 1000) != 1 ||
      !(result.events & EPOLLIN) || result.data.u64 != requested.data.u64)
    fail_operation("first write/readiness");

  char received = 0;
  if (read(reader, &received, 1) != 1 || received != first ||
      epoll_wait(epoll_fd, &result, 1, 0) != 0)
    fail_operation("first read/drain");

  if (write(writer, &second, 1) != 1 || epoll_wait(epoll_fd, &result, 1, 1000) != 1 ||
      !(result.events & EPOLLIN))
    fail_operation("second write/readiness");
  received = 0;
  if (read(reader, &received, 1) != 1 || received != second)
    fail_operation("second read");

  close(epoll_fd);
}

static void expect_writable(int descriptor) {
  int epoll_fd = epoll_create1(EPOLL_CLOEXEC);
  if (epoll_fd < 0)
    fail_operation("create write epoll");

  struct epoll_event requested = {.events = EPOLLOUT, .data.fd = descriptor};
  struct epoll_event result = {0};
  if (epoll_ctl(epoll_fd, EPOLL_CTL_ADD, descriptor, &requested) ||
      epoll_wait(epoll_fd, &result, 1, 0) != 1 || !(result.events & EPOLLOUT) ||
      result.data.fd != descriptor)
    fail_operation("write readiness");

  close(epoll_fd);
}

void test_epoll_pty(void) {
  printf("Testing epoll PTY readiness...");
  fflush(stdout);

  int master = posix_openpt(O_RDWR | O_NONBLOCK | O_NOCTTY);
  unsigned int terminal = 0;
  if (master < 0 || ioctl(master, TIOCGPTN, &terminal))
    fail_operation("open master");

  char path[64];
  if (snprintf(path, sizeof(path), "/dev/pts/%u", terminal) >= (int)sizeof(path))
    fail_operation("slave path");
  int slave = open(path, O_RDWR | O_NONBLOCK | O_NOCTTY);
  if (slave < 0)
    fail_operation("open slave");

  struct termios attributes;
  if (tcgetattr(slave, &attributes))
    fail_operation("tcgetattr");
  cfmakeraw(&attributes);
  if (tcsetattr(slave, TCSANOW, &attributes))
    fail_operation("tcsetattr");

  expect_writable(master);
  expect_writable(slave);
  expect_read_edges(slave, master, 'a', '\n');
  expect_read_edges(master, slave, 'c', 'd');

  close(slave);
  close(master);
  printf("OK\n");
}
