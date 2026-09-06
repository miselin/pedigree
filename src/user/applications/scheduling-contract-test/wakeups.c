#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/syscall.h>

struct condition_waiter {
  pthread_mutex_t mutex;
  pthread_cond_t condition;
  atomic_uint ready;
  pid_t tid;
  int released, destination, cleanup_ok;
};
static void condition_cleanup(void* argument) {
  struct condition_waiter* waiter = argument;
  waiter->cleanup_ok = sc_tls == 0x8291 && !sc_sample(waiter->destination);
  pthread_mutex_unlock(&waiter->mutex);
}
static void* condition_entry(void* argument) {
  struct condition_waiter* waiter = argument;
  if (sc_pin(0, sc_cpus[0]))
    return (void*)1;
  sc_tls = 0x8291;
  waiter->tid = (pid_t)syscall(SYS_gettid);
  int result = 0;
  pthread_mutex_lock(&waiter->mutex);
  pthread_cleanup_push(condition_cleanup, waiter);
  atomic_store_explicit(&waiter->ready, 1, memory_order_release);
  while (!waiter->released && !result)
    result = pthread_cond_wait(&waiter->condition, &waiter->mutex);
  result |= sc_sample(waiter->destination) || sc_tls != 0x8291;
  pthread_cleanup_pop(0);
  pthread_mutex_unlock(&waiter->mutex);
  return (void*)(intptr_t)result;
}
static int condition_wakeup(int cancel) {
  int failed = 0, created = 0, locked = 0;
  struct condition_waiter waiter = {.mutex = PTHREAD_MUTEX_INITIALIZER,
                                    .condition = PTHREAD_COND_INITIALIZER,
                                    .destination = sc_cpus[sc_count - 1]};
  pthread_t thread;
  CHECK(pthread_create(&thread, NULL, condition_entry, &waiter) == 0);
  created = 1;
  CHECK(sc_wait(&waiter.ready, 1) == 0);
  CHECK(pthread_mutex_lock(&waiter.mutex) == 0);
  locked = 1;
  // Acquiring this mutex follows the peer's logical cond-wait enrollment.
  CHECK(sc_pin(waiter.tid, waiter.destination) == 0);
  CHECK(sc_mask(waiter.tid, waiter.destination) == 0);
  if (cancel) {
    CHECK(pthread_cancel(thread) == 0);
  } else {
    waiter.released = 1;
    CHECK(pthread_cond_signal(&waiter.condition) == 0);
  }
  CHECK(pthread_mutex_unlock(&waiter.mutex) == 0);
  locked = 0;
  void* result;
  CHECK(pthread_join(thread, &result) == 0);
  created = 0;
  CHECK(cancel ? result == PTHREAD_CANCELED && waiter.cleanup_ok : result == NULL);
out:
  if (locked)
    pthread_mutex_unlock(&waiter.mutex);
  if (created) {
    pthread_cancel(thread);
    pthread_join(thread, NULL);
  }
  pthread_cond_destroy(&waiter.condition);
  pthread_mutex_destroy(&waiter.mutex);
  return failed;
}

struct io_waiter {
  atomic_uint ready, handled;
  pid_t tid;
  int fd, mode, destination;
  unsigned handler_cpu;
  int handler_ok;
};
static _Thread_local struct io_waiter* current_waiter;
static void migration_signal(int number) {
  struct io_waiter* waiter = current_waiter;
  if (!waiter)
    return;
  int saved = errno;
  unsigned cpu = UINT32_MAX, node = UINT32_MAX;
  long result = syscall(SYS_getcpu, &cpu, &node, NULL);
  waiter->handler_cpu = cpu;
  waiter->handler_ok = number == SIGUSR1 && !result && !node && sc_tls == 0x3498;
  atomic_store_explicit(&waiter->handled, 1, memory_order_release);
  errno = saved;
}
static void* io_entry(void* argument) {
  struct io_waiter* waiter = argument;
  if (sc_pin(0, sc_cpus[0]))
    return (void*)1;
  sc_tls = 0x3498;
  current_waiter = waiter;
  waiter->tid = (pid_t)syscall(SYS_gettid);
  sigset_t one;
  sigemptyset(&one);
  sigaddset(&one, SIGUSR1);
  if (pthread_sigmask(SIG_UNBLOCK, &one, NULL))
    return (void*)1;
  atomic_store_explicit(&waiter->ready, 1, memory_order_release);
  int error = 0;
  if (waiter->mode) {
    struct pollfd event = {.fd = waiter->fd, .events = POLLIN};
    int result;
    do
      result = poll(&event, 1, 10000);
    while (result < 0 && errno == EINTR);
    error = result != 1 || !(event.revents & POLLIN);
  }
  char byte = 0;
  ssize_t count;
  do
    count = read(waiter->fd, &byte, 1);
  while (count < 0 && errno == EINTR);
  error |= count != 1 || byte != 'W' || sc_sample(waiter->destination) || sc_tls != 0x3498;
  current_waiter = NULL;
  return (void*)(intptr_t)error;
}
static int io_wakeup(int mode) {
  int failed = 0, created = 0, action_set = 0;
  int pipefd[2] = {-1, -1};
  struct sigaction action = {.sa_handler = migration_signal}, previous;
  sigemptyset(&action.sa_mask);
  struct io_waiter waiter = {.mode = mode, .destination = sc_cpus[sc_count - 1]};
  pthread_t thread;
  CHECK(sigaction(SIGUSR1, &action, &previous) == 0);
  action_set = 1;
  CHECK(pipe(pipefd) == 0);
  waiter.fd = pipefd[0];
  CHECK(pthread_create(&thread, NULL, io_entry, &waiter) == 0);
  created = 1;
  CHECK(sc_wait(&waiter.ready, 1) == 0);
  CHECK(sc_pin(waiter.tid, waiter.destination) == 0);
  CHECK(pthread_kill(thread, SIGUSR1) == 0);
  CHECK(sc_wait(&waiter.handled, 1) == 0 && waiter.handler_ok &&
        waiter.handler_cpu == (unsigned)waiter.destination);
  CHECK(sc_send(pipefd[1], 'W') == 0);
  void* result;
  CHECK(pthread_join(thread, &result) == 0);
  created = 0;
  CHECK(result == NULL);
out:
  if (created) {
    pthread_cancel(thread);
    pthread_join(thread, NULL);
  }
  if (pipefd[0] >= 0)
    close(pipefd[0]);
  if (pipefd[1] >= 0)
    close(pipefd[1]);
  if (action_set)
    sigaction(SIGUSR1, &previous, NULL);
  return failed;
}
static void* signal_entry(void* argument) {
  struct io_waiter* waiter = argument;
  if (sc_pin(0, sc_cpus[0]))
    return (void*)1;
  sc_tls = 0x4289;
  waiter->tid = (pid_t)syscall(SYS_gettid);
  atomic_store_explicit(&waiter->ready, 1, memory_order_release);
  sigset_t one;
  sigemptyset(&one);
  sigaddset(&one, SIGUSR2);
  struct timespec timeout = {10, 0};
  siginfo_t info;
  int result = sigtimedwait(&one, &info, &timeout);
  return (void*)(intptr_t)(result != SIGUSR2 || info.si_signo != SIGUSR2 ||
                           sc_sample(waiter->destination) || sc_tls != 0x4289);
}
static int signal_wakeup(void) {
  int failed = 0, created = 0, masked = 0;
  sigset_t one, old;
  sigemptyset(&one);
  sigaddset(&one, SIGUSR2);
  struct io_waiter waiter = {.destination = sc_cpus[sc_count - 1]};
  pthread_t thread;
  CHECK(pthread_sigmask(SIG_BLOCK, &one, &old) == 0);
  masked = 1;
  CHECK(pthread_create(&thread, NULL, signal_entry, &waiter) == 0);
  created = 1;
  CHECK(sc_wait(&waiter.ready, 1) == 0 && sc_pin(waiter.tid, waiter.destination) == 0);
  CHECK(pthread_kill(thread, SIGUSR2) == 0);
  void* result;
  CHECK(pthread_join(thread, &result) == 0);
  created = 0;
  CHECK(result == NULL);
out:
  if (created) {
    pthread_cancel(thread);
    pthread_join(thread, NULL);
  }
  if (masked)
    pthread_sigmask(SIG_SETMASK, &old, NULL);
  return failed;
}
int sc_wakeups(void) {
  return condition_wakeup(0) || condition_wakeup(1) || io_wakeup(0) || io_wakeup(1) ||
         signal_wakeup();
}
