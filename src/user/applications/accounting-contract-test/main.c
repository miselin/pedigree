/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/acct.h>
#include <sys/stat.h>
#include <sys/wait.h>

#define CHECK(expression)                                                               \
  do {                                                                                  \
    if (!(expression)) {                                                                \
      fprintf(stderr, "ACCOUNTING-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

static int wait_for(pid_t child, int expected) {
  int status = 0;
  while (waitpid(child, &status, 0) == -1) {
    if (errno != EINTR)
      return 0;
  }
  return status == expected;
}

static int record_for(int fd, pid_t pid, struct acct_v3* result) {
  struct stat metadata;
  if (fstat(fd, &metadata) || metadata.st_size < 0 || metadata.st_size % sizeof(*result))
    return -1;
  int found = 0;
  for (off_t offset = 0; offset < metadata.st_size; offset += sizeof(*result)) {
    struct acct_v3 record;
    if (pread(fd, &record, sizeof(record), offset) != sizeof(record) ||
        record.ac_version != (3 | ACCT_BYTEORDER))
      return -1;
    if (record.ac_pid == (unsigned)pid) {
      *result = record;
      ++found;
    }
  }
  return found;
}

static pid_t child_exit(int code) {
  pid_t child = fork();
  if (!child)
    _exit(code);
  return child;
}

static void* thread_exit(void* argument) {
  return argument;
}

int main(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "--exec-child"))
    return 42;
  int failed = 0, first = -1, second = -1;
  char directory[256], first_path[300], second_path[300], missing[300];
  const char* base = argc > 1 ? argv[1] : "/tests";
  snprintf(directory, sizeof(directory), "%s/accounting-contract.XXXXXX", base);
  first_path[0] = second_path[0] = missing[0] = 0;
  alarm(40);
  CHECK(geteuid() == 0 && acct(NULL) == 0);
  CHECK(mkdtemp(directory));
  snprintf(first_path, sizeof(first_path), "%s/first", directory);
  snprintf(second_path, sizeof(second_path), "%s/second", directory);
  snprintf(missing, sizeof(missing), "%s/missing", directory);
  first = open(first_path, O_CREAT | O_EXCL | O_RDWR, 0600);
  second = open(second_path, O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(first >= 0 && second >= 0);
  errno = 0;
  CHECK(acct("") == -1 && errno == ENOENT);
  errno = 0;
  CHECK(acct(directory) == -1 && errno == EACCES);
  errno = 0;
  CHECK(acct(missing) == -1 && errno == ENOENT);
  CHECK(acct(first_path) == 0);
  errno = 0;
  CHECK(acct(missing) == -1 && errno == ENOENT);

  time_t before = time(NULL);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    struct timespec pause = {.tv_nsec = 80000000};
    nanosleep(&pause, NULL);
    volatile unsigned sum = 0;
    for (unsigned i = 0; i < 5000000; ++i)
      sum += i;
    _exit(23);
  }
  CHECK(wait_for(child, 23 << 8));
  struct acct_v3 record;
  CHECK(record_for(first, child, &record) == 1);
  CHECK(record.ac_ppid == (unsigned)getpid() && record.ac_uid == (unsigned)getuid() &&
        record.ac_gid == (unsigned)getgid() && record.ac_exitcode == (23 << 8));
  CHECK((record.ac_flag & AFORK) && !(record.ac_flag & AXSIG) && record.ac_etime >= 5 &&
        record.ac_btime >= (unsigned)before && record.ac_btime <= (unsigned)time(NULL));
  puts("ACCOUNTING-CONTRACT: PASS record-before-wait");

  child = fork();
  CHECK(child >= 0);
  if (!child) {
    struct timespec pause = {.tv_nsec = 80000000};
    nanosleep(&pause, NULL);
    execl("/usr/bin/accounting-contract-test", "accounting-contract-test", "--exec-child", NULL);
    _exit(99);
  }
  CHECK(wait_for(child, 42 << 8));
  CHECK(record_for(first, child, &record) == 1 && !(record.ac_flag & AFORK) &&
        record.ac_exitcode == (42 << 8) && record.ac_etime >= 5 &&
        !strncmp(record.ac_comm, "accounting-cont", 15));
  puts("ACCOUNTING-CONTRACT: PASS exec-lifetime");

  child = fork();
  CHECK(child >= 0);
  if (!child) {
    pthread_t threads[3];
    for (unsigned i = 0; i < 3; ++i)
      if (pthread_create(&threads[i], NULL, thread_exit, NULL))
        _exit(97);
    for (unsigned i = 0; i < 3; ++i)
      if (pthread_join(threads[i], NULL))
        _exit(98);
    if (record_for(first, getpid(), &record) != 0)
      _exit(99);
    _exit(24);
  }
  CHECK(wait_for(child, 24 << 8));
  CHECK(record_for(first, child, &record) == 1 && record.ac_exitcode == (24 << 8));
  puts("ACCOUNTING-CONTRACT: PASS final-process-exit");

  child = fork();
  CHECK(child >= 0);
  if (!child) {
    if (setgid(65534) || setuid(65534))
      _exit(1);
    errno = 0;
    if (acct(NULL) != -1 || errno != EPERM)
      _exit(2);
    errno = 0;
    if (acct(first_path) != -1 || errno != EPERM)
      _exit(3);
    _exit(0);
  }
  CHECK(wait_for(child, 0));
  CHECK(record_for(first, child, &record) == 1 && record.ac_uid == 65534 && record.ac_gid == 65534);
  puts("ACCOUNTING-CONTRACT: PASS credentials");

  child = fork();
  CHECK(child >= 0);
  if (!child) {
    kill(getpid(), SIGTERM);
    _exit(99);
  }
  CHECK(wait_for(child, SIGTERM));
  CHECK(record_for(first, child, &record) == 1 && (record.ac_flag & AXSIG) &&
        record.ac_exitcode == SIGTERM);

  pid_t children[8];
  for (unsigned i = 0; i < 8; ++i) {
    children[i] = child_exit(i);
    CHECK(children[i] > 0);
  }
  for (unsigned i = 0; i < 8; ++i) {
    CHECK(wait_for(children[i], i << 8));
    CHECK(record_for(first, children[i], &record) == 1 && record.ac_exitcode == (i << 8));
  }
  CHECK(unlink(first_path) == 0);
  child = child_exit(17);
  CHECK(child > 0 && wait_for(child, 17 << 8));
  CHECK(record_for(first, child, &record) == 1);
  puts("ACCOUNTING-CONTRACT: PASS concurrent-and-unlinked");

  struct stat old_first, old_second, current;
  CHECK(acct(second_path) == 0 && fstat(first, &old_first) == 0);
  child = child_exit(19);
  CHECK(child > 0 && wait_for(child, 19 << 8));
  CHECK(record_for(second, child, &record) == 1 && record_for(first, child, &record) == 0);
  CHECK(fstat(first, &current) == 0 && current.st_size == old_first.st_size);
  CHECK(acct(NULL) == 0 && fstat(second, &old_second) == 0);
  child = child_exit(21);
  CHECK(child > 0 && wait_for(child, 21 << 8));
  CHECK(fstat(second, &current) == 0 && current.st_size == old_second.st_size);
  CHECK(fsync(second) == 0);
  puts("ACCOUNTING-CONTRACT: PASS switch-and-disable");

out:
  acct(NULL);
  if (first >= 0)
    close(first);
  if (second >= 0)
    close(second);
  if (first_path[0])
    unlink(first_path);
  if (second_path[0])
    unlink(second_path);
  rmdir(directory);
  puts(failed ? "ACCOUNTING-CONTRACT: END FAIL" : "ACCOUNTING-CONTRACT: END PASS");
  return failed;
}
