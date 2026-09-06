#define _GNU_SOURCE
#include <errno.h>
#include <inttypes.h>
#include <limits.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/syscall.h>
#include <sys/timerfd.h>
#include <sys/timex.h>
#include <sys/wait.h>

_Static_assert(sizeof(struct timex) == 208, "Linux amd64 timex ABI");
_Static_assert(offsetof(struct timex, time) == 72, "Linux amd64 timex time ABI");
_Static_assert(offsetof(struct timex, tai) == 160, "Linux amd64 timex TAI ABI");

#define CHECK(expression)                                                              \
  do {                                                                                 \
    if (!(expression)) {                                                               \
      fprintf(stderr, "CLOCK-ADJUST-CONTRACT: line=%d errno=%d %s\n", __LINE__, errno, \
              #expression);                                                            \
      failed = 1;                                                                      \
      goto out;                                                                        \
    }                                                                                  \
  } while (0)

static int64_t now(clockid_t clock) {
  struct timespec value;
  return clock_gettime(clock, &value) ? -1 : (int64_t)value.tv_sec * 1000000000 + value.tv_nsec;
}

static struct timespec timespec(int64_t value) {
  return (struct timespec){value / 1000000000, value % 1000000000};
}

static int state(const struct timex* value) {
  const long units = value->status & STA_NANO ? 1000000000 : 1000000;
  return (value->status & STA_UNSYNC) && !(value->status & (STA_PLL | STA_PPSFREQ | STA_PPSTIME)) &&
         value->time.tv_sec > 0 && value->time.tv_usec >= 0 && value->time.tv_usec < units &&
         value->offset == 0 && value->freq == 0 && value->ppsfreq == 0 && value->maxerror > 0 &&
         value->esterror > 0;
}

static int query(void) {
  int failed = 0;
  struct timex value;
  memset(&value, 0xA5, sizeof(value));
  value.modes = 0;
  const int64_t before = now(CLOCK_REALTIME);
  CHECK(adjtimex(&value) == TIME_ERROR && state(&value));
  const int64_t timestamp = (int64_t)value.time.tv_sec * 1000000000 +
                            value.time.tv_usec * ((value.status & STA_NANO) ? 1 : 1000);
  CHECK(timestamp >= before - 100000000 && timestamp <= now(CLOCK_REALTIME) + 100000000);
  for (unsigned n = 0; n < sizeof(value.__padding) / sizeof(value.__padding[0]); ++n)
    CHECK(value.__padding[n] == 0);
  value = (struct timex){0};
  CHECK(syscall(SYS_clock_adjtime, CLOCK_REALTIME, &value) == TIME_ERROR && state(&value));
  value = (struct timex){.modes = ADJ_OFFSET_SS_READ};
  CHECK(syscall(SYS_adjtimex, &value) == TIME_ERROR && state(&value) && value.offset == 0);
  value = (struct timex){0};
  CHECK(clock_adjtime(CLOCK_MONOTONIC, &value) == -1 && errno == EOPNOTSUPP);
  CHECK(syscall(SYS_clock_adjtime, 123456, &value) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_adjtimex, NULL) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_clock_adjtime, CLOCK_REALTIME, NULL) == -1 && errno == EFAULT);
out:
  return failed;
}

static int step(int64_t seconds, int64_t fraction, unsigned modes, int route) {
  struct timex value = {.modes = ADJ_SETOFFSET | modes,
                        .time = {.tv_sec = seconds, .tv_usec = fraction}};
  return route ? syscall(SYS_clock_adjtime, CLOCK_REALTIME, &value) : syscall(SYS_adjtimex, &value);
}

static int steps(void) {
  int failed = 0, canceled = -1, absolute = -1, monotonic = -1;
  struct timex value = {.modes = ADJ_NANO};
  struct itimerspec setting = {0}, remaining;
  uint64_t count;
  CHECK(geteuid() == 0);
  CHECK(adjtimex(&value) == TIME_ERROR && (value.status & STA_NANO));
  value = (struct timex){.modes = ADJ_OFFSET_SS_READ};
  CHECK(adjtimex(&value) == TIME_ERROR && (value.status & STA_NANO));
  value = (struct timex){0};
  CHECK(adjtimex(&value) == TIME_ERROR && (value.status & STA_NANO));

  CHECK((canceled = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK)) >= 0);
  CHECK((absolute = timerfd_create(CLOCK_REALTIME, TFD_NONBLOCK)) >= 0);
  CHECK((monotonic = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK)) >= 0);
  setting.it_value = timespec(now(CLOCK_REALTIME) + 5000000000);
  CHECK(timerfd_settime(canceled, TFD_TIMER_ABSTIME | TFD_TIMER_CANCEL_ON_SET, &setting, NULL) ==
        0);
  CHECK(timerfd_settime(absolute, TFD_TIMER_ABSTIME, &setting, NULL) == 0);
  setting.it_value = timespec(now(CLOCK_MONOTONIC) + 5000000000);
  CHECK(timerfd_settime(monotonic, TFD_TIMER_ABSTIME, &setting, NULL) == 0);
  int64_t before = now(CLOCK_REALTIME) - now(CLOCK_MONOTONIC);
  CHECK(step(10, 250000000, ADJ_NANO, 1) == TIME_ERROR);
  int64_t after = now(CLOCK_REALTIME) - now(CLOCK_MONOTONIC);
  CHECK(after - before > 10150000000 && after - before < 10350000000);
  CHECK(read(canceled, &count, sizeof(count)) == -1 && errno == ECANCELED);
  CHECK(read(absolute, &count, sizeof(count)) == sizeof(count) && count == 1);
  CHECK(read(monotonic, &count, sizeof(count)) == -1 && errno == EAGAIN);
  CHECK(timerfd_gettime(monotonic, &remaining) == 0 && remaining.it_value.tv_sec >= 3);

  // SETOFFSET input remains microseconds unless this request includes ADJ_NANO.
  before = after;
  CHECK(step(-2, 750000, 0, 0) == TIME_ERROR);
  after = now(CLOCK_REALTIME) - now(CLOCK_MONOTONIC);
  CHECK(after - before > -1350000000 && after - before < -1150000000);
  value = (struct timex){0};
  CHECK(adjtimex(&value) == TIME_ERROR && (value.status & STA_NANO));

  value = (struct timex){.modes = ADJ_MICRO};
  CHECK(adjtimex(&value) == TIME_ERROR && !(value.status & STA_NANO));
  CHECK(step(0, -1, ADJ_NANO, 0) == -1 && errno == EINVAL);
  CHECK(step(0, 1000000, 0, 0) == -1 && errno == EINVAL);
  CHECK(step(0, 1000000000, ADJ_NANO, 1) == -1 && errno == EINVAL);
  CHECK(step(INT64_MAX, 0, ADJ_NANO, 0) == -1 && errno == EINVAL);
  CHECK(step(INT64_MIN, 0, ADJ_NANO, 1) == -1 && errno == EINVAL);
  CHECK(step(-now(CLOCK_REALTIME) / 1000000000 - 60, 0, ADJ_NANO, 0) == -1 && errno == EINVAL);
  value = (struct timex){0};
  CHECK(adjtimex(&value) == TIME_ERROR && !(value.status & STA_NANO));
  const unsigned unsupported[] = {ADJ_FREQUENCY, ADJ_OFFSET,   ADJ_OFFSET_SINGLESHOT,
                                  ADJ_STATUS,    ADJ_TICK,     ADJ_TAI,
                                  ADJ_MAXERROR,  ADJ_ESTERROR, ADJ_TIMECONST};
  for (unsigned n = 0; n < sizeof(unsupported) / sizeof(unsupported[0]); ++n) {
    value = (struct timex){.modes = unsupported[n], .freq = 100, .status = STA_PLL};
    CHECK(adjtimex(&value) == -1 && errno == EOPNOTSUPP);
  }
  value = (struct timex){.modes = 0x40000000};
  CHECK(adjtimex(&value) == -1 && errno == EINVAL);
out:
  if (canceled >= 0)
    close(canceled);
  if (absolute >= 0)
    close(absolute);
  if (monotonic >= 0)
    close(monotonic);
  return failed;
}

static int privilege(void) {
  int failed = 0;
  CHECK(setuid(65534) == 0 && geteuid() == 65534);
  CHECK(query() == 0);
  CHECK(step(0, 0, 0, 0) == -1 && errno == EPERM);
  const unsigned modes[] = {ADJ_NANO, ADJ_MICRO, ADJ_OFFSET_SINGLESHOT, ADJ_FREQUENCY};
  for (unsigned n = 0; n < sizeof(modes) / sizeof(modes[0]); ++n) {
    struct timex value = {.modes = modes[n]};
    CHECK(adjtimex(&value) == -1 && errno == EPERM);
  }
out:
  return failed;
}

static int reap(pid_t child) {
  const int64_t deadline = now(CLOCK_MONOTONIC) + 30000000000;
  while (now(CLOCK_MONOTONIC) < deadline) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
  }
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}

int main(void) {
  if (geteuid()) {
    puts("CLOCK-ADJUST-CONTRACT: FAIL requires root");
    return 1;
  }
  const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {{"query", query}, {"steps", steps}, {"privilege", privilege}};
  for (unsigned n = 0; n < sizeof(suites) / sizeof(suites[0]); ++n) {
    struct timex original = {0};
    if (adjtimex(&original) < 0)
      return 1;
    int64_t realtime = now(CLOCK_REALTIME), monotonic = now(CLOCK_MONOTONIC);
    printf("CLOCK-ADJUST-CONTRACT: BEGIN %s\n", suites[n].name);
    fflush(stdout);
    pid_t child = fork();
    if (child < 0)
      return 1;
    if (!child) {
      alarm(25);
      int result = suites[n].test();
      fflush(stdout);
      fflush(stderr);
      _exit(result ? 1 : 0);
    }
    int result = reap(child);
    if (!strcmp(suites[n].name, "steps")) {
      struct timespec restored = timespec(realtime + now(CLOCK_MONOTONIC) - monotonic);
      struct timex mode = {.modes = original.status & STA_NANO ? ADJ_NANO : ADJ_MICRO};
      if (clock_settime(CLOCK_REALTIME, &restored) || adjtimex(&mode) < 0)
        result = -1;
    }
    printf("CLOCK-ADJUST-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", suites[n].name,
           result);
    fflush(stdout);
    if (result)
      return 1;
  }
  puts("CLOCK-ADJUST-CONTRACT: END PASS");
  return 0;
}
