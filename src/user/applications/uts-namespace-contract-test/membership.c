#define _GNU_SOURCE
#include <fcntl.h>
#include <pthread.h>
#include <sched.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

static int sharing_child(int command, int report, void* argument) {
  int original = *(int*)argument;
  if (ns_expect("initial", "initial-domain") || ns_send(report, 'R') || ns_receive(command, 'M') ||
      ns_set("shared", "shared-domain") || ns_send(report, 'M') || ns_receive(command, 'U') ||
      unshare(CLONE_NEWUTS) || ns_expect("shared", "shared-domain") ||
      ns_set("private", "private-domain") || ns_send(report, 'U') || ns_receive(command, 'J') ||
      setns(original, 0) || ns_expect("shared", "shared-domain") ||
      ns_set("joined", "joined-domain") || ns_send(report, 'J'))
    return 1;
  return 0;
}

static int fork_membership(void) {
  int failed = 0, original = -1;
  struct ns_peer child = NS_PEER_INITIALIZER;
  CHECK(ns_set("initial", "initial-domain") == 0);
  original = open("/proc/thread-self/ns/uts", O_RDONLY | O_CLOEXEC);
  CHECK(original >= 0);
  CHECK(ns_spawn(&child, sharing_child, &original) == 0);
  CHECK(ns_receive(child.report, 'R') == 0);
  CHECK(ns_send(child.command, 'M') == 0 && ns_receive(child.report, 'M') == 0);
  CHECK(ns_expect("shared", "shared-domain") == 0);
  CHECK(ns_send(child.command, 'U') == 0 && ns_receive(child.report, 'U') == 0);
  CHECK(ns_expect("shared", "shared-domain") == 0);
  CHECK(ns_send(child.command, 'J') == 0 && ns_receive(child.report, 'J') == 0);
  CHECK(ns_expect("joined", "joined-domain") == 0);
  CHECK(ns_join(&child) == 0);
out:
  ns_cleanup(&child);
  if (original >= 0)
    close(original);
  return failed;
}

static void* inherited_thread(void* argument) {
  const char* expected = argument;
  return (void*)(intptr_t)ns_expect(expected, expected);
}
static int inherited_child(int command, int report, void* argument) {
  (void)command;
  (void)report;
  return ns_expect(argument, argument);
}

struct thread_membership {
  int command[2], report[2];
  int original;
  int result;
};
static void* separate_thread(void* argument) {
  struct thread_membership* state = argument;
  struct ns_peer child = NS_PEER_INITIALIZER;
  pthread_t descendant;
  void* result = NULL;
  state->result = 1;
  if (ns_expect("leader", "leader") || unshare(CLONE_NEWUTS) || ns_set("worker", "worker") ||
      ns_send(state->report[1], 'B') || ns_receive(state->command[0], 'I'))
    return NULL;
  if (pthread_create(&descendant, NULL, inherited_thread, "worker") ||
      pthread_join(descendant, &result) || result || ns_spawn(&child, inherited_child, "worker") ||
      ns_join(&child))
    goto out;
  if (ns_send(state->report[1], 'I') || ns_receive(state->command[0], 'J') ||
      setns(state->original, CLONE_NEWUTS) || ns_expect("leader", "leader") ||
      ns_set("rejoined", "rejoined") || ns_send(state->report[1], 'J'))
    goto out;
  state->result = 0;
out:
  ns_cleanup(&child);
  return NULL;
}

static int thread_memberships(void) {
  int failed = 0, started = 0;
  struct thread_membership state = {.command = {-1, -1}, .report = {-1, -1}, .original = -1};
  pthread_t worker, descendant;
  void* result = NULL;
  CHECK(ns_set("leader", "leader") == 0);
  state.original = open("/proc/thread-self/ns/uts", O_RDONLY | O_CLOEXEC);
  CHECK(state.original >= 0);
  CHECK(pipe(state.command) == 0 && pipe(state.report) == 0);
  CHECK(pthread_create(&worker, NULL, separate_thread, &state) == 0);
  started = 1;
  CHECK(ns_receive(state.report[0], 'B') == 0);
  CHECK(ns_expect("leader", "leader") == 0);
  CHECK(pthread_create(&descendant, NULL, inherited_thread, "leader") == 0);
  CHECK(pthread_join(descendant, &result) == 0 && !result);
  CHECK(ns_send(state.command[1], 'I') == 0 && ns_receive(state.report[0], 'I') == 0);
  CHECK(ns_expect("leader", "leader") == 0);
  CHECK(ns_send(state.command[1], 'J') == 0 && ns_receive(state.report[0], 'J') == 0);
  CHECK(ns_expect("rejoined", "rejoined") == 0);
out:
  if (failed && state.command[1] >= 0) {
    close(state.command[1]);
    state.command[1] = -1;
  }
  if (started && (pthread_join(worker, NULL) || state.result))
    failed = 1;
  for (int i = 0; i < 2; ++i) {
    if (state.command[i] >= 0)
      close(state.command[i]);
    if (state.report[i] >= 0)
      close(state.report[i]);
  }
  if (state.original >= 0)
    close(state.original);
  return failed;
}

struct clone_state {
  struct ns_identity original;
  int report;
};
static int new_uts_child(void* argument) {
  struct clone_state* state = argument;
  struct ns_identity own;
  if (ns_expect("clone-parent", "clone-parent") ||
      ns_path_identity("/proc/thread-self/ns/uts", &own) || ns_same(own, state->original) ||
      ns_set("clone-child", "clone-child") || ns_send(state->report, 'C'))
    return 1;
  return 0;
}
static int clone_membership(void) {
  int failed = 0, report[2] = {-1, -1};
  pid_t child = -1;
  const size_t length = ns_page * 16;
  void* stack = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  struct clone_state state;
  CHECK(stack != MAP_FAILED);
  CHECK(ns_set("clone-parent", "clone-parent") == 0);
  CHECK(ns_path_identity("/proc/thread-self/ns/uts", &state.original) == 0);
  CHECK(pipe(report) == 0);
  state.report = report[1];
  child = clone(new_uts_child, (char*)stack + length, CLONE_NEWUTS | SIGCHLD, &state);
  CHECK(child > 0);
  CHECK(ns_receive(report[0], 'C') == 0);
  CHECK(ns_expect("clone-parent", "clone-parent") == 0);
  int status = ns_reap(child, 10000);
  child = -1;
  CHECK(status == 0);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    ns_reap(child, 1000);
  }
  for (int i = 0; i < 2; ++i)
    if (report[i] >= 0)
      close(report[i]);
  if (stack != MAP_FAILED)
    munmap(stack, length);
  return failed;
}

int ns_membership(void) {
  return fork_membership() || thread_memberships() || clone_membership();
}
