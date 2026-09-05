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
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));
static const char* executable;

static int wait_word(int* word, int expected) {
  struct timespec now;
  clock_gettime(CLOCK_MONOTONIC, &now);
  const time_t deadline = now.tv_sec + 3;
  while (__atomic_load_n(word, __ATOMIC_ACQUIRE) != expected) {
    if (now.tv_sec >= deadline)
      return 0;
    sched_yield();
    clock_gettime(CLOCK_MONOTONIC, &now);
  }
  return 1;
}

static int reap_code(pid_t child, int expected) {
  int status = 0;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  return result == child && WIFEXITED(status) && WEXITSTATUS(status) == expected;
}

static int reap(pid_t child) {
  return reap_code(child, 0);
}

struct cow_probe {
  pthread_mutex_t* mutex;
  int ready;
  int release;
  int exited;
};

static void* cow_owner(void* parameter) {
  struct cow_probe* probe = parameter;
  if (pthread_mutex_lock(probe->mutex))
    _exit(11);
  probe->exited = (int)syscall(SYS_set_tid_address, &probe->exited);
  __atomic_store_n(&probe->ready, 1, __ATOMIC_RELEASE);
  if (!wait_word(&probe->release, 1))
    _exit(12);
  syscall(SYS_exit, 0);
  _exit(13);
}

static int private_cow_recovery(void) {
  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  pthread_mutex_t* mutex =
      mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (mutex == MAP_FAILED)
    return 0;
  pthread_mutexattr_t attributes;
  if (pthread_mutexattr_init(&attributes) ||
      pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED) ||
      pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST) ||
      pthread_mutex_init(mutex, &attributes))
    return 0;
  pthread_mutexattr_destroy(&attributes);
  struct cow_probe probe = {mutex, 0, 0, -1};
  pthread_t owner;
  if (pthread_create(&owner, NULL, cow_owner, &probe) || !wait_word(&probe.ready, 1))
    return 0;
  int ready[2], release[2];
  if (pipe(ready) || pipe(release))
    return 0;
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    alarm(5);
    close(ready[0]);
    close(release[1]);
    if (pthread_mutex_trylock(mutex) != EBUSY || write(ready[1], "r", 1) != 1)
      _exit(14);
    char token;
    if (read(release[0], &token, 1) != 1 || pthread_mutex_trylock(mutex) != EBUSY)
      _exit(15);
    _exit(0);
  }
  close(ready[1]);
  close(release[0]);
  char token;
  if (read(ready[0], &token, 1) != 1)
    return 0;
  close(ready[0]);
  // This touches the coordination page after fork, leaving the separate mutex
  // page COW until kernel owner-death processing writes it.
  __atomic_store_n(&probe.release, 1, __ATOMIC_RELEASE);
  int valid = wait_word(&probe.exited, 0);
  if (valid) {
    const int locked = pthread_mutex_trylock(mutex);
    valid = locked == EOWNERDEAD;
    if (locked == EOWNERDEAD)
      valid = pthread_mutex_consistent(mutex) == 0 && valid;
    if (locked == 0 || locked == EOWNERDEAD)
      valid = pthread_mutex_unlock(mutex) == 0 && valid;
  }
  valid = write(release[1], "x", 1) == 1 && valid;
  close(release[1]);
  valid = reap(child) && valid;
  valid = pthread_mutex_destroy(mutex) == 0 && valid;
  return munmap(mutex, page_size) == 0 && valid;
}

struct robust_head {
  uintptr_t next;
  intptr_t offset;
  uintptr_t pending;
};
struct robust_node {
  uintptr_t next;
  uint32_t owner;
};
struct demand_probe {
  uintptr_t address;
  size_t page_size;
  int fd;
  int exited;
  int owner;
};

static void* demand_owner(void* parameter) {
  struct demand_probe* probe = parameter;
  const int tid = (int)syscall(SYS_gettid);
  struct robust_head head = {probe->address + probe->page_size, offsetof(struct robust_node, owner),
                             0};
  struct robust_node node = {probe->address, (uint32_t)tid};
  if (pwrite(probe->fd, &head, sizeof(head), 0) != (ssize_t)sizeof(head) ||
      pwrite(probe->fd, &node, sizeof(node), (off_t)probe->page_size) != (ssize_t)sizeof(node))
    _exit(21);
  probe->owner = tid;
  probe->exited = (int)syscall(SYS_set_tid_address, &probe->exited);
  if (syscall(SYS_set_robust_list, probe->address, sizeof(head)))
    _exit(22);
  // Neither list page has been touched through this private mapping.
  syscall(SYS_exit, 0);
  _exit(23);
}

static int demand_recovery_case(int protected_head) {
  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  char path[80];
  snprintf(path, sizeof(path), "/tmp/robust-memory-%d", getpid());
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0 || ftruncate(fd, (off_t)(2 * page_size)))
    return 0;
  uintptr_t address =
      (uintptr_t)mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  if ((void*)address == MAP_FAILED)
    return 0;
  if (protected_head && mprotect((void*)address, page_size, PROT_NONE))
    return 0;
  struct demand_probe probe = {address, page_size, fd, -1, 0};
  pthread_t owner;
  if (pthread_create(&owner, NULL, demand_owner, &probe) || !wait_word(&probe.exited, 0))
    return 0;
  struct robust_node* node = (struct robust_node*)(address + page_size);
  const uint32_t expected = protected_head ? (uint32_t)probe.owner : UINT32_C(0x40000000);
  const int valid = __atomic_load_n(&node->owner, __ATOMIC_ACQUIRE) == expected;
  return munmap((void*)address, 2 * page_size) == 0 && close(fd) == 0 && unlink(path) == 0 && valid;
}

static int demand_recovery(void) {
  return demand_recovery_case(0);
}

static int protected_list(void) {
  return demand_recovery_case(1);
}

struct ctid_probe {
  int* word;
  int ready;
  int release;
};

static void* ctid_owner(void* parameter) {
  struct ctid_probe* probe = parameter;
  *probe->word = (int)syscall(SYS_set_tid_address, probe->word);
  __atomic_store_n(&probe->ready, 1, __ATOMIC_RELEASE);
  if (!wait_word(&probe->release, 1))
    _exit(31);
  syscall(SYS_exit, 0);
  _exit(32);
}

static int clear_tid_cow(void) {
  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  int* word = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (word == MAP_FAILED)
    return 0;
  struct ctid_probe probe = {word, 0, 0};
  pthread_t owner;
  if (pthread_create(&owner, NULL, ctid_owner, &probe) || !wait_word(&probe.ready, 1))
    return 0;
  int release[2];
  if (pipe(release))
    return 0;
  const int tid = *word;
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    alarm(5);
    close(release[1]);
    char token;
    if (read(release[0], &token, 1) != 1 || *word != tid)
      _exit(33);
    _exit(0);
  }
  close(release[0]);
  __atomic_store_n(&probe.release, 1, __ATOMIC_RELEASE);
  // No parent write touches the dedicated TID page after fork.
  int valid = tid > 0 && wait_word(word, 0);
  valid = write(release[1], "x", 1) == 1 && valid;
  close(release[1]);
  valid = reap(child) && valid;
  return munmap(word, page_size) == 0 && valid;
}

static int exec_recovery(void) {
  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  char path[80];
  snprintf(path, sizeof(path), "/tmp/robust-exec-%d", getpid());
  int fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0 || ftruncate(fd, (off_t)page_size))
    return 0;
  pthread_mutex_t* mutex = mmap(NULL, page_size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  if (mutex == MAP_FAILED)
    return 0;
  pthread_mutexattr_t attributes;
  if (pthread_mutexattr_init(&attributes) ||
      pthread_mutexattr_setpshared(&attributes, PTHREAD_PROCESS_SHARED) ||
      pthread_mutexattr_setrobust(&attributes, PTHREAD_MUTEX_ROBUST) ||
      pthread_mutex_init(mutex, &attributes))
    return 0;
  pthread_mutexattr_destroy(&attributes);
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    if (pthread_mutex_lock(mutex))
      _exit(41);
    uintptr_t before = 0, after = 0;
    size_t length = 0;
    if (syscall(SYS_get_robust_list, 0, &before, &length) || !before)
      _exit(42);
    char* const arguments[] = {(char*)executable, (char*)"--exec-shebang-unexpected-child", NULL};
    execv("/__pedigree_missing_robust_exec__", arguments);
    if (errno != ENOENT || syscall(SYS_get_robust_list, 0, &after, &length) || before != after)
      _exit(43);
    execv(executable, arguments);
    _exit(44);
  }
  int valid = reap_code(child, 120);
  const int locked = pthread_mutex_trylock(mutex);
  valid = locked == EOWNERDEAD && valid;
  if (locked == EOWNERDEAD)
    valid = pthread_mutex_consistent(mutex) == 0 && valid;
  if (locked == 0 || locked == EOWNERDEAD)
    valid = pthread_mutex_unlock(mutex) == 0 && valid;
  valid = pthread_mutex_destroy(mutex) == 0 && valid;
  return munmap(mutex, page_size) == 0 && close(fd) == 0 && unlink(path) == 0 && valid;
}

static int run_case(const char* name, int (*operation)(void)) {
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    alarm(5);
    _exit(operation() ? 0 : 1);
  }
  const int passed = reap(child);
  printf("ROBUST-MEMORY-CONTRACT: %s %s\n", passed ? "PASS" : "FAIL", name);
  fflush(stdout);
  return passed;
}

void test_robust_memory_contracts(const char* program) {
  executable = program;
  const int cow = run_case("private-cow-isolation", private_cow_recovery);
  const int demand = run_case("demand-head-and-link", demand_recovery);
  const int protected = run_case("protected-head-preservation", protected_list);
  const int ctid = run_case("clear-tid-cow-isolation", clear_tid_cow);
  const int exec = run_case("exec-owner-recovery", exec_recovery);
  if (!cow || !demand || !protected || !ctid || !exec)
    fail();
  puts("ROBUST-MEMORY-CONTRACT: PASS all");
}
