#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"

struct outsider {
  pid_t child;
  int peek_result, wait_result, status;
  siginfo_t peek;
  long result[3];
  int error[3];
  struct user_regs_struct registers;
  siginfo_t info;
};

static void* inspect_sibling(void* argument) {
  struct outsider* state = argument;
  memset(&state->registers, 0xa5, sizeof(state->registers));
  memset(&state->info, 0xa5, sizeof(state->info));
  state->peek_result = waitid(P_PID, state->child, &state->peek, WSTOPPED | WNOWAIT);
  state->wait_result = tc_wait(state->child, &state->status, 10000);
  errno = 0;
  state->result[0] = ptrace(PTRACE_GETREGS, state->child, (void*)0, (void*)&state->registers);
  state->error[0] = errno;
  errno = 0;
  state->result[1] = ptrace(PTRACE_GETSIGINFO, state->child, (void*)0, (void*)&state->info);
  state->error[1] = errno;
  errno = 0;
  state->result[2] = ptrace(PTRACE_CONT, state->child, (void*)0, (void*)0);
  state->error[2] = errno;
  return NULL;
}

static int sibling_denied(void) {
  int failed = 0, started = 0;
  struct tc_child child = TC_CHILD_INIT;
  struct outsider state = {0};
  struct user_regs_struct first, second;
  siginfo_t peek;
  pthread_t sibling;
  CHECK(tc_spawn(&child, 1) == 0 && tc_send(&child, 'S', SIGUSR1) == 0);
  CHECK(waitid(P_PID, child.pid, &peek, WSTOPPED | WNOWAIT) == 0);
  CHECK(peek.si_pid == child.pid && peek.si_code == CLD_TRAPPED && peek.si_status == SIGUSR1);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&first) == 0);
  state.child = child.pid;
  CHECK(pthread_create(&sibling, NULL, inspect_sibling, &state) == 0);
  started = 1;
  CHECK(pthread_join(sibling, NULL) == 0);
  started = 0;
  CHECK(state.peek_result == 0 && state.peek.si_pid == child.pid &&
        state.peek.si_code == CLD_TRAPPED && state.peek.si_status == SIGUSR1);
  CHECK(state.wait_result == 0 && WIFSTOPPED(state.status) && WSTOPSIG(state.status) == SIGUSR1);
  for (size_t i = 0; i < 3; ++i)
    CHECK(state.result[i] == -1 && state.error[i] == ESRCH);
  const unsigned char* bytes = (const unsigned char*)&state.registers;
  for (size_t i = 0; i < sizeof(state.registers); ++i)
    CHECK(bytes[i] == 0xa5);
  bytes = (const unsigned char*)&state.info;
  for (size_t i = 0; i < sizeof(state.info); ++i)
    CHECK(bytes[i] == 0xa5);
  CHECK(ptrace(PTRACE_GETREGS, child.pid, (void*)0, (void*)&second) == 0);
  CHECK(memcmp(&first, &second, sizeof(first)) == 0);
  CHECK(waitid(P_PID, child.pid, &peek, WSTOPPED | WNOHANG) == 0 && !peek.si_pid);
  CHECK(tc_resume(child.pid, PTRACE_CONT, 0) == 0 && tc_receive(child.report, 'A', NULL) == 0);
  CHECK(tc_finish(&child) == 0);
out:
  if (started && pthread_join(sibling, NULL))
    failed = 1;
  tc_close(&child);
  return failed;
}

struct creator {
  struct tc_child child;
  atomic_uint ready, finish;
  int result;
};

static void* create_then_exit(void* argument) {
  struct creator* state = argument;
  state->result = tc_spawn(&state->child, 1) || tc_stop(&state->child, SIGUSR1);
  atomic_store(&state->ready, 1);
  if (!state->result && tc_atomic_wait(&state->finish, 1))
    state->result = -1;
  // The relation must retire with this task even though its Process stays alive.
  return NULL;
}

static int tracer_task_exit(void) {
  int failed = 0, started = 0;
  struct creator state = {.child = TC_CHILD_INIT};
  struct tc_message message;
  struct user_regs_struct registers;
  pthread_t creator;
  CHECK(pthread_create(&creator, NULL, create_then_exit, &state) == 0);
  started = 1;
  CHECK(tc_atomic_wait(&state.ready, 1) == 0 && state.result == 0);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGS, state.child.pid, (void*)0, (void*)&registers) == -1 &&
        errno == ESRCH);
  atomic_store(&state.finish, 1);
  CHECK(pthread_join(creator, NULL) == 0);
  started = 0;
  CHECK(state.result == 0);
  CHECK(tc_receive(state.child.report, 'H', &message) == 0);
  CHECK(message.signal == SIGUSR1 && message.code == SI_TKILL && message.pid == state.child.pid &&
        message.uid == getuid());
  CHECK(tc_receive(state.child.report, 'A', NULL) == 0);
  errno = 0;
  CHECK(ptrace(PTRACE_GETREGS, state.child.pid, (void*)0, (void*)&registers) == -1 &&
        errno == ESRCH);
  CHECK(tc_finish(&state.child) == 0);
out:
  atomic_store(&state.finish, 1);
  if (started && pthread_join(creator, NULL))
    failed = 1;
  tc_close(&state.child);
  return failed;
}

int tc_ownership(void) {
  return sibling_denied() || tracer_task_exit();
}
