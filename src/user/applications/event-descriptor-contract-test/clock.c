#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/timerfd.h>

static int restore(int64_t realtime, int64_t monotonic) {
  struct timespec target = ed_timespec(realtime + ed_now(CLOCK_MONOTONIC) - monotonic);
  return clock_settime(CLOCK_REALTIME, &target);
}
static int jump(int64_t delta) {
  struct timespec target = ed_timespec(ed_now(CLOCK_REALTIME) + delta);
  return clock_settime(CLOCK_REALTIME, &target);
}
static int64_t remaining(const struct itimerspec* setting) {
  return (int64_t)setting->it_value.tv_sec * 1000000000 + setting->it_value.tv_nsec;
}

static int deadlines(void) {
  int failed = 0, absolute = -1, relative = -1, monotonic_fd = -1, active = 0;
  pthread_t worker;
  struct ed_reader reader = {.size = 8};
  struct itimerspec setting = {0}, snapshot;
  uint64_t count;
  const int64_t realtime = ed_now(CLOCK_REALTIME), monotonic = ed_now(CLOCK_MONOTONIC);
  CHECK((absolute = timerfd_create(CLOCK_REALTIME, 0)) >= 0);
  CHECK((relative = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK)) >= 0);
  CHECK((monotonic_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) >= 0);
  setting.it_value = ed_timespec(ed_now(CLOCK_REALTIME) + 5000000000);
  CHECK(timerfd_settime(absolute, TFD_TIMER_ABSTIME, &setting, NULL) == 0);
  setting.it_value = ed_timespec(3000000000);
  CHECK(timerfd_settime(relative, TFD_TIMER_CANCEL_ON_SET, &setting, NULL) == 0);
  setting.it_value = ed_timespec(ed_now(CLOCK_MONOTONIC) + 3000000000);
  CHECK(timerfd_settime(monotonic_fd, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &setting,
                        NULL) == 0);
  reader.fd = absolute;
  CHECK(pthread_create(&worker, NULL, ed_read_thread, &reader) == 0);
  active = 1;
  CHECK(ed_wait(&reader.started, 1000) == 0);
  ed_pause(30);
  CHECK(!__atomic_load_n(&reader.done, __ATOMIC_ACQUIRE));
  const int64_t jumped = ed_now(CLOCK_MONOTONIC);
  CHECK(jump(10000000000) == 0);
  CHECK(ed_wait(&reader.done, 1000) == 0 && pthread_join(worker, NULL) == 0);
  active = 0;
  memcpy(&count, reader.bytes, sizeof(count));
  CHECK(reader.result == 8 && count == 1 && reader.finished - jumped >= 0 &&
        reader.finished - jumped < 500000000);
  CHECK(timerfd_gettime(relative, &snapshot) == 0 && remaining(&snapshot) > 1000000000);
  CHECK(timerfd_gettime(monotonic_fd, &snapshot) == 0 && remaining(&snapshot) > 1000000000);
  CHECK(read(relative, &count, 8) == -1 && errno == EAGAIN);
  CHECK(read(monotonic_fd, &count, 8) == -1 && errno == EAGAIN);

  CHECK(fcntl(absolute, F_SETFL, O_NONBLOCK) == 0);
  setting.it_value = ed_timespec(ed_now(CLOCK_REALTIME) + 300000000);
  CHECK(timerfd_settime(absolute, TFD_TIMER_ABSTIME, &setting, NULL) == 0);
  CHECK(jump(-1000000000) == 0);
  ed_pause(400);
  CHECK(read(absolute, &count, 8) == -1 && errno == EAGAIN);
  CHECK(jump(1000000000) == 0);
  CHECK(ed_readable(absolute, 1000) == 1 && read(absolute, &count, 8) == 8 && count == 1);
out:
  if (absolute >= 0)
    close(absolute);
  if (active) {
    pthread_cancel(worker);
    pthread_join(worker, NULL);
  }
  if (relative >= 0)
    close(relative);
  if (monotonic_fd >= 0)
    close(monotonic_fd);
  if (restore(realtime, monotonic))
    failed = 1;
  return failed;
}

static int cancellation(void) {
  int failed = 0, fd = -1, alias = -1, active = 0;
  pthread_t worker;
  struct ed_reader reader = {.size = 8};
  const int flags = TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET;
  struct itimerspec setting = {0}, snapshot;
  uint64_t count;
  const int64_t realtime = ed_now(CLOCK_REALTIME), monotonic = ed_now(CLOCK_MONOTONIC);
  CHECK((fd = timerfd_create(CLOCK_REALTIME, 0)) >= 0 && (alias = dup(fd)) >= 0);
  setting.it_value = ed_timespec(ed_now(CLOCK_REALTIME) + 5000000000);
  CHECK(timerfd_settime(fd, flags, &setting, NULL) == 0);
  reader.fd = fd;
  CHECK(pthread_create(&worker, NULL, ed_read_thread, &reader) == 0);
  active = 1;
  CHECK(ed_wait(&reader.started, 1000) == 0);
  ed_pause(30);
  CHECK(!__atomic_load_n(&reader.done, __ATOMIC_ACQUIRE));
  const int64_t jumped = ed_now(CLOCK_MONOTONIC);
  CHECK(jump(1000000000) == 0);
  CHECK(ed_wait(&reader.done, 1000) == 0 && pthread_join(worker, NULL) == 0);
  active = 0;
  CHECK(reader.result == -1 && reader.error == ECANCELED && reader.finished - jumped >= 0 &&
        reader.finished - jumped < 500000000);
  CHECK(fcntl(alias, F_SETFL, O_NONBLOCK) == 0);
  CHECK(read(alias, &count, 8) == -1 && errno == EAGAIN);

  setting.it_value = ed_timespec(ed_now(CLOCK_REALTIME) + 5000000000);
  CHECK(timerfd_settime(alias, flags, &setting, NULL) == 0);
  CHECK(jump(-1000000000) == 0 && ed_readable(fd, 0) == 1);
  setting.it_value = ed_timespec(ed_now(CLOCK_REALTIME) + 300000000);
  CHECK(timerfd_settime(fd, flags, &setting, NULL) == -1 && errno == ECANCELED);
  CHECK(timerfd_gettime(alias, &snapshot) == 0 && remaining(&snapshot) > 0);
  CHECK(ed_readable(alias, 2000) == 1 && read(alias, &count, 8) == 8 && count == 1);
  CHECK(read(fd, &count, 8) == -1 && errno == EAGAIN);

  setting.it_value = (struct timespec){0};
  setting.it_interval = ed_timespec(77000000);
  CHECK(timerfd_settime(alias, flags, &setting, NULL) == 0);
  CHECK(timerfd_gettime(fd, &snapshot) == 0 && remaining(&snapshot) == 0 &&
        snapshot.it_interval.tv_nsec == 77000000);
  CHECK(jump(1000000000) == 0 && ed_readable(fd, 0) == 1);
  CHECK(read(alias, &count, 8) == -1 && errno == ECANCELED);
  CHECK(read(fd, &count, 8) == -1 && errno == EAGAIN);
out:
  if (fd >= 0)
    close(fd);
  if (alias >= 0)
    close(alias);
  if (active) {
    pthread_cancel(worker);
    pthread_join(worker, NULL);
  }
  if (restore(realtime, monotonic))
    failed = 1;
  return failed;
}

int event_descriptor_test_clock(void) {
  if (geteuid() != 0) {
    puts("EVENT-DESCRIPTOR-CONTRACT: SKIP clock requires root");
    return 0;
  }
  return deadlines() || cancellation();
}
