#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/wait.h>

int64_t st_now(clockid_t clock) {
  struct timespec now;
  return clock_gettime(clock, &now) ? -1 : (int64_t)now.tv_sec * 1000000000 + now.tv_nsec;
}
struct timespec st_timespec(int64_t nanoseconds) {
  return (struct timespec){nanoseconds / 1000000000, nanoseconds % 1000000000};
}
void st_pause(int milliseconds) {
  struct timespec pause = st_timespec((int64_t)milliseconds * 1000000);
  while (nanosleep(&pause, &pause) && errno == EINTR) {
  }
}
int st_wait(volatile int* flag, int milliseconds) {
  int64_t deadline = st_now(CLOCK_MONOTONIC) + (int64_t)milliseconds * 1000000;
  while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
    if (st_now(CLOCK_MONOTONIC) >= deadline)
      return -1;
    st_pause(2);
  }
  return 0;
}
int st_reap(pid_t child, int milliseconds) {
  int64_t deadline = st_now(CLOCK_MONOTONIC) + (int64_t)milliseconds * 1000000;
  while (st_now(CLOCK_MONOTONIC) < deadline) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    st_pause(5);
  }
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}

static int run(const char* name, int (*test)(void)) {
  printf("SIGNAL-TIMER-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  const int clock_suite = !strcmp(name, "clock") || !strcmp(name, "raw-clock");
  int64_t realtime = st_now(CLOCK_REALTIME), monotonic = st_now(CLOCK_MONOTONIC);
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child) {
    alarm(30);
    int result = test();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  int result = st_reap(child, 35000);
  if (clock_suite && geteuid() == 0) {
    // Restore the guest clock even if the child fails after changing it.
    struct timespec restored = st_timespec(realtime + st_now(CLOCK_MONOTONIC) - monotonic);
    if (clock_settime(CLOCK_REALTIME, &restored))
      result = -1;
  }
  printf("SIGNAL-TIMER-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", name, result);
  fflush(stdout);
  return result;
}
int main(int argc, char** argv) {
  if (argc > 1 && !strcmp(argv[1], "timer-exec"))
    return signal_timer_exec(argc, argv);
  static const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {{"signals", signal_timer_test_signals},
                {"timers", signal_timer_test_timers},
                {"clock", signal_timer_test_clock},
                {"raw-clock", signal_timer_test_raw_clock}};
  int selected = 0;
  for (unsigned n = 0; n < sizeof(suites) / sizeof(suites[0]); ++n) {
    if (argc > 1 && strcmp(argv[1], suites[n].name))
      continue;
    selected = 1;
    if (run(suites[n].name, suites[n].test))
      return 1;
  }
  if (!selected)
    return 2;
  puts("SIGNAL-TIMER-CONTRACT: END PASS");
  return 0;
}
