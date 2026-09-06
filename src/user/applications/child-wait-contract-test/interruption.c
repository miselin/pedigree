#define _GNU_SOURCE
#include <poll.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"

static volatile sig_atomic_t handler_report = -1;

static void caught(int signal) {
  int saved = errno;
  if (signal == SIGUSR1 && handler_report >= 0) {
    char value = 'H';
    ssize_t result = write(handler_report, &value, 1);
    (void)result;
  }
  errno = saved;
}

struct waiter {
  pid_t child;
  int use_waitpid;
  atomic_uint ready, done;
  int result, error, status;
  siginfo_t info;
};

static void* wait_for_child(void* argument) {
  struct waiter* waiter = argument;
  sigset_t mask;
  sigemptyset(&mask);
  sigaddset(&mask, SIGUSR1);
  pthread_sigmask(SIG_UNBLOCK, &mask, NULL);
  memset(&waiter->info, 0xa5, sizeof(waiter->info));
  atomic_store(&waiter->ready, 1);
  errno = 0;
  waiter->result = waiter->use_waitpid ? waitpid(waiter->child, &waiter->status, 0)
                                       : waitid(P_PID, waiter->child, &waiter->info, WEXITED);
  waiter->error = errno;
  atomic_store(&waiter->done, 1);
  return NULL;
}

static int interrupt_case(int use_waitpid, int restart) {
  int failed = 0, started = 0, installed = 0;
  int report[2] = {-1, -1};
  struct cw_child child = CW_CHILD_INIT;
  struct waiter waiter = {.use_waitpid = use_waitpid};
  struct sigaction action = {.sa_handler = caught, .sa_flags = restart ? SA_RESTART : 0}, old;
  pthread_t thread;
  CHECK(pipe(report) == 0);
  handler_report = report[1];
  sigemptyset(&action.sa_mask);
  CHECK(sigaction(SIGUSR1, &action, &old) == 0);
  installed = 1;
  CHECK(cw_spawn(&child, -1, (uid_t)-1, 96) == 0);
  waiter.child = child.pid;
  CHECK(pthread_create(&thread, NULL, wait_for_child, &waiter) == 0);
  started = 1;
  CHECK(cw_atomic_wait(&waiter.ready, 1) == 0);
  if (restart) {
    /* Each acknowledgment comes from the target handler; the child stays gated. */
    for (int i = 0; i < 8; ++i) {
      CHECK(!atomic_load(&waiter.done));
      CHECK(pthread_kill(thread, SIGUSR1) == 0 && cw_receive(report[0], 'H') == 0);
    }
    CHECK(!atomic_load(&waiter.done));
    CHECK(cw_send(child.command, 'E') == 0 && cw_atomic_wait(&waiter.done, 1) == 0);
    CHECK(waiter.result == (use_waitpid ? child.pid : 0));
    if (use_waitpid)
      CHECK(WIFEXITED(waiter.status) && WEXITSTATUS(waiter.status) == 96);
    else
      CHECK(cw_info(&waiter.info, child.pid, getuid(), CLD_EXITED, 96) == 0);
    child.pid = -1;
  } else {
    const int64_t end = cw_now() + INT64_C(5000000000);
    while (!atomic_load(&waiter.done) && cw_now() < end) {
      int result = pthread_kill(thread, SIGUSR1);
      CHECK(result == 0 || (result == ESRCH && atomic_load(&waiter.done)));
      struct pollfd pending = {.fd = report[0], .events = POLLIN};
      if (poll(&pending, 1, 20) > 0)
        CHECK(cw_receive(report[0], 'H') == 0);
      sched_yield();
    }
    CHECK(atomic_load(&waiter.done) && waiter.result == -1 && waiter.error == EINTR);
    if (!use_waitpid)
      CHECK(!cw_empty_info(&waiter.info));
    siginfo_t info;
    CHECK(waitid(P_PID, child.pid, &info, WEXITED | WNOHANG) == 0 && !cw_empty_info(&info));
    CHECK(cw_send(child.command, 'E') == 0 && waitid(P_PID, child.pid, &info, WEXITED) == 0);
    CHECK(cw_info(&info, child.pid, getuid(), CLD_EXITED, 96) == 0);
    child.pid = -1;
  }
out:
  cw_cleanup(&child);
  if (started) {
    if (!atomic_load(&waiter.done))
      pthread_cancel(thread);
    if (pthread_join(thread, NULL))
      failed = 1;
  }
  handler_report = -1;
  if (installed && sigaction(SIGUSR1, &old, NULL))
    failed = 1;
  if (report[0] >= 0)
    close(report[0]);
  if (report[1] >= 0)
    close(report[1]);
  return failed;
}

static int cancel_case(int use_waitpid) {
  int failed = 0, started = 0;
  struct cw_child child = CW_CHILD_INIT;
  struct waiter waiter = {.use_waitpid = use_waitpid};
  pthread_t thread;
  void* result = NULL;
  siginfo_t info;
  CHECK(cw_spawn(&child, -1, (uid_t)-1, 97) == 0);
  waiter.child = child.pid;
  CHECK(pthread_create(&thread, NULL, wait_for_child, &waiter) == 0);
  started = 1;
  CHECK(cw_atomic_wait(&waiter.ready, 1) == 0);
  CHECK(pthread_cancel(thread) == 0 && pthread_join(thread, &result) == 0);
  started = 0;
  CHECK(result == PTHREAD_CANCELED && !atomic_load(&waiter.done));
  CHECK(waitid(P_PID, child.pid, &info, WEXITED | WNOHANG) == 0 && !cw_empty_info(&info));
  CHECK(cw_send(child.command, 'E') == 0 && waitid(P_PID, child.pid, &info, WEXITED) == 0);
  CHECK(cw_info(&info, child.pid, getuid(), CLD_EXITED, 97) == 0);
  child.pid = -1;
out:
  cw_cleanup(&child);
  if (started) {
    pthread_cancel(thread);
    if (pthread_join(thread, NULL))
      failed = 1;
  }
  return failed;
}

int cw_interruption(void) {
  for (int use_waitpid = 0; use_waitpid < 2; ++use_waitpid)
    if (interrupt_case(use_waitpid, 0) || interrupt_case(use_waitpid, 1) ||
        cancel_case(use_waitpid))
      return 1;
  return 0;
}
