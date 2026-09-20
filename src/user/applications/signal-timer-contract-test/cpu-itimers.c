#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdint.h>
#include <unistd.h>

#include "contract.h"
#include <sys/resource.h>
#include <sys/time.h>

static volatile sig_atomic_t virtual_expiries, profile_expiries;
static volatile uint64_t checksum = 0x12345678;

static void expired(int signal) {
  if (signal == SIGVTALRM)
    ++virtual_expiries;
  else if (signal == SIGPROF)
    ++profile_expiries;
}

static int arm_cpu(int timer, int value_ms, int interval_ms) {
  struct itimerval setting = {.it_value = {value_ms / 1000, (value_ms % 1000) * 1000},
                              .it_interval = {interval_ms / 1000, (interval_ms % 1000) * 1000}};
  return setitimer(timer, &setting, NULL);
}

static int64_t timeval_us(struct timeval value) {
  return (int64_t)value.tv_sec * 1000000 + value.tv_usec;
}

static void user_chunk(void) {
  uint64_t value = checksum;
  for (unsigned i = 0; i < 65536; ++i) {
    value ^= value << 13;
    value ^= value >> 7;
    value ^= value << 17;
  }
  checksum = value;
}

static int burn_user(int milliseconds) {
  struct rusage before, after;
  if (getrusage(RUSAGE_SELF, &before))
    return -1;
  int64_t deadline = st_now(CLOCK_MONOTONIC) + 5000000000;
  do {
    user_chunk();
    if (getrusage(RUSAGE_SELF, &after) || st_now(CLOCK_MONOTONIC) >= deadline)
      return -1;
  } while (timeval_us(after.ru_utime) - timeval_us(before.ru_utime) < milliseconds * 1000);
  return 0;
}

static int await_expiries(volatile sig_atomic_t* count, sig_atomic_t target) {
  int64_t deadline = st_now(CLOCK_MONOTONIC) + 5000000000;
  while (*count < target) {
    user_chunk();
    if (st_now(CLOCK_MONOTONIC) >= deadline)
      return -1;
  }
  return 0;
}

int signal_timer_test_cpu_itimers(void) {
  int failed = 0;
  struct sigaction action = {.sa_handler = expired};
  struct itimerval observed;
  struct rusage before, after;
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGVTALRM, &action, NULL) == 0);
  CHECK(sigaction(SIGPROF, &action, NULL) == 0);
  CHECK(arm_cpu(ITIMER_VIRTUAL, 0, 0) == 0 && arm_cpu(ITIMER_PROF, 0, 0) == 0);
  CHECK(getrusage(RUSAGE_SELF, &before) == 0 && burn_user(40) == 0 &&
        getrusage(RUSAGE_SELF, &after) == 0);
  // Sampled accounting may miss the brief syscalls in this user-heavy workload.
  CHECK(timeval_us(after.ru_utime) > timeval_us(before.ru_utime) &&
        timeval_us(after.ru_stime) >= timeval_us(before.ru_stime) && !virtual_expiries &&
        !profile_expiries);
  puts("CPU-ITIMER-CONTRACT: PASS disarmed-rusage");

  CHECK(arm_cpu(ITIMER_VIRTUAL, 20, 0) == 0 && arm_cpu(ITIMER_PROF, 50, 0) == 0);
  CHECK(await_expiries(&virtual_expiries, 1) == 0 && await_expiries(&profile_expiries, 1) == 0);
  CHECK(getitimer(ITIMER_VIRTUAL, &observed) == 0 && !timeval_us(observed.it_value));
  CHECK(getitimer(ITIMER_PROF, &observed) == 0 && !timeval_us(observed.it_value));
  CHECK(virtual_expiries == 1 && profile_expiries == 1);
  puts("CPU-ITIMER-CONTRACT: PASS one-shot-signals");

  CHECK(arm_cpu(ITIMER_VIRTUAL, 20, 20) == 0 && arm_cpu(ITIMER_PROF, 20, 20) == 0);
  CHECK(await_expiries(&virtual_expiries, 3) == 0 && await_expiries(&profile_expiries, 3) == 0);
  CHECK(arm_cpu(ITIMER_VIRTUAL, 0, 0) == 0);
  sig_atomic_t virtual_before = virtual_expiries;
  CHECK(await_expiries(&profile_expiries, profile_expiries + 2) == 0);
  // Disarming does not withdraw a signal whose expiry was already delivered.
  CHECK(virtual_expiries <= virtual_before + 1);
  CHECK(getitimer(ITIMER_VIRTUAL, &observed) == 0 && !timeval_us(observed.it_value));
  virtual_before = virtual_expiries;
  CHECK(arm_cpu(ITIMER_VIRTUAL, 20, 0) == 0);
  CHECK(await_expiries(&virtual_expiries, virtual_before + 1) == 0);
  CHECK(await_expiries(&profile_expiries, profile_expiries + 2) == 0);
  CHECK(getitimer(ITIMER_PROF, &observed) == 0 && timeval_us(observed.it_interval) == 20000 &&
        timeval_us(observed.it_value) > 0);
  CHECK(arm_cpu(ITIMER_VIRTUAL, 20, 20) == 0 && arm_cpu(ITIMER_PROF, 0, 0) == 0);
  CHECK(await_expiries(&virtual_expiries, virtual_expiries + 2) == 0);
  CHECK(getitimer(ITIMER_PROF, &observed) == 0 && !timeval_us(observed.it_value));
  puts("CPU-ITIMER-CONTRACT: PASS independent-periodic-interest");

  CHECK(arm_cpu(ITIMER_VIRTUAL, 0, 0) == 0 && burn_user(80) == 0);
  virtual_before = virtual_expiries;
  CHECK(arm_cpu(ITIMER_VIRTUAL, 50, 0) == 0);
  CHECK(getitimer(ITIMER_VIRTUAL, &observed) == 0 && timeval_us(observed.it_value) >= 35000);
  CHECK(burn_user(10) == 0 && getitimer(ITIMER_VIRTUAL, &observed) == 0 &&
        timeval_us(observed.it_value) > 0);
  CHECK(await_expiries(&virtual_expiries, virtual_before + 1) == 0);
  CHECK(getitimer(ITIMER_VIRTUAL, &observed) == 0 && !timeval_us(observed.it_value));
  printf("CPU-ITIMER-CONTRACT: PASS rearm-baseline virtual=%d profile=%d checksum=%llu\n",
         (int)virtual_expiries, (int)profile_expiries, (unsigned long long)checksum);
out:
  arm_cpu(ITIMER_VIRTUAL, 0, 0);
  arm_cpu(ITIMER_PROF, 0, 0);
  return failed;
}
