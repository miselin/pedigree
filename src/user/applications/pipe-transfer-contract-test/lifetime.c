#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/socket.h>

static int queued_rights(void) {
  int failed = 0, p[2] = {-1, -1}, pair[2] = {-1, -1}, gate[2] = {-1, -1}, alias = -1,
      received = -1;
  pid_t child = -1;
  CHECK(!pipe(p) && !pipe(gate) && !socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  CHECK(!pt_fill(p[1], 16, 41) && (alias = dup(p[0])) >= 0);
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(8);
    close(p[0]);
    close(p[1]);
    close(alias);
    close(pair[0]);
    close(gate[1]);
    char byte;
    if (read(gate[0], &byte, 1) != 1)
      _exit(10);
    int fd = pt_receive_fd(pair[1]), q[2];
    if (fd < 0 || pipe(q) || splice(fd, NULL, q[1], NULL, 8, 0) != 8 || pt_read(q[0], 8, 0, 41) ||
        pt_send_fd(pair[1], fd))
      _exit(11);
    close(fd);
    _exit(0);
  }
  close(pair[1]);
  pair[1] = -1;
  close(gate[0]);
  gate[0] = -1;
  CHECK(!pt_send_fd(pair[0], alias));
  CHECK(!close(p[0]) && !close(alias));
  p[0] = alias = -1;
  CHECK(write(gate[1], "g", 1) == 1);
  CHECK((received = pt_receive_fd(pair[0])) >= 0);
  CHECK(!pt_read(received, 8, 8, 41));
  int status = pt_reap(child, 5000);
  child = -1;
  CHECK(!status);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    pt_reap(child, 1000);
  }
  if (received >= 0)
    close(received);
  if (alias >= 0)
    close(alias);
  for (int n = 0; n < 2; ++n) {
    if (p[n] >= 0)
      close(p[n]);
    if (pair[n] >= 0)
      close(pair[n]);
    if (gate[n] >= 0)
      close(gate[n]);
  }
  return failed;
}
static int numeric_reuse(void) {
  int failed = 0, p[2] = {-1, -1}, q[2] = {-1, -1}, pair[2] = {-1, -1}, replacement[2] = {-1, -1};
  int input_alias = -1, output_alias = -1, running = 0;
  struct pt_call call = {.kind = PT_SPLICE, .count = 32, .result = -2};
  pthread_t worker;
  CHECK(!pipe(p) && !pipe(q) && !socketpair(AF_UNIX, SOCK_STREAM, 0, pair) &&
        !socketpair(AF_UNIX, SOCK_STREAM, 0, replacement));
  CHECK(!pt_fill(p[1], 32, 59) && !pt_fill(q[1], 8, 83));
  ssize_t capacity = pt_socket_full(pair[0]);
  CHECK(capacity > 0 && (input_alias = dup(p[0])) >= 0 && (output_alias = dup(pair[0])) >= 0);
  call.input = p[0];
  call.output = pair[0];
  CHECK(!pthread_create(&worker, NULL, pt_worker, &call));
  running = 1;
  CHECK(!pt_wait(&call.ready, 5000));
  __atomic_store_n(&call.gate, 1, __ATOMIC_RELEASE);
  CHECK(!pt_wait_readiness(p[0], POLLIN, 0));
  CHECK(!__atomic_load_n(&call.done, __ATOMIC_ACQUIRE));
  CHECK(dup2(q[0], p[0]) == p[0] && dup2(replacement[0], pair[0]) == pair[0]);
  CHECK(!pt_read(pair[1], capacity, 0, PT_FILLER));
  CHECK(!pt_join(worker, &call));
  running = 0;
  CHECK(call.result > 0 && call.result <= 32 && call.error == 0);
  CHECK(!pt_read(pair[1], call.result, 0, 59));
  CHECK(!pt_read(input_alias, 32 - call.result, call.result, 59));
  CHECK(!pt_read(p[0], 8, 0, 83));
  CHECK(!fcntl(replacement[1], F_SETFL, O_NONBLOCK));
  char byte;
  CHECK(read(replacement[1], &byte, 1) == -1 && errno == EAGAIN);
out:
  if (failed)
    pt_diagnostic(&call);
  if (running) {
    shutdown(pair[1], SHUT_RD);
    pt_join(worker, &call);
  }
  if (input_alias >= 0)
    close(input_alias);
  if (output_alias >= 0)
    close(output_alias);
  for (int n = 0; n < 2; ++n) {
    if (p[n] >= 0)
      close(p[n]);
    if (q[n] >= 0)
      close(q[n]);
    if (pair[n] >= 0)
      close(pair[n]);
    if (replacement[n] >= 0)
      close(replacement[n]);
  }
  return failed;
}
static int terminal_exit(int reservation) {
  int failed = 0, p[2] = {-1, -1}, output[2] = {-1, -1};
  pid_t child = -1;
  ssize_t filler = 0;
  CHECK(!pipe(p));
  if (reservation) {
    CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, output) && !pt_fill(p[1], 32, 47));
    CHECK((filler = pt_socket_full(output[1])) > 0);
  } else
    CHECK(!pipe(output));
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(8);
    close(p[1]);
    close(output[0]);
    struct pt_call call = {.kind = reservation ? PT_SPLICE : PT_TEE,
                           .input = p[0],
                           .output = output[1],
                           .count = 32,
                           .result = -2};
    pthread_t worker;
    if (pthread_create(&worker, NULL, pt_worker, &call) || pt_wait(&call.ready, 3000))
      _exit(20);
    __atomic_store_n(&call.gate, 1, __ATOMIC_RELEASE);
    if (reservation) {
      if (pt_wait_readiness(p[0], POLLIN, 0) || __atomic_load_n(&call.done, __ATOMIC_ACQUIRE))
        _exit(21);
    } else if (pt_wait(&call.entered, 3000))
      _exit(22);
    /* Exit must retire any token or pair waiter admitted by the other thread. */
    _exit(0);
  }
  close(p[0]);
  p[0] = -1;
  close(output[1]);
  output[1] = -1;
  int status = pt_reap(child, 5000);
  child = -1;
  CHECK(!status);
  CHECK(write(p[1], "x", 1) == -1 && errno == EPIPE);
  CHECK(!pt_read(output[0], filler, 0, PT_FILLER));
  CHECK(!fcntl(output[0], F_SETFL, O_NONBLOCK));
  char byte;
  CHECK(read(output[0], &byte, 1) == 0);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    pt_reap(child, 1000);
  }
  for (int n = 0; n < 2; ++n) {
    if (p[n] >= 0)
      close(p[n]);
    if (output[n] >= 0)
      close(output[n]);
  }
  return failed;
}
int pipe_transfer_lifetime(void) {
  return queued_rights() || numeric_reuse() || terminal_exit(1) || terminal_exit(0);
}
