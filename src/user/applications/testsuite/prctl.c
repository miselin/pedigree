/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/fsuid.h>
#include <sys/prctl.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));

static int death_ready[2], death_go[2], death_report[2];
static int death_mode;
struct death_notice {
  int signal;
  pid_t sender;
};

static void death_handler(int signal, siginfo_t* info, void* context) {
  (void)context;
  struct death_notice notice = {signal, info->si_pid};
  (void)write(death_report[1], &notice, sizeof(notice));
  _exit(0);
}

static void read_bounded(int fd, void* data, size_t size) {
  struct pollfd pollfd = {fd, POLLIN, 0};
  if (poll(&pollfd, 1, 5000) != 1 || read(fd, data, size) != (ssize_t)size) {
    fail();
  }
}

static void* death_creator(void* unused) {
  (void)unused;
  int setting = -1;
  if (prctl(PR_GET_PDEATHSIG, &setting, 0UL, 0UL, 0UL) || setting) {
    fail();
  }
  pid_t child = fork();
  if (child < 0) {
    fail();
  }
  if (!child) {
    pid_t parent = getppid();
    struct sigaction action = {.sa_sigaction = death_handler, .sa_flags = SA_SIGINFO};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL)) {
      fail();
    }
    if (death_mode != 2 && prctl(PR_SET_PDEATHSIG, SIGUSR1, 0UL, 0UL, 0UL)) {
      fail();
    }
    if (death_mode == 1 && prctl(PR_SET_PDEATHSIG, 0UL, 0UL, 0UL, 0UL)) {
      fail();
    }
    if (write(death_ready[1], "r", 1) != 1) {
      fail();
    }
    if (!death_mode) {
      for (;;) {
        pause();
      }
    }
    for (int i = 0; getppid() == parent && i < 1000; ++i) {
      usleep(1000);
    }
    if (getppid() == parent ||
        (death_mode == 2 && prctl(PR_SET_PDEATHSIG, SIGUSR1, 0UL, 0UL, 0UL))) {
      fail();
    }
    usleep(100000);
    struct death_notice notice = {0, parent};
    if (write(death_report[1], &notice, sizeof(notice)) != sizeof(notice)) {
      fail();
    }
    _exit(0);
  }
  char go;
  read_bounded(death_go[0], &go, 1);
  return (void*)(intptr_t)child;
}

static void test_creator_exit(int threaded, int mode) {
  death_mode = mode;
  if (pipe(death_ready) || pipe(death_go) || pipe(death_report)) {
    fail();
  }
  pthread_t thread;
  pid_t parent = getpid();
  if (threaded) {
    if (pthread_create(&thread, NULL, death_creator, NULL)) {
      fail();
    }
  } else {
    parent = fork();
    if (parent < 0) {
      fail();
    }
    if (!parent) {
      death_creator(NULL);
      _exit(0);
    }
  }
  char ready;
  read_bounded(death_ready[0], &ready, 1);
  if (write(death_go[1], "g", 1) != 1) {
    fail();
  }
  int status;
  if (threaded) {
    void* child;
    if (pthread_join(thread, &child)) {
      fail();
    }
    if (waitpid((pid_t)(intptr_t)child, &status, 0) < 0 || status) {
      fail();
    }
  } else if (waitpid(parent, &status, 0) != parent || status) {
    fail();
  }
  struct death_notice notice;
  read_bounded(death_report[0], &notice, sizeof(notice));
  if (notice.signal != (mode ? 0 : SIGUSR1) || notice.sender != parent) {
    fail();
  }
  for (int i = 0; i < 2; ++i) {
    close(death_ready[i]);
    close(death_go[i]);
    close(death_report[i]);
  }
}

int test_prctl_exec(void) {
  int signal = 0;
  return prctl(PR_GET_PDEATHSIG, &signal, 0UL, 0UL, 0UL) || signal != SIGUSR1;
}

static void test_parent_death(void) {
  int signal = -1;
  if (prctl(PR_SET_PDEATHSIG, SIGUSR1, 0UL, 0UL, 0UL) ||
      prctl(PR_GET_PDEATHSIG, &signal, 0UL, 0UL, 0UL) || signal != SIGUSR1) {
    fail();
  }
  errno = 0;
  if (prctl(PR_SET_PDEATHSIG, 65UL, 0UL, 0UL, 0UL) != -1 || errno != EINVAL) {
    fail();
  }
  errno = 0;
  if (prctl(PR_SET_PDEATHSIG, -1UL, 0UL, 0UL, 0UL) != -1 || errno != EINVAL) {
    fail();
  }
  errno = 0;
  if (prctl(PR_GET_PDEATHSIG, (void*)-1, 0UL, 0UL, 0UL) != -1 || errno != EFAULT) {
    fail();
  }
  pid_t child = fork();
  if (!child) {
    if (prctl(PR_GET_PDEATHSIG, &signal, 0UL, 0UL, 0UL) || signal) {
      _exit(1);
    }
    if (prctl(PR_SET_PDEATHSIG, SIGUSR1, 0UL, 0UL, 0UL)) {
      _exit(2);
    }
    execl("/proc/self/exe", "testsuite", "--prctl-exec", NULL);
    _exit(3);
  }
  int status;
  if (child < 0 || waitpid(child, &status, 0) != child || status) {
    fail();
  }
  // A new pthread has its own cleared setting while the main task stays armed.
  test_creator_exit(1, 0);
  if (prctl(PR_GET_PDEATHSIG, &signal, 0UL, 0UL, 0UL) || signal != SIGUSR1) {
    fail();
  }
  child = fork();
  if (!child) {
    if (getuid() == 0) {
      if (prctl(PR_SET_PDEATHSIG, SIGUSR1, 0UL, 0UL, 0UL)) {
        _exit(4);
      }
      (void)setfsuid(65534);
      if (prctl(PR_GET_PDEATHSIG, &signal, 0UL, 0UL, 0UL) || signal) {
        _exit(5);
      }
      (void)setfsuid(0);
      if (prctl(PR_SET_PDEATHSIG, SIGUSR1, 0UL, 0UL, 0UL) || setresuid(0, 65534, 0) ||
          prctl(PR_GET_PDEATHSIG, &signal, 0UL, 0UL, 0UL) || signal) {
        _exit(6);
      }
    }
    _exit(0);
  }
  if (child < 0 || waitpid(child, &status, 0) != child || status) {
    fail();
  }
  if (prctl(PR_SET_PDEATHSIG, 0UL, 0UL, 0UL, 0UL) ||
      prctl(PR_GET_PDEATHSIG, &signal, 0UL, 0UL, 0UL) || signal) {
    fail();
  }
  test_creator_exit(0, 0);
  test_creator_exit(0, 1);
  test_creator_exit(0, 2);
  puts("PDEATH-CONTRACT PASS");
}

void test_prctl(void) {
  puts("Testing prctl(2) thread names... ");
  fflush(stdout);

  char original[16] = {0};
  char returned[16];
  if (prctl(PR_GET_NAME, original, 0UL, 0UL, 0UL))
    fail();

  if (prctl(PR_SET_NAME, "0123456789abcdef-long", 0UL, 0UL, 0UL))
    fail();
  memset(returned, 0xA5, sizeof(returned));
  if (prctl(PR_GET_NAME, returned, 0UL, 0UL, 0UL) || memcmp(returned, "0123456789abcde", 15) ||
      returned[15])
    fail();

  errno = 0;
  if (prctl(-1, 0UL, 0UL, 0UL, 0UL) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (prctl(PR_GET_NAME, (char*)-1, 0UL, 0UL, 0UL) != -1 || errno != EFAULT)
    fail();
  errno = 0;
  if (prctl(PR_SET_NAME, (char*)-1, 0UL, 0UL, 0UL) != -1 || errno != EFAULT)
    fail();

  if (prctl(PR_SET_NAME, original, 0UL, 0UL, 0UL))
    fail();
  test_parent_death();
  puts("OK\n");
  fflush(stdout);
}
