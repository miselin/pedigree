#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/syscall.h>

struct waiter {
  sigset_t set;
  siginfo_t info;
  volatile int entered, done;
  int result;
};
static void finished(void* argument) {
  struct waiter* waiter = argument;
  __atomic_store_n(&waiter->done, 1, __ATOMIC_RELEASE);
}
static void* wait_for_signal(void* argument) {
  struct waiter* waiter = argument;
  pthread_cleanup_push(finished, waiter);
  __atomic_store_n(&waiter->entered, 1, __ATOMIC_RELEASE);
  waiter->result = sigwaitinfo(&waiter->set, &waiter->info);
  pthread_cleanup_pop(1);
  return NULL;
}
static volatile int handled;
static siginfo_t handler_info;
static void handler(int signal, siginfo_t* info, void* context) {
  (void)signal;
  (void)context;
  handler_info = *info;
  __atomic_store_n(&handled, 1, __ATOMIC_RELEASE);
}

static int blocked_ignored_signals(void) {
  int failed = 0, masked = 0, installed = 0;
  const int signals[] = {SIGCHLD, SIGUSR2};
  sigset_t set, original, pending, one;
  struct sigaction previous[2];
  const struct timespec zero = {0};
  siginfo_t info;
  CHECK(sigemptyset(&set) == 0 && sigaddset(&set, SIGCHLD) == 0 && sigaddset(&set, SIGUSR2) == 0);
  CHECK(pthread_sigmask(SIG_BLOCK, &set, &original) == 0);
  masked = 1;
  for (int n = 0; n < 2; ++n) {
    struct sigaction action = {.sa_handler = n ? SIG_IGN : SIG_DFL};
    sigemptyset(&action.sa_mask);
    CHECK(sigaction(signals[n], &action, &previous[n]) == 0);
    ++installed;
  }
  for (int n = 0; n < 2; ++n) {
    CHECK(n ? sigqueue(getpid(), signals[n], (union sigval){.sival_int = 77}) == 0
            : kill(getpid(), signals[n]) == 0);
    CHECK(sigpending(&pending) == 0 && sigismember(&pending, signals[n]) == 1);
    CHECK(sigemptyset(&one) == 0 && sigaddset(&one, signals[n]) == 0);
    CHECK(sigtimedwait(&one, &info, &zero) == signals[n] && info.si_signo == signals[n]);
    CHECK(sigpending(&pending) == 0 && sigismember(&pending, signals[n]) == 0);
  }

out:
  if (masked) {
    // Both signals remain blocked until any test-generated pending signal is
    // consumed, so restoring SIGUSR2's original disposition cannot deliver it.
    while (sigtimedwait(&set, &info, &zero) >= 0) {
    }
    for (int n = 0; n < installed; ++n)
      if (sigaction(signals[n], &previous[n], NULL))
        failed = 1;
    if (pthread_sigmask(SIG_SETMASK, &original, NULL))
      failed = 1;
  }
  return failed;
}

int signal_timer_test_signals(void) {
  int failed = 0, masked = 0, worker_active = 0, handler_installed = 0;
  sigset_t set, original, pending, one;
  struct sigaction action = {.sa_sigaction = handler, .sa_flags = SA_SIGINFO}, old_action;
  struct timespec zero = {0}, timeout = {0, 150000000};
  siginfo_t info;
  pthread_t worker;
  struct waiter waiter = {0};
  void* bad = MAP_FAILED;
  const int low = SIGRTMIN, high = SIGRTMAX;
  const pid_t pid = getpid();
  const uid_t uid = getuid();
  sigemptyset(&set);
  CHECK(blocked_ignored_signals() == 0);
  CHECK(high == 64 && low > 34);
  CHECK(sigaddset(&set, SIGUSR1) == 0 && sigaddset(&set, low) == 0 &&
        sigaddset(&set, low + 1) == 0 && sigaddset(&set, high) == 0);
  CHECK(pthread_sigmask(SIG_BLOCK, &set, &original) == 0);
  masked = 1;
  sigemptyset(&one);
  sigaddset(&one, SIGUSR1);
  CHECK(sigqueue(pid, SIGUSR1, (union sigval){.sival_int = 101}) == 0);
  CHECK(sigqueue(pid, SIGUSR1, (union sigval){.sival_int = 202}) == 0);
  CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1) == 1);
  CHECK(sigwaitinfo(&one, &info) == SIGUSR1 && info.si_code == SI_QUEUE && info.si_pid == pid &&
        info.si_uid == uid && info.si_value.sival_int == 101);
  CHECK(sigtimedwait(&one, &info, &zero) == -1 && errno == EAGAIN);
  CHECK(sigpending(&pending) == 0 && sigismember(&pending, SIGUSR1) == 0);

  CHECK(sigqueue(pid, high, (union sigval){.sival_int = 640}) == 0);
  CHECK(sigqueue(pid, low, (union sigval){.sival_int = 350}) == 0);
  CHECK(sigqueue(pid, low, (union sigval){.sival_int = 351}) == 0);
  CHECK(sigwaitinfo(&set, &info) == low && info.si_value.sival_int == 350);
  CHECK(sigwaitinfo(&set, &info) == low && info.si_value.sival_int == 351);
  CHECK(sigwaitinfo(&set, &info) == high && info.si_value.sival_int == 640);
  sigemptyset(&one);
  sigaddset(&one, low);
  for (int n = 0; n < 16; ++n)
    CHECK(sigqueue(pid, low, (union sigval){.sival_int = n}) == 0);
  CHECK(sigqueue(pid, low, (union sigval){.sival_int = 16}) == -1 && errno == EAGAIN);
  CHECK(kill(pid, low) == -1 && errno == EAGAIN);
  CHECK(pthread_kill(pthread_self(), low) == EAGAIN);
  CHECK(sigwaitinfo(&one, &info) == low && info.si_value.sival_int == 0);
  CHECK(sigqueue(pid, low, (union sigval){.sival_int = 16}) == 0);
  for (int n = 1; n <= 16; ++n)
    CHECK(sigwaitinfo(&one, &info) == low && info.si_value.sival_int == n);

  bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  CHECK(sigpending(bad) == -1 && errno == EFAULT);
  CHECK(sigqueue(pid, low, (union sigval){.sival_int = 999}) == 0);
  CHECK(sigtimedwait(&one, bad, &zero) == -1 && errno == EFAULT);
  CHECK(sigpending(&pending) == 0 && sigismember(&pending, low) == 1);
  CHECK(sigwaitinfo(&one, &info) == low && info.si_value.sival_int == 999);
  CHECK(syscall(SYS_rt_sigpending, &pending, 16) == -1 && errno == EINVAL);
  struct timespec invalid = {0, 1000000000};
  CHECK(sigtimedwait(&one, &info, &invalid) == -1 && errno == EINVAL);
  int64_t started = st_now(CLOCK_MONOTONIC);
  CHECK(sigtimedwait(&one, &info, &timeout) == -1 && errno == EAGAIN);
  int64_t elapsed = st_now(CLOCK_MONOTONIC) - started;
  CHECK(elapsed >= 100000000 && elapsed < 2000000000);

  waiter.set = one;
  CHECK(pthread_create(&worker, NULL, wait_for_signal, &waiter) == 0);
  worker_active = 1;
  CHECK(st_wait(&waiter.entered, 1000) == 0);
  st_pause(20);
  CHECK(sigqueue(pid, low, (union sigval){.sival_int = 501}) == 0);
  CHECK(st_wait(&waiter.done, 2000) == 0 && pthread_join(worker, NULL) == 0);
  worker_active = 0;
  CHECK(waiter.result == low && waiter.info.si_value.sival_int == 501);
  memset(&waiter, 0, sizeof(waiter));
  waiter.set = one;
  CHECK(pthread_create(&worker, NULL, wait_for_signal, &waiter) == 0);
  worker_active = 1;
  CHECK(st_wait(&waiter.entered, 1000) == 0);
  st_pause(20);
  CHECK(pthread_cancel(worker) == 0 && st_wait(&waiter.done, 2000) == 0);
  void* result = NULL;
  CHECK(pthread_join(worker, &result) == 0 && result == PTHREAD_CANCELED);
  worker_active = 0;

  CHECK(sigaction(high, &action, &old_action) == 0);
  handler_installed = 1;
  sigemptyset(&one);
  sigaddset(&one, high);
  CHECK(pthread_sigmask(SIG_UNBLOCK, &one, NULL) == 0);
  CHECK(sigqueue(pid, high, (union sigval){.sival_int = 7654}) == 0 &&
        st_wait(&handled, 2000) == 0);
  CHECK(handler_info.si_signo == high && handler_info.si_code == SI_QUEUE &&
        handler_info.si_value.sival_int == 7654 && handler_info.si_pid == pid &&
        handler_info.si_uid == uid);

out:
  if (worker_active) {
    pthread_cancel(worker);
    if (st_wait(&waiter.done, 1000) == 0)
      pthread_join(worker, NULL);
    else
      _exit(1);
  }
  if (masked) {
    pthread_sigmask(SIG_BLOCK, &set, NULL);
    while (sigtimedwait(&set, &info, &zero) >= 0) {
    }
    if (handler_installed)
      sigaction(high, &old_action, NULL);
    pthread_sigmask(SIG_SETMASK, &original, NULL);
  }
  if (bad != MAP_FAILED)
    munmap(bad, 4096);
  return failed;
}
