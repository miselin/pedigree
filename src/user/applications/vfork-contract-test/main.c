/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define CHECK(expression)                                                          \
  do {                                                                             \
    if (!(expression)) {                                                           \
      fprintf(stderr, "VFORK-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      return 1;                                                                    \
    }                                                                              \
  } while (0)

extern char** environ;
static char self[PATH_MAX];
static volatile int shared_value;
static int child_ready, parent_returned, observer_failure;
static char* child_mapping;
static int observer_status;

static void pause_briefly(void) {
  const struct timespec delay = {0, 30000000};
  syscall(SYS_nanosleep, &delay, NULL);
}

static int exited(pid_t child, int code) {
  int status;
  CHECK(waitpid(child, &status, 0) == child);
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == code);
  return 0;
}

static int repeated_exit(void) {
  for (int iteration = 0; iteration < 12; ++iteration) {
    shared_value = 0;
    pid_t child = vfork();
    CHECK(child >= 0);
    if (!child) {
      shared_value = 1;
      pause_briefly();
      shared_value = 2;
      _exit(17);
    }
    CHECK(shared_value == 2);
    CHECK(exited(child, 17) == 0);
  }
  return 0;
}

static int private_fork(void) {
  shared_value = 11;
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    shared_value = 12;
    _exit(20);
  }
  CHECK(exited(child, 20) == 0 && shared_value == 11);
  return 0;
}

static int failed_exec(void) {
  char* arguments[] = {"missing", NULL};
  shared_value = 0;
  pid_t child = vfork();
  CHECK(child >= 0);
  if (!child) {
    if (syscall(SYS_execve, "/does-not-exist/vfork-contract", arguments, environ) != -1)
      _exit(81);
    shared_value = 3;
    pause_briefly();
    shared_value = 4;
    _exit(18);
  }
  CHECK(shared_value == 4);
  CHECK(exited(child, 18) == 0);
  return 0;
}

static int successful_exec(void) {
  int gate[2];
  CHECK(pipe(gate) == 0);
  char descriptor[24];
  snprintf(descriptor, sizeof(descriptor), "%d", gate[0]);
  char* arguments[] = {self, "--exec-child", descriptor, NULL};
  shared_value = 0;
  pid_t child = vfork();
  CHECK(child >= 0);
  if (!child) {
    shared_value = 5;
    syscall(SYS_execve, self, arguments, environ);
    _exit(82);
  }
  CHECK(shared_value == 5);
  // The replacement image cannot exit until vfork has returned to this write.
  CHECK(write(gate[1], "x", 1) == 1);
  CHECK(exited(child, 0) == 0);
  CHECK(close(gate[0]) == 0 && close(gate[1]) == 0);
  return 0;
}

static int clone_child(void* ignored) {
  (void)ignored;
  child_mapping = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (child_mapping == MAP_FAILED)
    return 83;
  child_mapping[0] = 42;
  shared_value = 6;
  pause_briefly();
  shared_value = 7;
  return 19;
}

static int spawn_clone(void) {
  const size_t stack_size = 65536;
  char* stack = mmap(NULL, stack_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(stack != MAP_FAILED);
  shared_value = 0;
  child_mapping = NULL;
  pid_t child = clone(clone_child, stack + stack_size, CLONE_VM | CLONE_VFORK | SIGCHLD, NULL);
  CHECK(child > 0 && shared_value == 7);
  CHECK(exited(child, 19) == 0);
  CHECK(child_mapping && child_mapping != MAP_FAILED && child_mapping[0] == 42);
  child_mapping[4095] = 43;
  CHECK(munmap(child_mapping, 4096) == 0);
  CHECK(munmap(stack, stack_size) == 0);
  return 0;
}

static void* kill_observer(void* ignored) {
  (void)ignored;
  int child;
  while (!(child = __atomic_load_n(&child_ready, __ATOMIC_ACQUIRE)))
    sched_yield();
  pause_briefly();
  if (__atomic_load_n(&parent_returned, __ATOMIC_ACQUIRE))
    observer_failure = 1;
  if (kill(child, SIGKILL))
    observer_failure = 2;
  if (waitpid(child, &observer_status, 0) != child)
    observer_failure = 3;
  return NULL;
}

static int signal_exit(void) {
  pthread_t observer;
  child_ready = parent_returned = observer_failure = 0;
  CHECK(pthread_create(&observer, NULL, kill_observer, NULL) == 0);
  pid_t child = vfork();
  CHECK(child >= 0);
  if (!child) {
    __atomic_store_n(&child_ready, (int)syscall(SYS_getpid), __ATOMIC_RELEASE);
    for (;;)
      syscall(SYS_pause);
  }
  __atomic_store_n(&parent_returned, 1, __ATOMIC_RELEASE);
  CHECK(pthread_join(observer, NULL) == 0 && !observer_failure);
  CHECK(WIFSIGNALED(observer_status) && WTERMSIG(observer_status) == SIGKILL);
  return 0;
}

int main(int argc, char** argv) {
  if (argc == 3 && !strcmp(argv[1], "--exec-child")) {
    char byte;
    return read(atoi(argv[2]), &byte, 1) == 1 && byte == 'x' ? 0 : 84;
  }
  alarm(30);
  ssize_t length = readlink("/proc/self/exe", self, sizeof(self) - 1);
  CHECK(length > 0 && length < (ssize_t)sizeof(self));
  self[length] = 0;
  CHECK(private_fork() == 0);
  CHECK(repeated_exit() == 0);
  CHECK(failed_exec() == 0);
  CHECK(successful_exec() == 0);
  CHECK(spawn_clone() == 0);
  CHECK(signal_exit() == 0);
  puts("VFORK-CONTRACT: PASS");
  return 0;
}
