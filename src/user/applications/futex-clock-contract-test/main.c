/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <limits.h>
#include <pthread.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/wait.h>

enum { Wait = 128, Wake = 129, Requeue = 131, Bitset = 137, Realtime = 256 };
#define CHECK(x)                                                                        \
  do {                                                                                  \
    if (!(x)) {                                                                         \
      fprintf(stderr, "FUTEX-CLOCK: line=%d errno=%d check=%s\n", __LINE__, errno, #x); \
      return -1;                                                                        \
    }                                                                                   \
  } while (0)

static int64_t now(clockid_t clock) {
  struct timespec value;
  return syscall(SYS_clock_gettime, clock, &value)
             ? -1
             : (int64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}
static struct timespec timespec(int64_t value) {
  return (struct timespec){value / 1000000000, value % 1000000000};
}
static void pause_ms(int milliseconds) {
  struct timespec duration = timespec((int64_t)milliseconds * 1000000);
  while (nanosleep(&duration, &duration) && errno == EINTR) {
  }
}
static int step(int64_t delta) {
  struct timespec target = timespec(now(CLOCK_REALTIME) + delta);
  return clock_settime(CLOCK_REALTIME, &target);
}
static int wake(int* address) {
  return syscall(SYS_futex, address, Wake, INT_MAX, NULL, NULL, 0);
}
static int requeue(int* source, int* destination) {
  return syscall(SYS_futex, source, Requeue, 0, 1, destination, 0);
}

struct waiter {
  int* address;
  int operation, timed, result, error;
  struct timespec deadline;
  int64_t started, finished;
  volatile int done;
};
static void* wait_thread(void* argument) {
  struct waiter* wait = argument;
  wait->started = now(CLOCK_MONOTONIC);
  wait->result = syscall(SYS_futex, wait->address, wait->operation, 0,
                         wait->timed ? &wait->deadline : NULL, NULL, UINT32_MAX);
  wait->error = errno;
  wait->finished = now(CLOCK_MONOTONIC);
  __atomic_store_n(&wait->done, 1, __ATOMIC_RELEASE);
  return NULL;
}
static int completed(struct waiter* wait, int milliseconds) {
  const int64_t until = now(CLOCK_MONOTONIC) + (int64_t)milliseconds * 1000000;
  while (!__atomic_load_n(&wait->done, __ATOMIC_ACQUIRE)) {
    if (now(CLOCK_MONOTONIC) >= until)
      return 0;
    pause_ms(1);
  }
  return 1;
}
static int enrolled(struct waiter* wait, int* destination) {
  const int64_t until = now(CLOCK_MONOTONIC) + 2000000000;
  while (!__atomic_load_n(&wait->done, __ATOMIC_ACQUIRE) && now(CLOCK_MONOTONIC) < until) {
    int moved = requeue(wait->address, destination);
    if (moved)
      return moved == 1;
    pause_ms(1);
  }
  return 0;
}

static int validation(void) {
  int word = 0;
  struct timespec past = {0}, invalid = {0, 1000000000};
  CHECK(syscall(SYS_futex, &word, Bitset | Realtime, 1, &past, NULL, UINT32_MAX) == -1 &&
        errno == EAGAIN);
  CHECK(syscall(SYS_futex, &word, Bitset | Realtime, 0, &past, NULL, UINT32_MAX) == -1 &&
        errno == ETIMEDOUT);
  CHECK(syscall(SYS_futex, &word, Bitset, 0, &past, NULL, UINT32_MAX) == -1 && errno == ETIMEDOUT);
  CHECK(syscall(SYS_futex, &word, Wait, 0, &past, NULL, 0) == -1 && errno == ETIMEDOUT);
  CHECK(syscall(SYS_futex, &word, Bitset, 0, &invalid, NULL, UINT32_MAX) == -1 && errno == EINVAL);
  invalid = (struct timespec){-1, 0};
  CHECK(syscall(SYS_futex, &word, Bitset, 0, &invalid, NULL, UINT32_MAX) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_futex, &word, Bitset, 0, (void*)-1, NULL, UINT32_MAX) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_futex, (char*)&word + 1, Bitset, 0, NULL, NULL, UINT32_MAX) == -1 &&
        errno == EINVAL);
  CHECK(syscall(SYS_futex, &word, Bitset, 0, NULL, NULL, 0) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_futex, &word, Bitset, 0, NULL, NULL, 1) == -1 && errno == ENOSYS);
  CHECK(syscall(SYS_futex, &word, Wake | Realtime, 1, NULL, NULL, 0) == -1 && errno == ENOSYS);
  CHECK(wake(&word) == 0);
  return 0;
}

static int ordinary_and_indefinite(void) {
  int word = 0;
  const int operations[] = {Wait, Bitset, Bitset | Realtime};
  for (unsigned n = 0; n < sizeof(operations) / sizeof(operations[0]); ++n) {
    pthread_t thread;
    struct waiter wait = {.address = &word, .operation = operations[n]};
    CHECK(pthread_create(&thread, NULL, wait_thread, &wait) == 0);
    CHECK(enrolled(&wait, &word));
    CHECK(wake(&word) == 1 && completed(&wait, 2000));
    CHECK(pthread_join(thread, NULL) == 0 && wait.result == 0);
    CHECK(wake(&word) == 0);
  }
  return 0;
}

static int deadlines(void) {
  int word = 0;
  for (int realtime = 0; realtime <= 1; ++realtime) {
    struct waiter wait = {
        .address = &word, .operation = Bitset | (realtime ? Realtime : 0), .timed = 1};
    const int64_t before = now(CLOCK_MONOTONIC);
    wait.deadline = timespec(now(realtime ? CLOCK_REALTIME : CLOCK_MONOTONIC) + 200000000);
    pthread_t thread;
    CHECK(pthread_create(&thread, NULL, wait_thread, &wait) == 0);
    CHECK(completed(&wait, 3000) && pthread_join(thread, NULL) == 0);
    CHECK(wait.result == -1 && wait.error == ETIMEDOUT && wait.finished - before >= 200000000);
    CHECK(wake(&word) == 0);
  }
  return 0;
}

static int clock_steps(void) {
  int word = 0;
  pthread_t thread;
  struct waiter wait = {.address = &word, .operation = Bitset | Realtime, .timed = 1};
  wait.deadline = timespec(now(CLOCK_REALTIME) + 10000000000);
  CHECK(pthread_create(&thread, NULL, wait_thread, &wait) == 0 && enrolled(&wait, &word));
  CHECK(step(20000000000) == 0);
  CHECK(completed(&wait, 2000) && pthread_join(thread, NULL) == 0);
  CHECK(wait.result == -1 && wait.error == ETIMEDOUT && wake(&word) == 0);

  wait = (struct waiter){.address = &word, .operation = Bitset | Realtime, .timed = 1};
  wait.deadline = timespec(now(CLOCK_REALTIME) + 1000000000);
  CHECK(pthread_create(&thread, NULL, wait_thread, &wait) == 0 && enrolled(&wait, &word));
  CHECK(step(-5000000000) == 0);
  pause_ms(1500);
  CHECK(!__atomic_load_n(&wait.done, __ATOMIC_ACQUIRE));
  CHECK(step(10000000000) == 0);
  CHECK(completed(&wait, 2000) && pthread_join(thread, NULL) == 0);
  CHECK(wait.result == -1 && wait.error == ETIMEDOUT && wake(&word) == 0);
  return 0;
}

static int requeue_clock_change(void) {
  int source = 0, destination = 0;
  pthread_t threads[2];
  struct waiter waits[2] = {{.address = &source, .operation = Wait},
                            {.address = &source, .operation = Bitset | Realtime, .timed = 1}};
  waits[1].deadline = timespec(now(CLOCK_REALTIME) + 10000000000);
  for (int n = 0; n < 2; ++n) {
    CHECK(pthread_create(&threads[n], NULL, wait_thread, &waits[n]) == 0);
    CHECK(enrolled(&waits[n], &destination));
  }
  __atomic_store_n(&source, 1, __ATOMIC_RELEASE);
  CHECK(step(-1000000000) == 0);
  pause_ms(10);
  CHECK(wake(&source) == 0 && wake(&destination) == 2);
  for (int n = 0; n < 2; ++n)
    CHECK(completed(&waits[n], 2000) && pthread_join(threads[n], NULL) == 0 &&
          waits[n].result == 0);
  CHECK(wake(&destination) == 0);
  return 0;
}

static volatile sig_atomic_t signals;
static void handler(int signal) {
  (void)signal;
  ++signals;
}
static int interruption(void) {
  int word = 0;
  struct sigaction action = {.sa_handler = handler};
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGUSR1, &action, NULL) == 0);
  for (int n = 0; n < 4; ++n) {
    pthread_t thread;
    struct waiter wait = {.address = &word, .operation = Bitset | Realtime, .timed = 1};
    wait.deadline = timespec(now(CLOCK_REALTIME) + 10000000000);
    CHECK(pthread_create(&thread, NULL, wait_thread, &wait) == 0 && enrolled(&wait, &word));
    CHECK(pthread_kill(thread, SIGUSR1) == 0);
    CHECK(completed(&wait, 2000) && pthread_join(thread, NULL) == 0);
    CHECK(wait.result == -1 && wait.error == EINTR && wake(&word) == 0);
    CHECK(step(1) == 0);
  }
  CHECK(signals == 4);
  return 0;
}

static int timeout_wake_race(void) {
  int word = 0;
  for (int n = 0; n < 24; ++n) {
    pthread_t thread;
    struct waiter wait = {.address = &word, .operation = Bitset, .timed = 1};
    wait.deadline = timespec(now(CLOCK_MONOTONIC) + 10000000);
    CHECK(pthread_create(&thread, NULL, wait_thread, &wait) == 0);
    pause_ms(10);
    const int woken = wake(&word);
    CHECK(completed(&wait, 2000) && pthread_join(thread, NULL) == 0);
    CHECK((woken == 1 && wait.result == 0) ||
          (woken == 0 && wait.result == -1 && wait.error == ETIMEDOUT));
  }
  return 0;
}

static int terminate_wait(void) {
  int ready[2];
  CHECK(pipe(ready) == 0);
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    close(ready[0]);
    alarm(10);
    int word = 0;
    pthread_t thread;
    struct waiter wait = {.address = &word, .operation = Bitset | Realtime, .timed = 1};
    wait.deadline = timespec(now(CLOCK_REALTIME) + 10000000000);
    if (pthread_create(&thread, NULL, wait_thread, &wait) || !enrolled(&wait, &word) ||
        write(ready[1], "r", 1) != 1)
      _exit(1);
    for (;;)
      pause();
  }
  close(ready[1]);
  char token = 0;
  int valid = read(ready[0], &token, 1) == 1 && token == 'r';
  close(ready[0]);
  int status;
  CHECK(kill(child, SIGKILL) == 0 && waitpid(child, &status, 0) == child);
  CHECK(valid && WIFSIGNALED(status) && WTERMSIG(status) == SIGKILL);
  CHECK(step(1) == 0);
  return 0;
}

int main(void) {
  setvbuf(stdout, NULL, _IONBF, 0);
  if (geteuid() != 0) {
    fputs("FUTEX-CLOCK: FAIL requires root for clock-step contracts\n", stderr);
    return 1;
  }
  const struct {
    const char* name;
    int (*test)(void);
  } tests[] = {{"validation", validation},
               {"ordinary-and-indefinite", ordinary_and_indefinite},
               {"deadlines", deadlines},
               {"clock-steps", clock_steps},
               {"requeue-clock-change", requeue_clock_change},
               {"interruption", interruption},
               {"timeout-wake-race", timeout_wake_race},
               {"terminate-wait", terminate_wait}};
  for (unsigned n = 0; n < sizeof(tests) / sizeof(tests[0]); ++n) {
    printf("FUTEX-CLOCK: RUN %s\n", tests[n].name);
    const int64_t realtime = now(CLOCK_REALTIME), monotonic = now(CLOCK_MONOTONIC);
    pid_t child = fork();
    if (!child) {
      alarm(20);
      _exit(tests[n].test() ? 1 : 0);
    }
    int status = -1;
    pid_t reaped;
    do {
      reaped = waitpid(child, &status, 0);
    } while (reaped < 0 && errno == EINTR);
    struct timespec restored = timespec(realtime + now(CLOCK_MONOTONIC) - monotonic);
    int failed = child < 0 || reaped != child || !WIFEXITED(status) || WEXITSTATUS(status);
    if (clock_settime(CLOCK_REALTIME, &restored))
      failed = 1;
    printf("FUTEX-CLOCK: %s %s status=%d\n", failed ? "FAIL" : "PASS", tests[n].name, status);
    if (failed)
      return 1;
  }
  puts("FUTEX-CLOCK: END PASS");
  return 0;
}
