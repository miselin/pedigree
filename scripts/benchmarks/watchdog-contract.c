#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

extern int delete_module(const char*, unsigned int);

static void fail(const char* operation) {
  printf("WATCHDOG-CONTRACT: FAIL operation=%s errno=%d\n", operation, errno);
  exit(1);
}

static void timeout(int signal) {
  (void)signal;
  static const char message[] = "WATCHDOG-CONTRACT: FAIL deadline\n";
  write(STDOUT_FILENO, message, sizeof(message) - 1);
  _exit(124);
}

static uint64_t now_ns(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now))
    fail("clock");
  return (uint64_t)now.tv_sec * 1000000000ULL + (uint64_t)now.tv_nsec;
}

static void survive(const char* phase, unsigned seconds) {
  const uint64_t start = now_ns();
  printf("WATCHDOG-CONTRACT: BEGIN %s seconds=%u\n", phase, seconds);
  struct timespec remaining = {(time_t)seconds, 0};
  while (nanosleep(&remaining, &remaining)) {
    if (errno != EINTR)
      fail("nanosleep");
  }
  const uint64_t elapsed = now_ns() - start;
  if (elapsed < (uint64_t)seconds * 1000000000ULL)
    fail("short-sleep");
  printf("WATCHDOG-CONTRACT: PASS %s elapsed_us=%llu\n", phase,
         (unsigned long long)(elapsed / 1000));
}

int main(void) {
  int serial = open("/dev/ttyS0", O_RDWR);
  if (serial < 0 || dup2(serial, STDOUT_FILENO) < 0 || dup2(serial, STDERR_FILENO) < 0)
    return 2;
  if (serial > STDERR_FILENO)
    close(serial);
  setvbuf(stdout, NULL, _IONBF, 0);
  struct sigaction action = {.sa_handler = timeout};
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGALRM, &action, NULL))
    fail("sigaction");
  alarm(65);

  // Run with QEMU -device ib700; the host must observe its enabled timer.
  survive("armed-refresh", 12);
  if (delete_module("ib700_wdt", O_NONBLOCK))
    fail("delete-module");
  puts("WATCHDOG-CONTRACT: PASS unload");
  survive("disabled-after-unload", 35);
  alarm(0);
  puts("WATCHDOG-CONTRACT: END PASS");
  return 0;
}
