#define _GNU_SOURCE

#include <errno.h>
#include <sched.h>
#include <signal.h>
#include <stdio.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contracts.h"
#include <sys/wait.h>

long long test_milliseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now)) {
    return -1;
  }
  return (long long)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

int test_wait_flag(volatile int* flag) {
  const long long start = test_milliseconds();
  if (start < 0) {
    return -1;
  }
  while (!__atomic_load_n(flag, __ATOMIC_ACQUIRE)) {
    const long long now = test_milliseconds();
    if (now < 0 || now - start >= 3000) {
      return -1;
    }
    sched_yield();
  }
  return 0;
}

void test_pause(void) {
  struct timespec delay = {.tv_nsec = 2000000};
  while (nanosleep(&delay, &delay) && errno == EINTR) {
  }
}

static int run_bounded(const char* program, const char* name) {
  printf("SIGNAL-EXEC-TEST: BEGIN %s\n", name);
  fflush(stdout);
  const pid_t child = fork();
  if (child < 0) {
    return 255;
  }
  if (!child) {
    alarm(10);
    const int result =
        !strncmp(name, "exec-", 5) ? exec_contract(program, name) : signal_contract(name);
    _exit(result);
  }

  int status = 0;
  const long long start = test_milliseconds();
  while (start >= 0) {
    const pid_t waited = waitpid(child, &status, WNOHANG);
    if (waited == child) {
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    }
    if (waited < 0 && errno != EINTR) {
      break;
    }
    const long long now = test_milliseconds();
    if (now < 0 || now - start >= 15000) {
      break;
    }
    test_pause();
  }
  (void)kill(child, SIGKILL);
  while (waitpid(child, &status, 0) < 0 && errno == EINTR) {
  }
  return 254;
}

int main(int argc, char* argv[]) {
  if (argc > 1 &&
      (!strcmp(argv[1], "--exec-image") || !strcmp(argv[1], "--exec-race-image") ||
       !strcmp(argv[1], "--exec-pending-image") || !strcmp(argv[1], "--exec-exit-image"))) {
    return exec_image_contract(argc, argv);
  }

  const char* cases[] = {
      "read-eintr",        "read-restart",        "wait-eintr",     "wait-restart",
      "partial-write",     "nanosleep-eintr",     "ppoll-eintr",    "pselect-eintr",
      "epoll-eintr",       "reset-hand",          "exec-main",      "exec-worker",
      "exec-failure",      "futex-eintr",         "futex-restart",  "exec-race",
      "exec-pending-main", "exec-pending-worker", "exec-exit-race", "sigsuspend-eintr"};
  int selected = 0;
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    if (argc > 1 && strcmp(argv[1], cases[i])) {
      continue;
    }
    selected = 1;
    const int exit_race = !strcmp(cases[i], "exec-exit-race");
    for (int iteration = 0; iteration < (exit_race ? 3 : 1); ++iteration) {
      const int result = run_bounded(argv[0], cases[i]);
      if (result && !(exit_race && result == 37)) {
        printf("SIGNAL-EXEC-TEST: FAIL %s code=%d\n", cases[i], result);
        puts("SIGNAL-EXEC-TEST: END FAIL");
        return 1;
      }
      if (exit_race) {
        printf("SIGNAL-EXEC-TEST: EXIT-RACE iteration=%d code=%d\n", iteration + 1, result);
      }
    }
    printf("SIGNAL-EXEC-TEST: PASS %s\n", cases[i]);
    fflush(stdout);
  }
  if (!selected) {
    fprintf(stderr, "Unknown signal/exec test: %s\n", argv[1]);
    return 2;
  }
  puts("SIGNAL-EXEC-TEST: PASS");
  puts("SIGNAL-EXEC-TEST: END PASS");
  return 0;
}
