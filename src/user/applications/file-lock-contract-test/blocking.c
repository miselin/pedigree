#define _GNU_SOURCE
#include <poll.h>
#include <pthread.h>
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/socket.h>

struct lock_wait {
  int fd, kind, probe, probe_error, result, error;
  volatile int entered, done;
  int64_t started, finished;
};
static volatile sig_atomic_t signal_count;
static void caught(int signal) {
  (void)signal;
  __atomic_add_fetch(&signal_count, 1, __ATOMIC_RELAXED);
}
static void finished(void* argument) {
  __atomic_store_n((volatile int*)argument, 1, __ATOMIC_RELEASE);
}
static void* lock_wait(void* argument) {
  struct lock_wait* wait = argument;
  pthread_cleanup_push(finished, (void*)&wait->done);
  wait->started = fl_now();
  wait->probe = fl_lock(wait->fd, wait->kind, F_WRLCK, 0);
  wait->probe_error = errno;
  if (wait->probe == -1 && wait->probe_error == EAGAIN) {
    __atomic_store_n(&wait->entered, 1, __ATOMIC_RELEASE);
    wait->result = fl_lock(wait->fd, wait->kind, F_WRLCK, 1);
    wait->error = errno;
  }
  wait->finished = fl_now();
  pthread_cleanup_pop(1);
  return NULL;
}
static void diagnostic(const struct lock_wait* wait, int action, int64_t acted) {
  const int entered = __atomic_load_n(&wait->entered, __ATOMIC_ACQUIRE);
  const int done = __atomic_load_n(&wait->done, __ATOMIC_ACQUIRE);
  fprintf(stderr,
          "FILE-LOCK-CONTRACT: wait kind=%d action=%d entered=%d done=%d "
          "probe=%d/%d result=%d/%d start=%lld action_time=%lld finish=%lld now=%lld\n",
          wait->kind, action, entered, done, entered || done ? wait->probe : -2,
          entered || done ? wait->probe_error : -2, done ? wait->result : -2,
          done ? wait->error : -2, entered || done ? (long long)wait->started : -1,
          (long long)acted, done ? (long long)wait->finished : -1, (long long)fl_now());
}
static pid_t holder(const char* path, int inherited, int kind, int* control) {
  int pair[2];
  if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair))
    return -1;
  pid_t child = fork();
  if (!child) {
    alarm(10);
    close(pair[0]);
    close(inherited);
    int fd = open(path, O_RDWR);
    if (fd < 0 || fl_lock(fd, kind, F_WRLCK, 0) || write(pair[1], "h", 1) != 1 ||
        fl_byte(pair[1], 'x'))
      _exit(10);
    _exit(fl_lock(fd, kind, F_UNLCK, 0) ? 11 : 0);
  }
  close(pair[1]);
  if (child < 0) {
    close(pair[0]);
    return -1;
  }
  *control = pair[0];
  return child;
}

enum { RELEASE, INTERRUPT, CANCEL, REPLACE };
static int blocking_case(int kind, int action) {
  int failed = 0, fd = -1, other = -1, probe = -1, control = -1, running = 0;
  pid_t child = -1;
  pthread_t worker;
  struct lock_wait wait = {.kind = kind, .result = -2, .error = -2};
  char path[128] = {0}, other_path[128] = {0};
  int64_t acted = -1;
  CHECK((fd = fl_file(path, "/tmp")) >= 0 && (other = fl_file(other_path, "/tmp")) >= 0);
  CHECK((child = holder(path, fd, kind, &control)) > 0 && fl_byte(control, 'h') == 0);
  CHECK(fcntl(fd, F_SETFL, fcntl(fd, F_GETFL, 0) | O_NONBLOCK) == 0);
  wait.fd = fd;
  CHECK(pthread_create(&worker, NULL, lock_wait, &wait) == 0);
  running = 1;
  CHECK(fl_wait(&wait.entered, 1000) == 0);
  CHECK(__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE) == 0);
  if (action == REPLACE) {
    // Public contention is observable, but syscall enrollment is not. Retain
    // timing evidence if this scheduling interval proves insufficient.
    fl_pause(100);
    CHECK(__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE) == 0);
    acted = fl_now();
    CHECK(dup2(other, fd) == fd);
    if (kind == FL_CLASSIC) {
      CHECK(fl_wait(&wait.done, 2000) == 0 && wait.result == -1 && wait.error == EBADF);
    } else {
      CHECK(__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE) == 0);
    }
  } else if (action == INTERRUPT) {
    acted = fl_now();
    const int64_t until = acted + 2000000000;
    while (!__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE) && fl_now() < until) {
      CHECK(pthread_kill(worker, SIGUSR1) == 0);
      fl_pause(10);
    }
    CHECK(fl_wait(&wait.done, 100) == 0 && wait.result == -1 && wait.error == EINTR);
    CHECK(fl_lock(fd, kind, F_WRLCK, 0) == -1 && errno == EAGAIN);
  } else if (action == CANCEL) {
    acted = fl_now();
    CHECK(pthread_cancel(worker) == 0 && fl_wait(&wait.done, 2000) == 0);
    void* result;
    CHECK(pthread_join(worker, &result) == 0 && result == PTHREAD_CANCELED);
    running = 0;
  }
  if (acted < 0)
    acted = fl_now();
  CHECK(write(control, "x", 1) == 1 && fl_reap(child, 2000) == 0);
  child = -1;
  if (running) {
    CHECK(fl_wait(&wait.done, 2000) == 0);
    if (action == RELEASE || (action == REPLACE && kind != FL_CLASSIC))
      CHECK(wait.result == 0);
    CHECK(pthread_join(worker, NULL) == 0);
    running = 0;
  }
  if (action == REPLACE) {
    CHECK((probe = open(path, O_RDWR)) >= 0);
    CHECK(fl_lock(probe, kind == FL_CLASSIC ? FL_OFD : kind, F_WRLCK, 0) == 0);
    close(probe);
    probe = -1;
    CHECK((probe = open(other_path, O_RDWR)) >= 0);
    CHECK(fl_lock(probe, kind == FL_CLASSIC ? FL_OFD : kind, F_WRLCK, 0) == 0);
  } else {
    CHECK(fl_lock(fd, kind, F_WRLCK, 0) == 0);
  }
out:
  if (failed)
    diagnostic(&wait, action, acted);
  if (child > 0) {
    kill(child, SIGKILL);
    fl_reap(child, 1000);
  }
  if (running) {
    if (fl_wait(&wait.done, 2000))
      _exit(1);
    pthread_join(worker, NULL);
  }
  if (control >= 0)
    close(control);
  if (probe >= 0)
    close(probe);
  if (other >= 0)
    close(other);
  if (fd >= 0)
    close(fd);
  if (*path)
    unlink(path);
  if (*other_path)
    unlink(other_path);
  return failed;
}

static int deadlock(void) {
  int failed = 0, a = -1, b = -1, pair[2] = {-1, -1}, running = 0;
  pid_t child = -1;
  char path[128] = {0}, second[128] = {0};
  pthread_t worker;
  struct lock_wait wait = {.kind = FL_CLASSIC, .result = -2, .error = -2};
  CHECK((a = fl_file(path, "/tmp")) >= 0 && (b = fl_file(second, "/tmp")) >= 0);
  CHECK(fl_lock(a, FL_CLASSIC, F_WRLCK, 0) == 0);
  CHECK(socketpair(AF_UNIX, SOCK_STREAM, 0, pair) == 0 && (child = fork()) >= 0);
  if (!child) {
    alarm(8);
    close(pair[0]);
    if (fl_lock(b, FL_CLASSIC, F_WRLCK, 0) || fl_lock(a, FL_CLASSIC, F_WRLCK, 0) != -1 ||
        errno != EAGAIN || write(pair[1], "c", 1) != 1)
      _exit(10);
    int result = fl_lock(a, FL_CLASSIC, F_WRLCK, 1), error = errno;
    char report = result == 0 ? 'a' : error == EDEADLK ? 'd' : 'e';
    if (fl_lock(b, FL_CLASSIC, F_UNLCK, 0) || write(pair[1], &report, 1) != 1)
      _exit(11);
    _exit(report == 'e' ? 12 : 0);
  }
  close(pair[1]);
  pair[1] = -1;
  CHECK(fl_byte(pair[0], 'c') == 0);
  wait.fd = b;
  CHECK(pthread_create(&worker, NULL, lock_wait, &wait) == 0);
  running = 1;
  CHECK(fl_wait(&wait.entered, 1000) == 0);
  const int64_t until = fl_now() + 4000000000;
  while (!__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE)) {
    struct pollfd ready = {.fd = pair[0], .events = POLLIN};
    if (poll(&ready, 1, 0) > 0)
      break;
    CHECK(fl_now() < until);
    fl_pause(2);
  }
  const int parent_deadlock =
      __atomic_load_n(&wait.done, __ATOMIC_ACQUIRE) && wait.result == -1 && wait.error == EDEADLK;
  if (parent_deadlock)
    CHECK(fl_lock(a, FL_CLASSIC, F_UNLCK, 0) == 0);
  CHECK(fl_byte(pair[0], parent_deadlock ? 'a' : 'd') == 0);
  CHECK(fl_wait(&wait.done, 2000) == 0 &&
        (parent_deadlock ? wait.result == -1 && wait.error == EDEADLK : wait.result == 0));
  CHECK(pthread_join(worker, NULL) == 0);
  running = 0;
  CHECK(fl_reap(child, 2000) == 0);
  child = -1;
out:
  if (failed)
    diagnostic(&wait, 4, -1);
  if (a >= 0)
    fl_lock(a, FL_CLASSIC, F_UNLCK, 0);
  if (b >= 0)
    fl_lock(b, FL_CLASSIC, F_UNLCK, 0);
  if (child > 0) {
    kill(child, SIGKILL);
    fl_reap(child, 1000);
  }
  if (running) {
    if (fl_wait(&wait.done, 2000))
      _exit(1);
    pthread_join(worker, NULL);
  }
  for (int n = 0; n < 2; ++n)
    if (pair[n] >= 0)
      close(pair[n]);
  if (b >= 0)
    close(b);
  if (a >= 0)
    close(a);
  if (*path)
    unlink(path);
  if (*second)
    unlink(second);
  return failed;
}
int file_lock_blocking(void) {
  int failed = 0, mask_changed = 0;
  struct sigaction action = {.sa_handler = caught}, original;
  sigset_t unblocked, previous;
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGUSR1, &action, &original) == 0);
  sigemptyset(&unblocked);
  sigaddset(&unblocked, SIGUSR1);
  if (pthread_sigmask(SIG_UNBLOCK, &unblocked, &previous)) {
    failed = 1;
    goto restore;
  }
  mask_changed = 1;
  for (int kind = 0; kind < 3; ++kind) {
    if (blocking_case(kind, RELEASE) || blocking_case(kind, INTERRUPT) ||
        blocking_case(kind, REPLACE)) {
      failed = 1;
      goto restore;
    }
  }
  if (!__atomic_load_n(&signal_count, __ATOMIC_RELAXED) || blocking_case(FL_CLASSIC, CANCEL) ||
      deadlock())
    failed = 1;
restore:
  if (sigaction(SIGUSR1, &original, NULL))
    failed = 1;
out:
  if (mask_changed && pthread_sigmask(SIG_SETMASK, &previous, NULL))
    failed = 1;
  return failed;
}
