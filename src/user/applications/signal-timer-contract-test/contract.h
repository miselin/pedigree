#ifndef SIGNAL_TIMER_CONTRACT_H
#define SIGNAL_TIMER_CONTRACT_H

#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#include <sys/types.h>

#define CHECK(condition)                                                                \
  do {                                                                                  \
    if (!(condition)) {                                                                 \
      fprintf(stderr, "%s:%d: %s (errno=%d)\n", __FILE__, __LINE__, #condition, errno); \
      failed = 1;                                                                       \
      goto out;                                                                         \
    }                                                                                   \
  } while (0)

int64_t st_now(clockid_t clock);
struct timespec st_timespec(int64_t nanoseconds);
void st_pause(int milliseconds);
int st_wait(volatile int* flag, int milliseconds);
int st_reap(pid_t child, int milliseconds);
int signal_timer_test_signals(void);
int signal_timer_test_timers(void);
int signal_timer_test_clock(void);
int signal_timer_test_raw_clock(void);
int signal_timer_exec(int argc, char** argv);

#endif
