#define _GNU_SOURCE
#include <unistd.h>

#include "contract.h"
#include <sys/fsuid.h>
#include <sys/syscall.h>

enum { OWNER_REAL = 51001, OWNER_EFFECTIVE = 51002, OWNER_SAVED = 51003, FOREIGN = 51004 };
struct target_state {
  atomic_uint ready;
  pid_t tid;
  int command, report;
};
struct target_report {
  unsigned cpu;
  int count, failed;
};
static void* target_entry(void* argument) {
  struct target_state* state = argument;
  if (sc_pin(0, sc_cpus[0]))
    return (void*)1;
  state->tid = (pid_t)syscall(SYS_gettid);
  atomic_store_explicit(&state->ready, 1, memory_order_release);
  for (;;) {
    char command;
    if (sc_read(state->command, &command, 1))
      return (void*)1;
    if (command == 'Q')
      return NULL;
    if (command != 'S')
      return (void*)1;
    unsigned node = UINT32_MAX;
    cpu_set_t mask;
    struct target_report result = {.cpu = UINT32_MAX};
    result.failed = syscall(SYS_getcpu, &result.cpu, &node, NULL) || node ||
                    sched_getaffinity(0, sizeof(mask), &mask);
    result.count = result.failed ? -1 : CPU_COUNT(&mask);
    if (sc_write(state->report, &result, sizeof(result)))
      return (void*)1;
  }
}
static int target_child(int command, int report, void* argument) {
  (void)argument;
  if (setresuid(OWNER_REAL, OWNER_EFFECTIVE, OWNER_SAVED))
    return 1;
  int gate[2];
  if (pipe(gate))
    return 1;
  struct target_state state = {.command = gate[0], .report = report};
  pthread_t thread;
  if (pthread_create(&thread, NULL, target_entry, &state))
    return 1;
  if (sc_wait(&state.ready, 1) || sc_write(report, &state.tid, sizeof(state.tid)))
    _exit(2);
  for (;;) {
    char byte;
    if (sc_read(command, &byte, 1) || (byte != 'S' && byte != 'Q') || sc_send(gate[1], byte))
      _exit(3);
    if (byte == 'Q')
      break;
  }
  void* result;
  int failed = pthread_join(thread, &result) || result;
  close(gate[0]);
  close(gate[1]);
  return failed;
}
struct permission_case {
  pid_t target;
  uid_t real, effective, filesystem;
  int allowed, destination;
};
static int permission_child(int command, int report, void* argument) {
  (void)command;
  (void)report;
  struct permission_case* test = argument;
  int failed = 0;
  CHECK(setresuid(test->real, test->effective, 0) == 0);
  setfsuid(test->filesystem);
  CHECK((uid_t)setfsuid((uid_t)-1) == test->filesystem);
  CHECK(geteuid() == test->effective);
  cpu_set_t before, after;
  struct sched_param param = {.sched_priority = 0};
  CHECK(sched_getaffinity(test->target, sizeof(before), &before) == 0);
  CHECK(syscall(SYS_sched_getscheduler, test->target) == SCHED_OTHER);
  CHECK(syscall(SYS_sched_getparam, test->target, &param) == 0 && !param.sched_priority);
  errno = 0;
  int result = sc_pin(test->target, test->destination);
  CHECK(test->allowed ? result == 0 : result == -1 && errno == EPERM);
  CHECK(sched_getaffinity(test->target, sizeof(after), &after) == 0);
  CHECK(test->allowed ? sc_mask(test->target, test->destination) == 0 : CPU_EQUAL(&before, &after));
  errno = 0;
  long policy = syscall(SYS_sched_setparam, test->target, &param);
  CHECK(test->allowed ? policy == 0 : policy == -1 && errno == EPERM);
  errno = 0;
  policy = syscall(SYS_sched_setscheduler, test->target, SCHED_OTHER, &param);
  CHECK(test->allowed ? policy == 0 : policy == -1 && errno == EPERM);
  CHECK(syscall(SYS_sched_getscheduler, test->target) == SCHED_OTHER);
out:
  return failed;
}
int sc_permissions(void) {
  int failed = 0;
  struct sc_peer target = SC_PEER_INITIALIZER, caller = SC_PEER_INITIALIZER;
  CHECK(geteuid() == 0);
  CHECK(sc_spawn(&target, target_child, NULL) == 0);
  pid_t tid;
  CHECK(sc_read(target.report, &tid, sizeof(tid)) == 0 && tid > 0 && tid != target.pid);
  struct permission_case cases[] = {
      {.real = FOREIGN, .effective = OWNER_REAL, .filesystem = FOREIGN, .allowed = 1},
      {.real = FOREIGN, .effective = OWNER_EFFECTIVE, .filesystem = FOREIGN, .allowed = 1},
      {.real = OWNER_REAL, .effective = FOREIGN, .filesystem = OWNER_REAL, .allowed = 0},
      {.real = FOREIGN, .effective = OWNER_SAVED, .filesystem = FOREIGN, .allowed = 0},
      {.real = FOREIGN, .effective = FOREIGN, .filesystem = 0, .allowed = 0},
      {.real = FOREIGN, .effective = 0, .filesystem = FOREIGN, .allowed = 1}};
  for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
    cases[i].target = tid;
    cases[i].destination = sc_cpus[sc_count - 1];
    CHECK(sc_pin(tid, sc_cpus[0]) == 0);
    CHECK(sc_spawn(&caller, permission_child, &cases[i]) == 0);
    CHECK(sc_join(&caller) == 0);
    CHECK(sc_send(target.command, 'S') == 0);
    struct target_report result;
    CHECK(sc_read(target.report, &result, sizeof(result)) == 0);
    int expected = cases[i].allowed ? cases[i].destination : sc_cpus[0];
    CHECK(!result.failed && result.count == 1 && result.cpu == (unsigned)expected);
    // Targeting its worker TID must not mutate the leader's independent mask.
    cpu_set_t leader;
    CHECK(sched_getaffinity(target.pid, sizeof(leader), &leader) == 0 &&
          CPU_EQUAL(&leader, &sc_allowed));
  }
  CHECK(sc_send(target.command, 'Q') == 0 && sc_join(&target) == 0);
out:
  sc_cleanup(&caller);
  sc_cleanup(&target);
  return failed;
}
