/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <unistd.h>

#include <sys/syscall.h>

extern void fail(void) __attribute__((noreturn));

enum { Dup3StressIterations = 256, Dup3WaitAttempts = 100000 };

struct dup3_stress_context {
  int source;
  int target;
  volatile int ready;
  volatile int start;
  volatile int finished;
  volatile int failed;
  volatile int observations;
};

static int wait_for_value(volatile int* value, int expected) {
  for (size_t attempt = 0; attempt < Dup3WaitAttempts; ++attempt) {
    if (__atomic_load_n(value, __ATOMIC_ACQUIRE) == expected)
      return 0;
    sched_yield();
  }
  return -1;
}

static void* replace_cloexec_descriptor(void* parameter) {
  struct dup3_stress_context* context = parameter;
  __atomic_add_fetch(&context->ready, 1, __ATOMIC_RELEASE);
  if (wait_for_value(&context->start, 1)) {
    __atomic_store_n(&context->failed, 1, __ATOMIC_RELEASE);
    __atomic_store_n(&context->finished, 1, __ATOMIC_RELEASE);
    return 0;
  }

  for (size_t iteration = 0; iteration < Dup3StressIterations; ++iteration) {
    if (syscall(SYS_dup3, context->source, context->target, O_CLOEXEC) != context->target) {
      __atomic_store_n(&context->failed, 1, __ATOMIC_RELEASE);
      break;
    }
    sched_yield();
  }
  __atomic_store_n(&context->finished, 1, __ATOMIC_RELEASE);
  return 0;
}

static void* observe_cloexec_descriptor(void* parameter) {
  struct dup3_stress_context* context = parameter;
  __atomic_add_fetch(&context->ready, 1, __ATOMIC_RELEASE);
  if (wait_for_value(&context->start, 1)) {
    __atomic_store_n(&context->failed, 1, __ATOMIC_RELEASE);
    return 0;
  }

  do {
    if (fcntl(context->target, F_GETFD) != FD_CLOEXEC) {
      __atomic_store_n(&context->failed, 1, __ATOMIC_RELEASE);
      return 0;
    }
    __atomic_add_fetch(&context->observations, 1, __ATOMIC_RELEASE);
    sched_yield();
  } while (!__atomic_load_n(&context->finished, __ATOMIC_ACQUIRE));
  return 0;
}

void test_dup3(void) {
  int sourcePipe[2];
  int displacedPipe[2];
  void (*previousSigpipe)(int) = signal(SIGPIPE, SIG_IGN);

  puts("Testing dup3 descriptor replacement... ");
  fflush(stdout);
  if (previousSigpipe == SIG_ERR || pipe(sourcePipe) || pipe(displacedPipe) ||
      fcntl(sourcePipe[0], F_SETFD, FD_CLOEXEC))
    fail();

  errno = 173;
  if (syscall(SYS_dup3, sourcePipe[0], displacedPipe[0], 0) != displacedPipe[0] || errno != 173 ||
      fcntl(sourcePipe[0], F_GETFD) != FD_CLOEXEC || fcntl(displacedPipe[0], F_GETFD) != 0)
    fail();

  errno = 0;
  if (write(displacedPipe[1], "x", 1) != -1 || errno != EPIPE || close(displacedPipe[1]))
    fail();

  if (write(sourcePipe[1], "a", 1) != 1)
    fail();
  char byte = 0;
  if (read(displacedPipe[0], &byte, 1) != 1 || byte != 'a')
    fail();

  if (syscall(SYS_dup3, sourcePipe[0], displacedPipe[0], O_CLOEXEC) != displacedPipe[0] ||
      fcntl(displacedPipe[0], F_GETFD) != FD_CLOEXEC)
    fail();

  errno = 0;
  if (syscall(SYS_dup3, sourcePipe[0], sourcePipe[0], 0) != -1 || errno != EINVAL ||
      fcntl(sourcePipe[0], F_GETFD) != FD_CLOEXEC)
    fail();
  errno = 0;
  if (syscall(SYS_dup3, -1, -1, 0) != -1 || errno != EINVAL)
    fail();
  errno = 0;
  if (syscall(SYS_dup3, sourcePipe[0], displacedPipe[0], O_NONBLOCK) != -1 || errno != EINVAL ||
      fcntl(displacedPipe[0], F_GETFD) != FD_CLOEXEC)
    fail();
  errno = 0;
  if (syscall(SYS_dup3, -1, displacedPipe[0], 0) != -1 || errno != EBADF ||
      fcntl(displacedPipe[0], F_GETFD) != FD_CLOEXEC)
    fail();
  errno = 0;
  if (syscall(SYS_dup3, sourcePipe[0], -1, 0) != -1 || errno != EBADF)
    fail();
  errno = 0;
  if (syscall(SYS_dup3, sourcePipe[0], 16384, 0) != -1 || errno != EBADF)
    fail();

  if (write(sourcePipe[1], "b", 1) != 1 || read(displacedPipe[0], &byte, 1) != 1 || byte != 'b')
    fail();

  struct dup3_stress_context stress = {
      .source = sourcePipe[0],
      .target = displacedPipe[0],
  };
  pthread_t replacer;
  pthread_t observer;
  if (pthread_create(&replacer, 0, replace_cloexec_descriptor, &stress))
    fail();
  if (pthread_create(&observer, 0, observe_cloexec_descriptor, &stress)) {
    __atomic_store_n(&stress.start, 1, __ATOMIC_RELEASE);
    pthread_join(replacer, 0);
    fail();
  }
  if (wait_for_value(&stress.ready, 2)) {
    __atomic_store_n(&stress.start, 1, __ATOMIC_RELEASE);
    pthread_join(replacer, 0);
    pthread_join(observer, 0);
    fail();
  }
  __atomic_store_n(&stress.start, 1, __ATOMIC_RELEASE);
  if (pthread_join(replacer, 0) || pthread_join(observer, 0) ||
      __atomic_load_n(&stress.failed, __ATOMIC_ACQUIRE) ||
      !__atomic_load_n(&stress.observations, __ATOMIC_ACQUIRE))
    fail();

  if (close(sourcePipe[0]) || write(sourcePipe[1], "c", 1) != 1 ||
      read(displacedPipe[0], &byte, 1) != 1 || byte != 'c' || close(displacedPipe[0]) ||
      close(sourcePipe[1]) || signal(SIGPIPE, previousSigpipe) == SIG_ERR)
    fail();

  puts("OK\n");
  fflush(stdout);
}
