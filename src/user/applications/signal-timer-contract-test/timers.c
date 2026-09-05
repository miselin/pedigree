#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/syscall.h>

static int arm(timer_t timer, int milliseconds, int interval) {
  struct itimerspec setting = {.it_value = st_timespec((int64_t)milliseconds * 1000000),
                               .it_interval = st_timespec((int64_t)interval * 1000000)};
  return timer_settime(timer, 0, &setting, NULL);
}
static int64_t remaining(const struct itimerspec* setting) {
  return (int64_t)setting->it_value.tv_sec * 1000000000 + setting->it_value.tv_nsec;
}

static int basic_timers(void) {
  int failed = 0, live = 0;
  timer_t timer;
  struct sigevent event = {.sigev_notify = SIGEV_NONE};
  struct itimerspec setting, replacement = {.it_value = {0, 1000000}};
  void* bad = mmap(NULL, 4096, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  CHECK(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0);
  live = 1;
  CHECK(timer_gettime(timer, &setting) == 0 && remaining(&setting) == 0);
  CHECK(arm(timer, 3000, 0) == 0);
  CHECK(timer_gettime(timer, &setting) == 0 && remaining(&setting) > 1000000000);
  CHECK(timer_settime(timer, 0, &replacement, bad) == -1 && errno == EFAULT);
  CHECK(timer_gettime(timer, &setting) == 0 && remaining(&setting) > 1000000000);
  CHECK(timer_gettime(timer, bad) == -1 && errno == EFAULT);
  CHECK(syscall(SYS_timer_settime, timer, 0, NULL, NULL) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_timer_settime, timer, 0, bad, NULL) == -1 && errno == EFAULT);
  replacement.it_value.tv_nsec = 1000000000;
  CHECK(timer_settime(timer, 0, &replacement, NULL) == -1 && errno == EINVAL);
  CHECK(arm(timer, 0, 10) == 0);
  CHECK(timer_gettime(timer, &setting) == 0 && remaining(&setting) == 0 &&
        setting.it_interval.tv_sec == 0 && setting.it_interval.tv_nsec == 0);
  CHECK(timer_delete(timer) == 0);
  live = 0;
  CHECK(timer_gettime(timer, &setting) == -1 && errno == EINVAL);
  CHECK(syscall(SYS_timer_create, CLOCK_MONOTONIC, NULL, bad) == -1 && errno == EFAULT);
  CHECK(timer_create(CLOCK_PROCESS_CPUTIME_ID, &event, &timer) == -1 && errno == EINVAL);
out:
  if (live)
    timer_delete(timer);
  if (bad != MAP_FAILED)
    munmap(bad, 4096);
  return failed;
}

static int delivered_timers(void) {
  int failed = 0, live = 0, masked = 0;
  timer_t timer;
  sigset_t set, original;
  siginfo_t info;
  struct timespec zero = {0}, timeout = {2, 0};
  struct itimerspec setting;
  struct sigevent event = {
      .sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGRTMIN, .sigev_value.sival_int = 0x4567};
  sigemptyset(&set);
  sigaddset(&set, SIGRTMIN);
  sigaddset(&set, SIGALRM);
  CHECK(pthread_sigmask(SIG_BLOCK, &set, &original) == 0);
  masked = 1;
  CHECK(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0);
  live = 1;
  CHECK(arm(timer, 60, 0) == 0);
  CHECK(sigtimedwait(&set, &info, &timeout) == SIGRTMIN && info.si_code == SI_TIMER &&
        info.si_value.sival_int == 0x4567 && info.si_overrun == 0);
  CHECK(timer_gettime(timer, &setting) == 0 && remaining(&setting) == 0);
  CHECK(timer_getoverrun(timer) == 0);
  CHECK(arm(timer, 30, 30) == 0);
  st_pause(170);
  CHECK(sigtimedwait(&set, &info, &timeout) == SIGRTMIN && info.si_code == SI_TIMER &&
        info.si_overrun >= 1 && timer_getoverrun(timer) == info.si_overrun);
  CHECK(arm(timer, 0, 0) == 0);
  CHECK(sigtimedwait(&set, &info, &zero) == -1 && errno == EAGAIN);

  CHECK(arm(timer, 20, 0) == 0);
  st_pause(80);
  CHECK(arm(timer, 500, 0) == 0);
  CHECK(sigtimedwait(&set, &info, &zero) == -1 && errno == EAGAIN);
  CHECK(arm(timer, 20, 0) == 0);
  st_pause(80);
  CHECK(timer_delete(timer) == 0);
  live = 0;
  CHECK(sigtimedwait(&set, &info, &zero) == -1 && errno == EAGAIN);

  CHECK(timer_create(CLOCK_REALTIME, NULL, &timer) == 0);
  live = 1;
  CHECK(arm(timer, 40, 0) == 0);
  CHECK(sigtimedwait(&set, &info, &timeout) == SIGALRM && info.si_code == SI_TIMER &&
        info.si_value.sival_int == (int)(intptr_t)timer);
out:
  if (live)
    timer_delete(timer);
  if (masked) {
    while (sigtimedwait(&set, &info, &zero) >= 0) {
    }
    pthread_sigmask(SIG_SETMASK, &original, NULL);
  }
  return failed;
}

struct target_wait {
  sigset_t set;
  siginfo_t info;
  volatile int tid, done;
  int result;
};
static void target_done(void* argument) {
  struct target_wait* target = argument;
  __atomic_store_n(&target->done, 1, __ATOMIC_RELEASE);
}
static void* target_wait(void* argument) {
  struct target_wait* target = argument;
  struct timespec timeout = {2, 0};
  pthread_cleanup_push(target_done, target);
  __atomic_store_n(&target->tid, (int)syscall(SYS_gettid), __ATOMIC_RELEASE);
  target->result = sigtimedwait(&target->set, &target->info, &timeout);
  pthread_cleanup_pop(1);
  return NULL;
}
struct callback_state {
  sem_t completed;
  volatile int count, tid;
};
static void timer_callback(union sigval value) {
  struct callback_state* state = value.sival_ptr;
  __atomic_store_n(&state->tid, (int)syscall(SYS_gettid), __ATOMIC_RELEASE);
  __atomic_add_fetch(&state->count, 1, __ATOMIC_RELEASE);
  sem_post(&state->completed);
}
static int threaded_timers(void) {
  int failed = 0, live = 0, masked = 0, worker_active = 0, sem_live = 0;
  pthread_t worker;
  timer_t timer;
  struct target_wait target = {0};
  static struct callback_state callback;
  sigset_t original;
  siginfo_t info;
  struct timespec zero = {0};
  struct itimerspec setting;
  struct sigevent event = {
      .sigev_notify = SIGEV_THREAD_ID, .sigev_signo = SIGRTMAX, .sigev_value.sival_int = 6401};
  sigemptyset(&target.set);
  sigaddset(&target.set, SIGRTMAX);
  CHECK(pthread_sigmask(SIG_BLOCK, &target.set, &original) == 0);
  masked = 1;
  CHECK(pthread_create(&worker, NULL, target_wait, &target) == 0);
  worker_active = 1;
  CHECK(st_wait(&target.tid, 1000) == 0);
  event.sigev_notify_thread_id = target.tid;
  CHECK(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0);
  live = 1;
  CHECK(arm(timer, 40, 0) == 0);
  CHECK(st_wait(&target.done, 2000) == 0 && pthread_join(worker, NULL) == 0);
  worker_active = 0;
  CHECK(target.result == SIGRTMAX && target.info.si_code == SI_TIMER &&
        target.info.si_value.sival_int == 6401);
  CHECK(sigtimedwait(&target.set, &info, &zero) == -1 && errno == EAGAIN);
  CHECK(timer_gettime(timer, &setting) == -1 && errno == EINVAL);
  live = 0;

  CHECK(sem_init(&callback.completed, 0, 0) == 0);
  sem_live = 1;
  event = (struct sigevent){.sigev_notify = SIGEV_THREAD,
                            .sigev_notify_function = timer_callback,
                            .sigev_value.sival_ptr = &callback};
  CHECK(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0);
  live = 1;
  CHECK(arm(timer, 40, 0) == 0);
  struct timespec deadline = st_timespec(st_now(CLOCK_REALTIME) + 2000000000);
  CHECK(sem_timedwait(&callback.completed, &deadline) == 0);
  CHECK(__atomic_load_n(&callback.count, __ATOMIC_ACQUIRE) == 1 &&
        __atomic_load_n(&callback.tid, __ATOMIC_ACQUIRE) != syscall(SYS_gettid));
  CHECK(timer_gettime(timer, &setting) == 0 && remaining(&setting) == 0);
  CHECK(timer_delete(timer) == 0);
  live = 0;
  int64_t deadline_exit = st_now(CLOCK_MONOTONIC) + 2000000000;
  while (syscall(SYS_tkill, callback.tid, 0) == 0 && st_now(CLOCK_MONOTONIC) < deadline_exit)
    st_pause(2);
  CHECK(syscall(SYS_tkill, callback.tid, 0) == -1 && errno == ESRCH);
out:
  if (live) {
    timer_delete(timer);
    st_pause(60);
  }
  if (worker_active) {
    pthread_cancel(worker);
    if (st_wait(&target.done, 1000) == 0)
      pthread_join(worker, NULL);
    else
      _exit(1);
  }
  if (sem_live)
    sem_destroy(&callback.completed);
  if (masked) {
    while (sigtimedwait(&target.set, &info, &zero) >= 0) {
    }
    pthread_sigmask(SIG_SETMASK, &original, NULL);
  }
  return failed;
}

int signal_timer_exec(int argc, char** argv) {
  if (argc != 3)
    return 2;
  alarm(5);
  timer_t old = (timer_t)(intptr_t)strtol(argv[2], NULL, 10), timer;
  struct itimerspec setting;
  if (timer_gettime(old, &setting) != -1 || errno != EINVAL)
    return 3;
  struct sigevent event = {.sigev_notify = SIGEV_NONE};
  if (timer_create(CLOCK_MONOTONIC, &event, &timer) || timer == old)
    return 4;
  if (timer_gettime(timer, &setting) || remaining(&setting) || timer_delete(timer))
    return 5;
  sigset_t set;
  sigemptyset(&set);
  sigaddset(&set, SIGRTMIN);
  struct timespec zero = {0};
  st_pause(150);
  if (sigtimedwait(&set, NULL, &zero) != -1 || errno != EAGAIN)
    return 6;
  return 0;
}
static int timer_lifetime(void) {
  int failed = 0, live = 0;
  timer_t timer;
  struct sigevent event = {.sigev_notify = SIGEV_NONE};
  struct itimerspec setting;
  pid_t child;
  CHECK(timer_create(CLOCK_MONOTONIC, &event, &timer) == 0);
  live = 1;
  CHECK(arm(timer, 30000, 0) == 0);
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(5);
    timer_t own;
    if (timer_gettime(timer, &setting) != -1 || errno != EINVAL)
      _exit(10);
    if (timer_create(CLOCK_MONOTONIC, &event, &own) || own == timer)
      _exit(11);
    if (timer_delete(own))
      _exit(12);
    _exit(0);
  }
  CHECK(st_reap(child, 6000) == 0);
  CHECK(timer_gettime(timer, &setting) == 0 && remaining(&setting) > 0);
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(5);
    sigset_t set;
    sigemptyset(&set);
    sigaddset(&set, SIGRTMIN);
    if (pthread_sigmask(SIG_BLOCK, &set, NULL))
      _exit(20);
    event = (struct sigevent){.sigev_notify = SIGEV_SIGNAL, .sigev_signo = SIGRTMIN};
    timer_t own;
    if (timer_create(CLOCK_MONOTONIC, &event, &own) || arm(own, 30, 30))
      _exit(21);
    st_pause(80);
    char identifier[32];
    snprintf(identifier, sizeof(identifier), "%ld", (long)(intptr_t)own);
    execl("/applications/signal-timer-contract-test", "signal-timer-contract-test", "timer-exec",
          identifier, (char*)NULL);
    _exit(22);
  }
  CHECK(st_reap(child, 6000) == 0);
out:
  if (live)
    timer_delete(timer);
  return failed;
}

int signal_timer_test_timers(void) {
  if (basic_timers() || delivered_timers() || threaded_timers() || timer_lifetime())
    return 1;
  return 0;
}
