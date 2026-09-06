/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/wait.h>

#define CHECK(x)                                                                     \
  do {                                                                               \
    if (!(x)) {                                                                      \
      fprintf(stderr, "SESSION-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      return 1;                                                                      \
    }                                                                                \
  } while (0)

static int finish(pid_t child) {
  int status;
  return waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static int inheritance(void) {
  pid_t sid = getsid(0), pgid = getpgrp();
  CHECK(sid > 0 && pgid > 0 && getsid(getpid()) == sid && getpgid(getpid()) == pgid);
  int ready[2], release[2];
  CHECK(pipe(ready) == 0 && pipe(release) == 0);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(ready[0]);
    close(release[1]);
    char byte = 'x';
    if (getsid(0) != sid || getpgrp() != pgid || getpgid(getppid()) != pgid ||
        write(ready[1], &byte, 1) != 1 || read(release[0], &byte, 1) != 1)
      _exit(1);
    _exit(0);
  }
  close(ready[1]);
  close(release[0]);
  char byte;
  CHECK(read(ready[0], &byte, 1) == 1);
  CHECK(getsid(child) == sid && getpgid(child) == pgid);
  CHECK(setpgid(child, child) == 0 && getpgid(child) == child && getsid(child) == sid);
  CHECK(write(release[1], &byte, 1) == 1 && finish(child));
  close(ready[0]);
  close(ready[1]);
  close(release[0]);
  close(release[1]);
  errno = 0;
  CHECK(getsid(-1) == -1 && errno == ESRCH);
  errno = 0;
  CHECK(getsid(child) == -1 && errno == ESRCH);
  puts("SESSION-CONTRACT: PASS inheritance");
  return 0;
}

static int independent_session(void) {
  pid_t oldGroup = getpgrp();
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    pid_t me = getpid();
    if (setsid() != me || getsid(0) != me || getpgrp() != me)
      _exit(1);
    errno = 0;
    if (setsid() != -1 || errno != EPERM)
      _exit(2);
    errno = 0;
    if (setpgid(0, 0) != -1 || errno != EPERM)
      _exit(3);
    pid_t member = fork();
    if (member < 0)
      _exit(4);
    if (!member) {
      if (getsid(0) != me || getpgrp() != me)
        _exit(5);
      errno = 0;
      if (setpgid(0, oldGroup) != -1 || errno != EPERM)
        _exit(6);
      if (setpgid(0, 0) || getpgrp() != getpid() || getsid(0) != me)
        _exit(7);
      _exit(0);
    }
    _exit(finish(member) ? 0 : 8);
  }
  CHECK(finish(child));
  puts("SESSION-CONTRACT: PASS independent-session");
  return 0;
}

static int leader_exit(void) {
  int ready[2], release[2];
  CHECK(pipe(ready) == 0 && pipe(release) == 0);
  pid_t leader = fork();
  CHECK(leader >= 0);
  if (!leader) {
    close(ready[0]);
    close(release[1]);
    if (setsid() != getpid())
      _exit(1);
    pid_t member = fork();
    if (member < 0)
      _exit(2);
    if (!member) {
      pid_t me = getpid();
      char byte;
      if (write(ready[1], &me, sizeof(me)) != sizeof(me) || read(release[0], &byte, 1) != 1)
        _exit(3);
      _exit(0);
    }
    _exit(0);
  }
  close(ready[1]);
  close(release[0]);
  pid_t member;
  CHECK(read(ready[0], &member, sizeof(member)) == sizeof(member));
  CHECK(finish(leader));
  CHECK(getsid(member) == leader && getpgid(member) == leader);
  char byte = 'x';
  CHECK(write(release[1], &byte, 1) == 1);
  close(ready[0]);
  close(ready[1]);
  close(release[0]);
  close(release[1]);
  puts("SESSION-CONTRACT: PASS leader-exit");
  return 0;
}

static int exec_boundary(const char* program) {
  int ready[2], release[2];
  CHECK(pipe(ready) == 0 && pipe(release) == 0);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(ready[0]);
    close(release[1]);
    char send[24], receive[24];
    snprintf(send, sizeof(send), "%d", ready[1]);
    snprintf(receive, sizeof(receive), "%d", release[0]);
    execl(program, program, "--exec-child", send, receive, (char*)0);
    _exit(127);
  }
  close(ready[1]);
  close(release[0]);
  char byte;
  CHECK(read(ready[0], &byte, 1) == 1);
  errno = 0;
  CHECK(setpgid(child, child) == -1 && errno == EACCES);
  CHECK(write(release[1], &byte, 1) == 1 && finish(child));
  close(ready[0]);
  close(ready[1]);
  close(release[0]);
  close(release[1]);
  puts("SESSION-CONTRACT: PASS exec-boundary");
  return 0;
}

static int direct_child_only(void) {
  int ready[2], release[2];
  CHECK(pipe(ready) == 0 && pipe(release) == 0);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(ready[0]);
    close(release[1]);
    pid_t grandchild = fork();
    if (grandchild < 0)
      _exit(1);
    if (!grandchild) {
      pid_t me = getpid();
      char byte;
      if (write(ready[1], &me, sizeof(me)) != sizeof(me) || read(release[0], &byte, 1) != 1)
        _exit(2);
      _exit(0);
    }
    _exit(finish(grandchild) ? 0 : 3);
  }
  close(ready[1]);
  close(release[0]);
  pid_t grandchild;
  CHECK(read(ready[0], &grandchild, sizeof(grandchild)) == sizeof(grandchild));
  errno = 0;
  CHECK(setpgid(grandchild, grandchild) == -1 && errno == ESRCH);
  char byte = 'x';
  CHECK(write(release[1], &byte, 1) == 1 && finish(child));
  close(ready[0]);
  close(ready[1]);
  close(release[0]);
  close(release[1]);
  puts("SESSION-CONTRACT: PASS direct-child-only");
  return 0;
}

#ifndef __APPLE__
static void* thread_identity(void* unused) {
  (void)unused;
  const pid_t tid = syscall(SYS_gettid);
  if (tid == getpid() || getsid(tid) != getsid(0) || getpgid(tid) != getpgrp())
    return (void*)1;
  errno = 0;
  return setpgid(tid, 0) == -1 && errno == EINVAL ? NULL : (void*)2;
}
#endif

static int task_identity(void) {
#ifdef __APPLE__
  puts("SESSION-CONTRACT: SKIP Linux task IDs on native host");
#else
  pthread_t thread;
  void* result;
  CHECK(pthread_create(&thread, NULL, thread_identity, NULL) == 0);
  CHECK(pthread_join(thread, &result) == 0 && result == NULL);
  puts("SESSION-CONTRACT: PASS task-identity");
#endif
  return 0;
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(45);
  if (argc == 4 && !strcmp(argv[1], "--exec-child")) {
    char byte = 'x';
    return write(atoi(argv[2]), &byte, 1) != 1 || read(atoi(argv[3]), &byte, 1) != 1;
  }
  const int portable = argc == 2 && !strcmp(argv[1], "--portable-host");
  if (portable)
    puts("SESSION-CONTRACT: SKIP Linux direct-child-only rule on native host");
  if (inheritance() || independent_session() || leader_exit() || exec_boundary(argv[0]) ||
      (!portable && direct_child_only()) || task_identity())
    return 1;
  puts("SESSION-CONTRACT: END PASS");
  return 0;
}
