/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/sem.h>
#include <sys/syscall.h>
#include <sys/wait.h>

union test_semun {
  int val;
  struct semid_ds* buf;
  unsigned short* array;
  struct seminfo* info;
};

#define CHECK(expression)                                                               \
  do {                                                                                  \
    if (!(expression)) {                                                                \
      printf("IPC-SEM: line %d failed: %s (errno=%d)\n", __LINE__, #expression, errno); \
      goto failed;                                                                      \
    }                                                                                   \
  } while (0)

static int reap(pid_t child) {
  int status;
  pid_t result;
  do {
    result = waitpid(child, &status, 0);
  } while (result < 0 && errno == EINTR);
  if (result == child && WIFEXITED(status) && !WEXITSTATUS(status))
    return 1;
  printf("IPC-SEM: child %d returned %d status=%#x errno=%d\n", child, result,
         result == child ? status : 0, errno);
  return 0;
}

static int wait_value(int id, int command, int value) {
  struct timespec start, now, pause = {0, 1000000};
  if (clock_gettime(CLOCK_MONOTONIC, &start))
    return 0;
  do {
    int result = semctl(id, 0, command);
    if (result == value)
      return 1;
    if (result < 0 || clock_gettime(CLOCK_MONOTONIC, &now))
      return 0;
    nanosleep(&pause, NULL);
  } while (now.tv_sec - start.tv_sec < 3);
  return 0;
}

static void interrupted(int signal_number) {
  (void)signal_number;
}

static int waiter_contract(int id, int zero, int expected_error, int remove_set) {
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    alarm(5);
    struct sigaction action = {.sa_handler = interrupted};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGUSR1, &action, NULL))
      _exit(10);
    struct sembuf operation = {0, zero ? 0 : -1, 0};
    const int result = semop(id, &operation, 1);
    _exit(expected_error ? !(result == -1 && errno == expected_error) : result != 0);
  }
  const int command = zero ? GETZCNT : GETNCNT;
  int ok = wait_value(id, command, 1);
  if (ok) {
    if (remove_set)
      ok = semctl(id, 0, IPC_RMID) == 0;
    else if (expected_error == EINTR)
      ok = kill(child, SIGUSR1) == 0;
    else
      ok = semctl(id, 0, SETVAL, (union test_semun){.val = zero ? 0 : 1}) == 0;
  }
  if (!ok)
    kill(child, SIGKILL);
  ok = reap(child) && ok;
  if (!remove_set && semctl(id, 0, command) != 0)
    ok = 0;
  return ok;
}

static void* undo_worker(void* argument) {
  struct sembuf operation = {0, -1, SEM_UNDO};
  return (void*)(intptr_t)semop(*(int*)argument, &operation, 1);
}

struct raw_undo {
  int id;
  int tid;
  int result;
  struct sembuf operation;
};

_Static_assert(offsetof(struct raw_undo, tid) == 4, "clone child TID offset");
_Static_assert(offsetof(struct raw_undo, result) == 8, "semop result offset");
_Static_assert(offsetof(struct raw_undo, operation) == 12, "semop operation offset");

// musl's public clone rejects CLONE_THREAD. This child has no libc thread state,
// so it executes only semop and exit directly, without entering C or touching TLS.
long ipc_sem_raw_clone(void* stack, struct raw_undo* state);
#define ASM_VALUE_INNER(value) #value
#define ASM_VALUE(value) ASM_VALUE_INNER(value)
__asm__(".text\n"
        ".global ipc_sem_raw_clone\n"
        ".hidden ipc_sem_raw_clone\n"
        ".type ipc_sem_raw_clone,@function\n"
        "ipc_sem_raw_clone:\n"
        "mov %rsi,%r9\n"
        "mov %rdi,%rsi\n"
        "and $-16,%rsi\n"
        "mov $(" ASM_VALUE(CLONE_VM | CLONE_FS | CLONE_FILES | CLONE_SIGHAND | CLONE_THREAD |
                           CLONE_CHILD_SETTID | CLONE_CHILD_CLEARTID) "),%edi\n"
        "lea 4(%r9),%r10\n"
        "xor %edx,%edx\n"
        "xor %r8d,%r8d\n"
        "mov $" ASM_VALUE(SYS_clone) ",%eax\n"
        "syscall\n"
        "test %rax,%rax\n"
        "jnz 1f\n"
        "mov (%r9),%edi\n"
        "lea 12(%r9),%rsi\n"
        "mov $1,%edx\n"
        "mov $" ASM_VALUE(SYS_semop) ",%eax\n"
        "syscall\n"
        "mov %eax,8(%r9)\n"
        "xor %edi,%edi\n"
        "mov $" ASM_VALUE(SYS_exit) ",%eax\n"
        "syscall\n"
        "ud2\n"
        "1: ret\n"
        ".size ipc_sem_raw_clone,.-ipc_sem_raw_clone\n");
#undef ASM_VALUE
#undef ASM_VALUE_INNER

static int undo_contracts(int id) {
  // A pthread shares undo state: joining it must leave its adjustment pending.
  pid_t child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    alarm(5);
    pthread_t thread;
    void* result;
    if (pthread_create(&thread, NULL, undo_worker, &id) || pthread_join(thread, &result) ||
        result || semctl(id, 0, GETVAL) != 0)
      _exit(11);
    _exit(0);
  }
  if (!reap(child) || semctl(id, 0, GETVAL) != 1)
    return 0;

  struct sembuf operation = {0, -1, SEM_UNDO};
  if (semop(id, &operation, 1))
    return 0;
  child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    alarm(5);
    _exit(0);
  }
  if (!reap(child) || semctl(id, 0, GETVAL) != 0 ||
      semctl(id, 0, SETVAL, (union test_semun){.val = 1}))
    return 0;

  // Omitting CLONE_SYSVSEM gives a thread an independent undo lifetime.
  child = fork();
  if (child < 0)
    return 0;
  if (!child) {
    alarm(5);
    const size_t size = 65536;
    void* stack = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (stack == MAP_FAILED)
      _exit(12);
    struct raw_undo state = {id, -1, -1, {0, -1, SEM_UNDO}};
    const long tid = ipc_sem_raw_clone((char*)stack + size, &state);
    if (tid < 0) {
      printf("IPC-SEM: independent undo clone failed: %ld\n", tid);
      _exit(13);
    }
    while (__atomic_load_n(&state.tid, __ATOMIC_ACQUIRE))
      sched_yield();
    const int value = semctl(id, 0, GETVAL);
    if (state.result || value != 1) {
      printf("IPC-SEM: independent undo semop=%d value=%d\n", state.result, value);
      _exit(14);
    }
    munmap(stack, size);
    _exit(0);
  }
  if (!reap(child) || semctl(id, 0, GETVAL) != 1)
    return 0;

  // SETVAL clears every process's outstanding undo adjustment for that entry.
  int ready[2], release[2];
  if (pipe(ready))
    return 0;
  if (pipe(release)) {
    close(ready[0]);
    close(ready[1]);
    return 0;
  }
  child = fork();
  if (child < 0) {
    close(ready[0]);
    close(ready[1]);
    close(release[0]);
    close(release[1]);
    return 0;
  }
  if (!child) {
    alarm(5);
    close(ready[0]);
    close(release[1]);
    char token = 0;
    if (semop(id, &operation, 1) || write(ready[1], &token, 1) != 1 ||
        read(release[0], &token, 1) != 1)
      _exit(15);
    _exit(0);
  }
  close(ready[1]);
  close(release[0]);
  char token;
  int ok =
      read(ready[0], &token, 1) == 1 && semctl(id, 0, SETVAL, (union test_semun){.val = 4}) == 0;
  close(ready[0]);
  if (ok)
    ok = write(release[1], &token, 1) == 1;
  close(release[1]);
  if (!ok)
    kill(child, SIGKILL);
  return reap(child) && ok && semctl(id, 0, GETVAL) == 4;
}

int ipc_test_semaphores(void) {
  int id = -1, keyed = -1;
  id = semget(IPC_PRIVATE, 3, 0600);
  CHECK(id >= 0);
  struct semid_ds metadata;
  CHECK(semctl(id, 0, IPC_STAT, (union test_semun){.buf = &metadata}) == 0);
  CHECK(metadata.sem_nsems == 3 && !metadata.sem_otime && metadata.sem_ctime > 0);
  CHECK(metadata.sem_perm.uid == geteuid() && (metadata.sem_perm.mode & 0777) == 0600);
  struct seminfo information;
  const int highest = semctl(0, 0, IPC_INFO, (union test_semun){.info = &information});
  CHECK(highest >= 0 && information.semmsl >= 3 && information.semvmx == 32767 &&
        information.semopm >= 3);
  int found = 0;
  for (int index = 0; index <= highest; ++index) {
    if (semctl(index, 0, SEM_STAT, (union test_semun){.buf = &metadata}) == id) {
      CHECK(metadata.sem_nsems == 3);
      CHECK(semctl(index, 0, SEM_STAT_ANY, (union test_semun){.buf = &metadata}) == id);
      found = 1;
      break;
    }
  }
  CHECK(found);
  CHECK(semctl(0, 0, SEM_INFO, (union test_semun){.info = &information}) >= 0 &&
        information.semusz >= 1 && information.semaem >= 3);

  key_t key = (key_t)(0x53450000u | (getpid() & 0xffff));
  keyed = semget(key, 2, IPC_CREAT | IPC_EXCL | 0600);
  CHECK(keyed >= 0);
  CHECK(semget(key, 0, 0600) == keyed);
  errno = 0;
  CHECK(semget(key, 2, IPC_CREAT | IPC_EXCL | 0600) == -1 && errno == EEXIST);
  errno = 0;
  CHECK(semget(key, 3, 0600) == -1 && errno == EINVAL);
  CHECK(semctl(keyed, 0, IPC_RMID) == 0);
  keyed = -1;
  errno = 0;
  CHECK(semget(key, 0, 0600) == -1 && errno == ENOENT);

  unsigned short values[3] = {99, 99, 99};
  CHECK(semctl(id, 0, GETALL, (union test_semun){.array = values}) == 0);
  CHECK(!values[0] && !values[1] && !values[2]);
  values[0] = 2;
  values[2] = 1;
  CHECK(semctl(id, 0, SETALL, (union test_semun){.array = values}) == 0);
  CHECK(semctl(id, 0, GETPID) == getpid());
  values[0] = 9;
  values[1] = 65535;
  errno = 0;
  CHECK(semctl(id, 0, SETALL, (union test_semun){.array = values}) == -1 && errno == ERANGE);
  CHECK(semctl(id, 0, GETVAL) == 2 && semctl(id, 1, GETVAL) == 0);

  struct sembuf failed_pair[2] = {{0, -1, 0}, {1, -1, IPC_NOWAIT}};
  errno = 0;
  CHECK(semop(id, failed_pair, 2) == -1 && errno == EAGAIN);
  CHECK(semctl(id, 0, GETVAL) == 2);
  struct sembuf duplicate[3] = {{0, -1, 0}, {0, -1, 0}, {1, 2, 0}};
  CHECK(semop(id, duplicate, 3) == 0);
  CHECK(semctl(id, 0, GETVAL) == 0 && semctl(id, 1, GETVAL) == 2);
  struct sembuf out_of_range = {3, 1, IPC_NOWAIT};
  errno = 0;
  CHECK(semop(id, &out_of_range, 1) == -1 && errno == EFBIG);
  errno = 0;
  CHECK(semop(id, duplicate, 0) == -1 && errno == EINVAL);
  struct sembuf zero = {0, 0, 0};
  CHECK(semop(id, &zero, 1) == 0);
  CHECK(semctl(id, 0, IPC_STAT, (union test_semun){.buf = &metadata}) == 0 &&
        metadata.sem_otime > 0);
  metadata.sem_perm.mode = 0640;
  CHECK(semctl(id, 0, IPC_SET, (union test_semun){.buf = &metadata}) == 0);
  CHECK(semctl(id, 0, IPC_STAT, (union test_semun){.buf = &metadata}) == 0 &&
        (metadata.sem_perm.mode & 0777) == 0640);

  struct timespec duration = {0, 20000000};
  const struct timespec original = duration;
  struct sembuf timed = {2, -2, 0};
  errno = 0;
  CHECK(semtimedop(id, &timed, 1, &duration) == -1 && errno == EAGAIN);
  CHECK(!memcmp(&duration, &original, sizeof(duration)) && semctl(id, 2, GETVAL) == 1);
  duration.tv_nsec = 1000000000;
  errno = 0;
  CHECK(semtimedop(id, &timed, 1, &duration) == -1 && errno == EINVAL);
  duration = (struct timespec){0, 0};
  errno = 0;
  CHECK(semtimedop(id, &timed, 1, &duration) == -1 && errno == EAGAIN);

  CHECK(waiter_contract(id, 0, 0, 0));
  CHECK(semctl(id, 0, SETVAL, (union test_semun){.val = 1}) == 0);
  CHECK(waiter_contract(id, 1, 0, 0));
  CHECK(waiter_contract(id, 0, EINTR, 0));
  CHECK(semctl(id, 0, SETVAL, (union test_semun){.val = 1}) == 0);
  CHECK(undo_contracts(id));

  if (!geteuid()) {
    pid_t child = fork();
    CHECK(child >= 0);
    if (!child) {
      alarm(5);
      if (setgid(65534) || setuid(65534))
        _exit(16);
      errno = 0;
      if (semctl(id, 0, GETVAL) != -1 || errno != EACCES)
        _exit(17);
      errno = 0;
      _exit(!(semctl(id, 0, IPC_RMID) == -1 && errno == EPERM));
    }
    CHECK(reap(child));
  }
  CHECK(semctl(id, 0, SETVAL, (union test_semun){.val = 0}) == 0);
  CHECK(waiter_contract(id, 0, EIDRM, 1));
  errno = 0;
  CHECK(semctl(id, 0, GETVAL) == -1 && (errno == EINVAL || errno == EIDRM));
  id = -1;
  puts("IPC-SEM: values, atomic vectors, waits, metadata, removal and undo passed");
  return 0;

failed:
  if (keyed >= 0)
    semctl(keyed, 0, IPC_RMID);
  if (id >= 0)
    semctl(id, 0, IPC_RMID);
  return -1;
}
