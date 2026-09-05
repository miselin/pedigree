/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));
extern void test_robust_memory_contracts(const char* program);

struct shared_fixture {
  int futex;
  pthread_mutex_t mutex;
};
struct identity {
  long tid;
  int valid;
};

static void* identity_worker(void* parameter) {
  struct identity* identity = parameter;
  identity->tid = syscall(SYS_gettid);
  identity->valid = identity->tid > 0 && syscall(SYS_tgkill, getpid(), identity->tid, 0) == 0;
  return NULL;
}

static int reap_exit_code(pid_t child) {
  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  if (result != child || !WIFEXITED(status)) {
    printf("FUTEX-CONTRACT: reap child=%d result=%d status=%#x errno=%d\n", child, result, status,
           errno);
    fflush(stdout);
    return -1;
  }
  return WEXITSTATUS(status);
}

static int reap(pid_t child) {
  int code = reap_exit_code(child);
  if (code > 0) {
    printf("FUTEX-CONTRACT: child=%d exit=%d\n", child, code);
    fflush(stdout);
  }
  return code == 0;
}

struct exit_probe {
  int ready;
  int release;
};

static void* exit_race_worker(void* parameter) {
  struct exit_probe* probe = parameter;
  __atomic_add_fetch(&probe->ready, 1, __ATOMIC_RELEASE);
  while (!__atomic_load_n(&probe->release, __ATOMIC_ACQUIRE))
    sched_yield();
  return NULL;
}

static void* raw_exit_worker(void* parameter) {
  exit_race_worker(parameter);
  syscall(SYS_exit, 43);
  _exit(90);
}

static int raw_exit_contracts(void) {
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    syscall(SYS_exit, 37);
    _exit(90);
  }
  if (reap_exit_code(child) != 37)
    return 0;
  puts("FUTEX-CONTRACT: raw single-thread exit status passed");
  fflush(stdout);

  for (size_t attempt = 0; attempt < 4; ++attempt) {
    child = fork();
    if (child < 0)
      return 0;
    if (!child) {
      alarm(5);
      struct exit_probe early = {0};
      struct exit_probe late = {0};
      pthread_t workers[4];
      for (size_t i = 0; i < 2; ++i) {
        if (pthread_create(&workers[i], NULL, exit_race_worker, &early))
          _exit(91);
      }
      while (__atomic_load_n(&early.ready, __ATOMIC_ACQUIRE) != 2)
        sched_yield();
      __atomic_store_n(&early.release, 1, __ATOMIC_RELEASE);
      // Normal pthread cleanup keeps musl's thread list coherent while these
      // exits overlap new clone publication.
      for (size_t i = 2; i < 4; ++i) {
        if (pthread_create(&workers[i], NULL, raw_exit_worker, &late))
          _exit(92);
      }
      for (size_t i = 0; i < 2; ++i) {
        if (pthread_join(workers[i], NULL))
          _exit(93);
      }
      while (__atomic_load_n(&late.ready, __ATOMIC_ACQUIRE) != 2)
        sched_yield();
      // No pthread operations follow these raw exits, which intentionally
      // bypass musl's own detach-state and thread-list cleanup.
      __atomic_store_n(&late.release, 1, __ATOMIC_RELEASE);
      syscall(SYS_exit, 42);
      _exit(90);
    }
    const int code = reap_exit_code(child);
    if (code != 42 && code != 43) {
      printf("FUTEX-CONTRACT: concurrent raw exit status=%d attempt=%zu\n", code, attempt);
      fflush(stdout);
      return 0;
    }
  }
  puts("FUTEX-CONTRACT: concurrent exits and clone publication passed");
  fflush(stdout);
  return 1;
}

static int shared_requeue(int first_fd, struct shared_fixture* first, struct shared_fixture* second,
                          size_t page_size) {
  int ready[2];
  if (pipe(ready))
    return 0;
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    close(ready[0]);
    alarm(5);
    struct shared_fixture* alias =
        mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, first_fd, 0);
    if (alias == MAP_FAILED || alias == first || munmap(first, page_size) ||
        syscall(SYS_gettid) != getpid())
      _exit(11);
    if (write(ready[1], "r", 1) != 1)
      _exit(12);
    long result;
    do {
      result = syscall(SYS_futex, &alias->futex, 0, 0, NULL, NULL, 0);
    } while (result < 0 && errno == EINTR);
    _exit(result == 0 ? 0 : 13);
  }
  close(ready[1]);
  char token = 0;
  int valid = read(ready[0], &token, 1) == 1 && token == 'r';
  close(ready[0]);
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  const time_t deadline = now.tv_sec + 3;
  long moved = 0;
  while (valid && moved == 0 && now.tv_sec < deadline) {
    moved = syscall(SYS_futex, &first->futex, 3, 0, 1, &second->futex, 0);
    if (!moved)
      sched_yield();
    clock_gettime(CLOCK_MONOTONIC, &now);
  }
  valid = valid && moved == 1 &&
          syscall(SYS_futex, &first->futex, 1, INT_MAX, NULL, NULL, 0) == 0 &&
          syscall(SYS_futex, &second->futex, 1, 1, NULL, NULL, 0) == 1;
  printf("FUTEX-CONTRACT: shared-requeue moved=%ld valid=%d\n", moved, valid);
  fflush(stdout);
  if (!valid)
    kill(child, SIGKILL);
  return reap(child) && valid;
}

static int robust_owner_exit(int fd, struct shared_fixture* shared, size_t page_size) {
  pthread_mutexattr_t attributes;
  if (pthread_mutexattr_init(&attributes) ||
      pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED) ||
      pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST) ||
      pthread_mutex_init(&shared->mutex, &attributes))
    return 0;
  pthread_mutexattr_destroy(&attributes);
  int ready[2];
  int release[2];
  if (pipe(ready) || pipe(release))
    return 0;
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    close(ready[0]);
    close(release[1]);
    alarm(5);
    struct shared_fixture* alias = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (alias == MAP_FAILED || alias == shared || munmap(shared, page_size) ||
        syscall(SYS_gettid) != getpid() || pthread_mutex_lock(&alias->mutex))
      _exit(21);
    if (write(ready[1], "r", 1) != 1)
      _exit(22);
    char token;
    if (read(release[0], &token, 1) != 1)
      _exit(23);
    puts("FUTEX-CONTRACT: robust owner exiting");
    fflush(stdout);
    // Raw thread exit exercises the kernel's registered robust-list cleanup.
    syscall(SYS_exit, 0);
    _exit(24);
  }
  close(ready[1]);
  close(release[0]);
  char token = 0;
  int valid = read(ready[0], &token, 1) == 1 && token == 'r';
  close(ready[0]);
  valid = valid && write(release[1], "x", 1) == 1;
  close(release[1]);
  if (!valid) {
    kill(child, SIGKILL);
    reap(child);
    return 0;
  }
  puts("FUTEX-CONTRACT: robust recovery begin");
  fflush(stdout);
  int locked = pthread_mutex_lock(&shared->mutex);
  printf("FUTEX-CONTRACT: robust recovery result=%d expected=%d\n", locked, EOWNERDEAD);
  fflush(stdout);
  valid = locked == EOWNERDEAD;
  if (locked == EOWNERDEAD)
    valid = pthread_mutex_consistent(&shared->mutex) == 0 && valid;
  if (locked == 0 || locked == EOWNERDEAD)
    valid = pthread_mutex_unlock(&shared->mutex) == 0 && valid;
  puts("FUTEX-CONTRACT: robust owner reap begin");
  fflush(stdout);
  valid = reap(child) && valid;
  printf("FUTEX-CONTRACT: robust owner reap complete valid=%d\n", valid);
  fflush(stdout);
  return pthread_mutex_destroy(&shared->mutex) == 0 && valid;
}

static int contracts_child(void) {
  alarm(15);
  if (!raw_exit_contracts())
    return 8;
  const long main_tid = syscall(SYS_gettid);
  if (main_tid != getpid())
    return 1;
  struct identity identities[2] = {{0}};
  for (size_t i = 0; i < 2; ++i) {
    pthread_t thread;
    if (pthread_create(&thread, NULL, identity_worker, &identities[i]) ||
        pthread_join(thread, NULL))
      return 2;
    if (!identities[i].valid || identities[i].tid == main_tid)
      return 3;
  }
  if (identities[0].tid == identities[1].tid)
    return 4;
  puts("FUTEX-CONTRACT: task identities passed");
  fflush(stdout);
  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  char paths[2][80];
  int files[2];
  struct shared_fixture* mappings[2];
  for (size_t i = 0; i < 2; ++i) {
    snprintf(paths[i], sizeof(paths[i]), "/tmp/futex-contract-%d-%zu", getpid(), i);
    files[i] = open(paths[i], O_CREAT | O_TRUNC | O_RDWR, 0600);
    if (files[i] < 0 || ftruncate(files[i], (off_t)page_size))
      return 5;
    mappings[i] = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, files[i], 0);
    if (mappings[i] == MAP_FAILED)
      return 6;
    memset(mappings[i], 0, sizeof(*mappings[i]));
  }
  int valid = shared_requeue(files[0], mappings[0], mappings[1], page_size);
  valid = robust_owner_exit(files[0], mappings[0], page_size) && valid;
  for (size_t i = 0; i < 2; ++i) {
    valid = munmap(mappings[i], page_size) == 0 && valid;
    valid = close(files[i]) == 0 && valid;
    valid = unlink(paths[i]) == 0 && valid;
  }
  return valid ? 0 : 7;
}

void test_futex_contracts(const char* program) {
  pid_t child = fork();
  if (!child)
    _exit(contracts_child());
  if (child < 0 || !reap(child)) {
    puts("FUTEX-CONTRACT: FAIL task-ids-shared-requeue-robust-exit");
    fail();
  }
  puts("FUTEX-CONTRACT: PASS task-ids-shared-requeue-robust-exit");
  test_robust_memory_contracts(program);
}
