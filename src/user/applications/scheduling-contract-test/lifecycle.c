#define _GNU_SOURCE
#include <signal.h>
#include <stdlib.h>
#include <unistd.h>

#include "contract.h"
#include <sys/syscall.h>

struct inheritance {
  int cpu;
  pid_t tid;
};
static void* inherited_entry(void* argument) {
  struct inheritance* state = argument;
  state->tid = (pid_t)syscall(SYS_gettid);
  int policy;
  struct sched_param param;
  return (void*)(intptr_t)(sc_mask(0, state->cpu) || sc_sample(state->cpu) ||
                           pthread_getschedparam(pthread_self(), &policy, &param) ||
                           policy != SCHED_OTHER || param.sched_priority);
}
static int check_inherited_thread(int cpu, pid_t* tid) {
  struct inheritance state = {.cpu = cpu};
  pthread_t thread;
  if (pthread_create(&thread, NULL, inherited_entry, &state))
    return -1;
  void* result;
  if (pthread_join(thread, &result) || result)
    return -1;
  if (tid)
    *tid = state.tid;
  return 0;
}
static void* creator_entry(void* argument) {
  int cpu = *(int*)argument;
  if (sc_pin(0, cpu) || check_inherited_thread(cpu, NULL))
    return (void*)1;
  pid_t child = fork();
  if (!child)
    _exit(sc_mask(0, cpu) || sc_sample(cpu) ? 1 : 0);
  return (void*)(intptr_t)(child < 0 || sc_reap(child, 10000));
}
static int inheritance(void) {
  int failed = 0;
  CHECK(sc_pin(0, sc_cpus[0]) == 0);
  pid_t retired;
  CHECK(check_inherited_thread(sc_cpus[0], &retired) == 0);
  cpu_set_t mask;
  struct sched_param param = {.sched_priority = 0};
  errno = 0;
  CHECK(sched_getaffinity(retired, sizeof(mask), &mask) == -1 && errno == ESRCH);
  errno = 0;
  CHECK(sc_pin(retired, sc_cpus[0]) == -1 && errno == ESRCH);
  errno = 0;
  CHECK(syscall(SYS_sched_getparam, retired, &param) == -1 && errno == ESRCH);
  errno = 0;
  CHECK(syscall(SYS_sched_getscheduler, retired) == -1 && errno == ESRCH);
  errno = 0;
  CHECK(syscall(SYS_sched_setparam, retired, &param) == -1 && errno == ESRCH);
  errno = 0;
  CHECK(syscall(SYS_sched_setscheduler, retired, SCHED_OTHER, &param) == -1 && errno == ESRCH);
  pthread_t creator;
  CHECK(pthread_create(&creator, NULL, creator_entry, &sc_cpus[sc_count - 1]) == 0);
  void* result;
  CHECK(pthread_join(creator, &result) == 0 && result == NULL);
  CHECK(sc_mask(0, sc_cpus[0]) == 0 && sc_sample(sc_cpus[0]) == 0);
out:
  return failed;
}
int sc_exec(int argc, char** argv) {
  int failed = 0;
  alarm(15);
  CHECK(argc == 3);
  int cpu = atoi(argv[2]);
  CHECK(cpu >= 0 && cpu < CPU_SETSIZE);
  CHECK(sc_mask(0, cpu) == 0 && sc_sample(cpu) == 0);
  CHECK((pid_t)syscall(SYS_gettid) == getpid());
  CHECK(check_inherited_thread(cpu, NULL) == 0);
out:
  return failed;
}
static int enter_exec(int cpu) {
  if (sc_pin(0, cpu))
    return 1;
  execl("/applications/scheduling-no-such-image", "absent", (char*)NULL);
  if (errno != ENOENT || sc_mask(0, cpu) || sc_sample(cpu))
    return 1;
  char text[16];
  snprintf(text, sizeof(text), "%d", cpu);
  execl(SC_APP, SC_APP, "schedule-exec", text, (char*)NULL);
  return 1;
}
static void* exec_entry(void* argument) {
  _exit(enter_exec(*(int*)argument));
}
static int exec_child(int command, int report, void* argument) {
  (void)command;
  (void)report;
  if (sc_pin(0, sc_cpus[0]))
    return 1;
  if (!*(int*)argument)
    return enter_exec(sc_cpus[sc_count - 1]);
  pthread_t worker;
  if (pthread_create(&worker, NULL, exec_entry, &sc_cpus[sc_count - 1]))
    return 1;
  for (;;)
    pause();
}
static int exec_inheritance(void) {
  int failed = 0;
  struct sc_peer peer = SC_PEER_INITIALIZER;
  for (int threaded = 0; threaded < 2; ++threaded) {
    CHECK(sc_spawn(&peer, exec_child, &threaded) == 0);
    CHECK(sc_join(&peer) == 0);
  }
out:
  sc_cleanup(&peer);
  return failed;
}

struct competition {
  atomic_uint ready, start, stop, sample, sampled;
  pid_t tid;
  unsigned final_cpu;
  int failed;
};
struct setter {
  struct competition* state;
  int reverse, exit_race;
};
static void* competition_target(void* argument) {
  struct competition* state = argument;
  if (sc_pin(0, sc_cpus[0]))
    return (void*)1;
  state->tid = (pid_t)syscall(SYS_gettid);
  atomic_store_explicit(&state->ready, 1, memory_order_release);
  while (!atomic_load_explicit(&state->stop, memory_order_acquire)) {
    if (atomic_load_explicit(&state->sample, memory_order_acquire) &&
        !atomic_load_explicit(&state->sampled, memory_order_relaxed)) {
      unsigned cpu, node;
      state->failed = syscall(SYS_getcpu, &cpu, &node, NULL) || node;
      state->final_cpu = cpu;
      atomic_store_explicit(&state->sampled, 1, memory_order_release);
    }
    sched_yield();
  }
  return NULL;
}
static void* competition_setter(void* argument) {
  struct setter* setter = argument;
  if (sc_wait(&setter->state->start, 1))
    return (void*)1;
  for (int i = 0; i < (setter->exit_race ? 1 : 8); ++i) {
    int cpu = sc_cpus[(i + setter->reverse) % 2 ? sc_count - 1 : 0];
    int result = sc_pin(setter->state->tid, cpu);
    if (result && !(setter->exit_race && errno == ESRCH))
      return (void*)1;
  }
  return NULL;
}
static int competing_requests(void) {
  int failed = 0, target_live = 0, created = 0;
  struct competition state = {0};
  pthread_t target, controllers[2];
  struct setter setters[2] = {{&state, 0, 0}, {&state, 1, 0}};
  CHECK(pthread_create(&target, NULL, competition_target, &state) == 0);
  target_live = 1;
  CHECK(sc_wait(&state.ready, 1) == 0);
  for (int i = 0; i < 2; ++i) {
    CHECK(pthread_create(&controllers[i], NULL, competition_setter, &setters[i]) == 0);
    ++created;
  }
  atomic_store_explicit(&state.start, 1, memory_order_release);
  for (int i = 0; i < created; ++i) {
    void* result;
    if (pthread_join(controllers[i], &result) || result)
      failed = 1;
  }
  created = 0;
  CHECK(!failed);
  CHECK(sc_pin(state.tid, sc_cpus[sc_count - 1]) == 0);
  atomic_store_explicit(&state.sample, 1, memory_order_release);
  CHECK(sc_wait(&state.sampled, 1) == 0 && !state.failed &&
        state.final_cpu == (unsigned)sc_cpus[sc_count - 1]);
out:
  atomic_store_explicit(&state.start, 1, memory_order_release);
  for (int i = 0; i < created; ++i)
    pthread_join(controllers[i], NULL);
  atomic_store_explicit(&state.stop, 1, memory_order_release);
  if (target_live) {
    void* result;
    if (pthread_join(target, &result) || result)
      failed = 1;
  }
  return failed;
}
static int exit_requests(void) {
  int failed = 0;
  for (int i = 0; i < 4; ++i) {
    struct competition state = {0};
    struct setter setter = {&state, 1, 1};
    pthread_t target, controller;
    CHECK(pthread_create(&target, NULL, competition_target, &state) == 0);
    if (sc_wait(&state.ready, 1)) {
      atomic_store(&state.stop, 1);
      pthread_join(target, NULL);
      CHECK(0);
    }
    if (pthread_create(&controller, NULL, competition_setter, &setter)) {
      atomic_store(&state.stop, 1);
      pthread_join(target, NULL);
      CHECK(0);
    }
    atomic_store_explicit(&state.start, 1, memory_order_release);
    atomic_store_explicit(&state.stop, 1, memory_order_release);
    void* changed;
    void* stopped;
    int result = pthread_join(controller, &changed);
    result |= pthread_join(target, &stopped);
    CHECK(!result && !changed && !stopped);
    errno = 0;
    CHECK(sc_pin(state.tid, sc_cpus[0]) == -1 && errno == ESRCH);
  }
out:
  return failed;
}
int sc_lifecycle(void) {
  return inheritance() || exec_inheritance() || competing_requests() || exit_requests();
}
