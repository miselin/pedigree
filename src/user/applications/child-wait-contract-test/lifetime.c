#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"

static int64_t microseconds(struct timeval time) {
  return (int64_t)time.tv_sec * 1000000 + time.tv_usec;
}

static int repeated_exit(void) {
  int failed = 0, status;
  struct cw_child child = CW_CHILD_INIT;
  siginfo_t info;
  struct rusage before, middle, after, first, usage;
  memset(&before, 0xa5, sizeof(before));
  memset(&middle, 0xa5, sizeof(middle));
  memset(&after, 0xa5, sizeof(after));
  memset(&first, 0xa5, sizeof(first));
  CHECK(getrusage(RUSAGE_CHILDREN, &before) == 0);
  CHECK(cw_spawn(&child, -1, (uid_t)-1, 83) == 0);
  CHECK(cw_send(child.command, 'B') == 0 && cw_receive(child.report, 'B') == 0);
  CHECK(cw_send(child.command, 'E') == 0);
  CHECK(cw_raw_waitid(P_PID, child.pid, &info, WEXITED | WNOWAIT, &first) == 0);
  CHECK(cw_info(&info, child.pid, getuid(), CLD_EXITED, 83) == 0 && !cw_usage(&first));
  for (int i = 0; i < 4; ++i) {
    memset(&usage, 0xa5, sizeof(usage));
    CHECK(cw_raw_waitid(P_PID, child.pid, &info, WEXITED | WNOWAIT, &usage) == 0);
    CHECK(cw_info(&info, child.pid, getuid(), CLD_EXITED, 83) == 0);
    CHECK(memcmp(&usage, &first, CW_RUSAGE_BYTES) == 0 && !cw_usage(&usage));
  }
  CHECK(getrusage(RUSAGE_CHILDREN, &middle) == 0);
  CHECK(microseconds(before.ru_utime) == microseconds(middle.ru_utime));
  CHECK(microseconds(before.ru_stime) == microseconds(middle.ru_stime));
  memset(&usage, 0xa5, sizeof(usage));
  CHECK(wait4(child.pid, &status, 0, &usage) == child.pid);
  child.pid = -1;
  CHECK(WIFEXITED(status) && WEXITSTATUS(status) == 83);
  CHECK(memcmp(&usage, &first, CW_RUSAGE_BYTES) == 0 && !cw_usage(&usage));
  CHECK(getrusage(RUSAGE_CHILDREN, &after) == 0);
  const int64_t user_delta = microseconds(after.ru_utime) - microseconds(before.ru_utime);
  const int64_t system_delta = microseconds(after.ru_stime) - microseconds(before.ru_stime);
  CHECK(user_delta >= microseconds(usage.ru_utime) &&
        user_delta <= microseconds(usage.ru_utime) + 1);
  CHECK(system_delta >= microseconds(usage.ru_stime) &&
        system_delta <= microseconds(usage.ru_stime) + 1);
out:
  cw_cleanup(&child);
  return failed;
}

struct contender {
  pid_t child;
  int use_wait4;
  atomic_uint* start;
  atomic_uint ready, done;
  long result;
  int error, status;
  siginfo_t info;
};

static void* compete(void* argument) {
  struct contender* waiter = argument;
  atomic_store(&waiter->ready, 1);
  if (cw_atomic_wait(waiter->start, 1)) {
    waiter->result = -2;
  } else {
    errno = 0;
    waiter->result = waiter->use_wait4 ? wait4(waiter->child, &waiter->status, 0, NULL)
                                       : waitid(P_PID, waiter->child, &waiter->info, WEXITED);
    waiter->error = errno;
  }
  atomic_store(&waiter->done, 1);
  return NULL;
}

static int competing_reapers(void) {
  int failed = 0, created = 0;
  struct cw_child child = CW_CHILD_INIT;
  pthread_t threads[2];
  atomic_uint start = 0;
  struct contender contenders[2] = {{.use_wait4 = 0, .start = &start},
                                    {.use_wait4 = 1, .start = &start}};
  CHECK(cw_spawn(&child, -1, (uid_t)-1, 87) == 0);
  for (int i = 0; i < 2; ++i) {
    contenders[i].child = child.pid;
    CHECK(pthread_create(&threads[i], NULL, compete, &contenders[i]) == 0);
    ++created;
    CHECK(cw_atomic_wait(&contenders[i].ready, 1) == 0);
  }
  atomic_store(&start, 1);
  CHECK(cw_send(child.command, 'E') == 0);
  CHECK(cw_atomic_wait(&contenders[0].done, 1) == 0 && cw_atomic_wait(&contenders[1].done, 1) == 0);
  pid_t pid = child.pid;
  child.pid = -1;
  CHECK((contenders[0].result == 0) + (contenders[1].result == pid) == 1);
  for (int i = 0; i < 2; ++i) {
    if (contenders[i].result < 0)
      CHECK(contenders[i].result == -1 && contenders[i].error == ECHILD);
    else if (i == 0)
      CHECK(cw_info(&contenders[i].info, pid, getuid(), CLD_EXITED, 87) == 0);
    else
      CHECK(WIFEXITED(contenders[i].status) && WEXITSTATUS(contenders[i].status) == 87);
  }
  CHECK(waitpid(pid, NULL, WNOHANG) == -1 && errno == ECHILD);
out:
  atomic_store(&start, 1);
  cw_cleanup(&child);
  for (int i = 0; i < created; ++i)
    if (pthread_join(threads[i], NULL))
      failed = 1;
  return failed;
}

struct sibling {
  struct cw_child child;
  int result;
};

static void* sibling_fork(void* argument) {
  struct sibling* state = argument;
  state->result = cw_spawn(&state->child, -1, (uid_t)-1, 91);
  return NULL;
}

static int sibling_child(void) {
  int failed = 0, started = 0;
  pthread_t creator;
  struct sibling state = {.child = CW_CHILD_INIT, .result = -1};
  siginfo_t info;
  CHECK(pthread_create(&creator, NULL, sibling_fork, &state) == 0);
  started = 1;
  CHECK(pthread_join(creator, NULL) == 0);
  started = 0;
  CHECK(state.result == 0 && cw_send(state.child.command, 'E') == 0);
  CHECK(waitid(P_PID, state.child.pid, &info, WEXITED | WNOWAIT) == 0);
  CHECK(cw_info(&info, state.child.pid, getuid(), CLD_EXITED, 91) == 0);
  CHECK(waitid(P_ALL, 0, &info, WEXITED) == 0 && info.si_pid == state.child.pid);
  state.child.pid = -1;
out:
  if (started)
    pthread_join(creator, NULL);
  cw_cleanup(&state.child);
  return failed;
}

int cw_lifetime(void) {
  return repeated_exit() || competing_reapers() || sibling_child();
}
