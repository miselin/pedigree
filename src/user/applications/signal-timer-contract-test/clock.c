#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <mqueue.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

struct clock_wait {
  clockid_t clock;
  int flags, result;
  struct timespec request;
  volatile int entered, done;
  int64_t started, finished;
};
static void wait_done(void* argument) {
  __atomic_store_n((volatile int*)argument, 1, __ATOMIC_RELEASE);
}
static void* clock_wait(void* argument) {
  struct clock_wait* waiter = argument;
  pthread_cleanup_push(wait_done, (void*)&waiter->done);
  waiter->started = st_now(CLOCK_MONOTONIC);
  __atomic_store_n(&waiter->entered, 1, __ATOMIC_RELEASE);
  waiter->result = clock_nanosleep(waiter->clock, waiter->flags, &waiter->request, NULL);
  waiter->finished = st_now(CLOCK_MONOTONIC);
  pthread_cleanup_pop(1);
  return NULL;
}
static int restore_clock(int64_t realtime, int64_t monotonic) {
  struct timespec target = st_timespec(realtime + st_now(CLOCK_MONOTONIC) - monotonic);
  return clock_settime(CLOCK_REALTIME, &target);
}

struct mqueue_wait {
  mqd_t queue;
  int sending, result, error;
  struct timespec deadline;
  volatile int entered, done;
  int64_t finished;
};
static void* mqueue_wait(void* argument) {
  struct mqueue_wait* waiter = argument;
  char byte;
  pthread_cleanup_push(wait_done, (void*)&waiter->done);
  __atomic_store_n(&waiter->entered, 1, __ATOMIC_RELEASE);
  waiter->result = waiter->sending
                       ? mq_timedsend(waiter->queue, "x", 1, 1, &waiter->deadline)
                       : mq_timedreceive(waiter->queue, &byte, 1, NULL, &waiter->deadline);
  waiter->error = errno;
  waiter->finished = st_now(CLOCK_MONOTONIC);
  pthread_cleanup_pop(1);
  return NULL;
}
static int mqueue_clock_change(void) {
  int failed = 0, changed = 0, active[2] = {0};
  pthread_t workers[2];
  struct mqueue_wait waits[2] = {{.queue = (mqd_t)-1}, {.queue = (mqd_t)-1, .sending = 1}};
  char names[2][64];
  struct mq_attr attributes = {.mq_maxmsg = 1, .mq_msgsize = 1}, observed;
  const int64_t realtime = st_now(CLOCK_REALTIME), monotonic = st_now(CLOCK_MONOTONIC);
  for (int n = 0; n < 2; ++n)
    snprintf(names[n], sizeof(names[n]), "/clock-contract-%ld-%d", (long)getpid(), n);
  for (int n = 0; n < 2; ++n) {
    waits[n].queue = mq_open(names[n], O_CREAT | O_EXCL | O_RDWR, 0600, &attributes);
    CHECK(waits[n].queue != (mqd_t)-1);
    waits[n].deadline = st_timespec(st_now(CLOCK_REALTIME) + 5000000000);
    if (n == 1)
      CHECK(mq_send(waits[n].queue, "q", 1, 7) == 0);
    CHECK(pthread_create(&workers[n], NULL, mqueue_wait, &waits[n]) == 0);
    active[n] = 1;
    CHECK(st_wait(&waits[n].entered, 1000) == 0);
  }
  st_pause(50);
  CHECK(!__atomic_load_n(&waits[0].done, __ATOMIC_ACQUIRE) &&
        !__atomic_load_n(&waits[1].done, __ATOMIC_ACQUIRE));
  struct timespec target = st_timespec(st_now(CLOCK_REALTIME) + 10000000000);
  const int64_t jumped = st_now(CLOCK_MONOTONIC);
  CHECK(clock_settime(CLOCK_REALTIME, &target) == 0);
  changed = 1;
  for (int n = 0; n < 2; ++n) {
    CHECK(st_wait(&waits[n].done, 500) == 0);
    CHECK(waits[n].result == -1 && waits[n].error == ETIMEDOUT);
    // A one-second polling recheck cannot satisfy this clock-change wake contract.
    CHECK(waits[n].finished - jumped >= 0 && waits[n].finished - jumped < 500000000);
    CHECK(pthread_join(workers[n], NULL) == 0);
    active[n] = 0;
    CHECK(mq_getattr(waits[n].queue, &observed) == 0 && observed.mq_curmsgs == n);
    attributes.mq_flags = O_NONBLOCK;
    CHECK(mq_setattr(waits[n].queue, &attributes, NULL) == 0);
  }
  char byte;
  unsigned priority;
  CHECK(mq_receive(waits[0].queue, &byte, 1, NULL) == -1 && errno == EAGAIN);
  CHECK(mq_receive(waits[1].queue, &byte, 1, &priority) == 1 && byte == 'q' && priority == 7);
  CHECK(mq_receive(waits[1].queue, &byte, 1, NULL) == -1 && errno == EAGAIN);
out:
  if (changed && restore_clock(realtime, monotonic))
    failed = 1;
  for (int n = 0; n < 2; ++n)
    if (waits[n].queue != (mqd_t)-1)
      mq_unlink(names[n]);
  for (int n = 0; n < 2; ++n) {
    if (active[n]) {
      pthread_cancel(workers[n]);
      if (st_wait(&waits[n].done, 1000) == 0)
        pthread_join(workers[n], NULL);
      else
        _exit(1);
    }
    if (waits[n].queue != (mqd_t)-1)
      mq_close(waits[n].queue);
  }
  return failed;
}

int signal_timer_test_clock(void) {
  int failed = 0, changed = 0, active[3] = {0}, timer_live[3] = {0};
  pthread_t workers[3];
  struct clock_wait waits[3] = {0};
  timer_t timers[3];
  struct sigevent event = {.sigev_notify = SIGEV_NONE};
  struct itimerspec setting = {0}, observed;
  const int64_t realtime = st_now(CLOCK_REALTIME), monotonic = st_now(CLOCK_MONOTONIC);
  struct timespec target = st_timespec(realtime), invalid = {0, 1000000000};
  void* bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(realtime > 20000000000 && monotonic >= 0 && bad != MAP_FAILED);
  CHECK(clock_settime(CLOCK_MONOTONIC, &target) == -1 && errno == EINVAL);
  CHECK(clock_settime(CLOCK_REALTIME, &invalid) == -1 && errno == EINVAL);
  CHECK(clock_settime(CLOCK_REALTIME, bad) == -1 && errno == EFAULT);
  if (geteuid() != 0) {
    CHECK(clock_settime(CLOCK_REALTIME, &target) == -1 && errno == EPERM);
    puts("SIGNAL-TIMER-CONTRACT: SKIP clock changes require uid 0");
    goto out;
  }
  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    alarm(5);
    if (setuid(65534))
      _exit(10);
    _exit(clock_settime(CLOCK_REALTIME, &target) == -1 && errno == EPERM ? 0 : 11);
  }
  CHECK(st_reap(child, 6000) == 0);
  CHECK(mqueue_clock_change() == 0);

  int64_t before_real = st_now(CLOCK_REALTIME), before_mono = st_now(CLOCK_MONOTONIC);
  target = st_timespec(before_real + 10000000000);
  CHECK(clock_settime(CLOCK_REALTIME, &target) == 0);
  changed = 1;
  CHECK(st_now(CLOCK_REALTIME) >= before_real + 10000000000 &&
        st_now(CLOCK_REALTIME) < before_real + 12000000000);
  CHECK(st_now(CLOCK_MONOTONIC) >= before_mono &&
        st_now(CLOCK_MONOTONIC) - before_mono < 2000000000);
  target = st_timespec(before_real - 10000000000);
  CHECK(clock_settime(CLOCK_REALTIME, &target) == 0);
  CHECK(st_now(CLOCK_REALTIME) >= before_real - 10000000000 &&
        st_now(CLOCK_REALTIME) < before_real - 8000000000);
  CHECK(restore_clock(realtime, monotonic) == 0);

  // A past deadline in the final signed-nanosecond second must remain past.
  target = (struct timespec){9223372036, 250000000};
  CHECK(clock_settime(CLOCK_REALTIME, &target) == 0);
  struct timespec past = {9223372036, 0};
  before_mono = st_now(CLOCK_MONOTONIC);
  CHECK(clock_nanosleep(CLOCK_REALTIME, TIMER_ABSTIME, &past, NULL) == 0);
  CHECK(st_now(CLOCK_MONOTONIC) - before_mono < 300000000);
  CHECK(restore_clock(realtime, monotonic) == 0);

  waits[0] = (struct clock_wait){.clock = CLOCK_REALTIME,
                                 .flags = TIMER_ABSTIME,
                                 .request = st_timespec(st_now(CLOCK_REALTIME) + 400000000)};
  waits[1] = (struct clock_wait){.clock = CLOCK_MONOTONIC,
                                 .flags = TIMER_ABSTIME,
                                 .request = st_timespec(st_now(CLOCK_MONOTONIC) + 250000000)};
  waits[2] = (struct clock_wait){.clock = CLOCK_REALTIME, .request = {0, 250000000}};
  for (int n = 0; n < 3; ++n) {
    CHECK(pthread_create(&workers[n], NULL, clock_wait, &waits[n]) == 0);
    active[n] = 1;
    CHECK(st_wait(&waits[n].entered, 1000) == 0);
  }
  st_pause(30);
  target = st_timespec(st_now(CLOCK_REALTIME) - 1000000000);
  CHECK(clock_settime(CLOCK_REALTIME, &target) == 0);
  st_pause(500);
  CHECK(__atomic_load_n(&waits[0].done, __ATOMIC_ACQUIRE) == 0);
  for (int n = 1; n < 3; ++n) {
    CHECK(st_wait(&waits[n].done, 1000) == 0 && waits[n].result == 0);
    CHECK(waits[n].finished - waits[n].started >= 200000000 &&
          waits[n].finished - waits[n].started < 2000000000);
  }
  CHECK(restore_clock(realtime, monotonic) == 0);
  CHECK(st_wait(&waits[0].done, 1500) == 0 && waits[0].result == 0);
  for (int n = 0; n < 3; ++n) {
    CHECK(pthread_join(workers[n], NULL) == 0);
    active[n] = 0;
  }

  waits[0] = (struct clock_wait){.clock = CLOCK_REALTIME,
                                 .flags = TIMER_ABSTIME,
                                 .request = st_timespec(st_now(CLOCK_REALTIME) + 5000000000)};
  CHECK(pthread_create(&workers[0], NULL, clock_wait, &waits[0]) == 0);
  active[0] = 1;
  CHECK(st_wait(&waits[0].entered, 1000) == 0);
  for (int n = 0; n < 3; ++n) {
    CHECK(timer_create(n == 2 ? CLOCK_MONOTONIC : CLOCK_REALTIME, &event, &timers[n]) == 0);
    timer_live[n] = 1;
    setting.it_value = n == 0 ? waits[0].request : (struct timespec){3, 0};
    CHECK(timer_settime(timers[n], n == 0 ? TIMER_ABSTIME : 0, &setting, NULL) == 0);
  }
  st_pause(30);
  target = st_timespec(st_now(CLOCK_REALTIME) + 10000000000);
  before_mono = st_now(CLOCK_MONOTONIC);
  CHECK(clock_settime(CLOCK_REALTIME, &target) == 0);
  CHECK(st_wait(&waits[0].done, 1500) == 0 && waits[0].result == 0);
  CHECK(st_now(CLOCK_MONOTONIC) - before_mono < 1500000000);
  CHECK(pthread_join(workers[0], NULL) == 0);
  active[0] = 0;
  CHECK(timer_gettime(timers[0], &observed) == 0 && observed.it_value.tv_sec == 0 &&
        observed.it_value.tv_nsec == 0);
  for (int n = 1; n < 3; ++n)
    CHECK(timer_gettime(timers[n], &observed) == 0 && observed.it_value.tv_sec >= 1);

out:
  if (changed && restore_clock(realtime, monotonic))
    failed = 1;
  for (int n = 0; n < 3; ++n) {
    if (timer_live[n])
      timer_delete(timers[n]);
    if (active[n]) {
      pthread_cancel(workers[n]);
      if (st_wait(&waits[n].done, 1000) == 0)
        pthread_join(workers[n], NULL);
      else
        _exit(1);
    }
  }
  if (bad != MAP_FAILED)
    munmap(bad, 4096);
  return failed;
}
