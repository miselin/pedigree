#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/stat.h>

static volatile int pipe_signals;
static void caught(int number) {
  if (number == SIGPIPE)
    __atomic_add_fetch(&pipe_signals, 1, __ATOMIC_RELEASE);
}
static int cancel_reservation(void) {
  int failed = 0, p[2] = {-1, -1}, pair[2] = {-1, -1}, epoll = -1, running = 0;
  struct pt_call call = {.kind = PT_SPLICE, .count = 32, .result = -2};
  pthread_t worker;
  struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data.u64 = 17};
  CHECK(!pipe(p) && !socketpair(AF_UNIX, SOCK_STREAM, 0, pair) && !pt_fill(p[1], 32, 37));
  ssize_t capacity = pt_socket_full(pair[0]);
  CHECK(capacity > 0 && (epoll = epoll_create1(0)) >= 0);
  CHECK(!epoll_ctl(epoll, EPOLL_CTL_ADD, p[0], &event));
  CHECK(epoll_wait(epoll, &event, 1, 1000) == 1 && (event.events & EPOLLIN));
  call.input = p[0];
  call.output = pair[0];
  CHECK(!pthread_create(&worker, NULL, pt_worker, &call));
  running = 1;
  CHECK(!pt_wait(&call.ready, 5000));
  __atomic_store_n(&call.gate, 1, __ATOMIC_RELEASE);
  CHECK(!pt_wait_readiness(p[0], POLLIN, 0));
  CHECK(!__atomic_load_n(&call.done, __ATOMIC_ACQUIRE));
  CHECK(epoll_wait(epoll, &event, 1, 0) == 0);
  /* Appending behind a reserved head must not make that head readable. */
  CHECK(!pt_fill(p[1], 5, 59) && pt_ready(p[0], POLLIN, 0) == 0);
  CHECK(!fcntl(p[0], F_SETFL, O_NONBLOCK));
  char byte;
  CHECK(read(p[0], &byte, 1) == -1 && errno == EAGAIN);
  const int64_t until = pt_now() + 3000000000;
  while (!__atomic_load_n(&call.done, __ATOMIC_ACQUIRE) && pt_now() < until) {
    CHECK(!pthread_kill(worker, SIGUSR1));
    pt_pause(2);
  }
  CHECK(!pt_wait(&call.done, 1000) && call.result == -1 && call.error == EINTR);
  CHECK(!pt_join(worker, &call));
  running = 0;
  CHECK(pt_ready(p[0], POLLIN, 1000) == 1);
  CHECK(epoll_wait(epoll, &event, 1, 1000) == 1 && (event.events & EPOLLIN) &&
        event.data.u64 == 17);
  CHECK(epoll_wait(epoll, &event, 1, 0) == 0);
  CHECK(!pt_read(p[0], 32, 0, 37) && !pt_read(p[0], 5, 0, 59));
  CHECK(!pt_read(pair[1], capacity, 0, PT_FILLER));
  CHECK(!fcntl(pair[1], F_SETFL, O_NONBLOCK));
  CHECK(read(pair[1], &byte, 1) == -1 && errno == EAGAIN);
out:
  if (failed)
    pt_diagnostic(&call);
  if (running) {
    shutdown(pair[1], SHUT_RD);
    pt_join(worker, &call);
  }
  if (epoll >= 0)
    close(epoll);
  for (int n = 0; n < 2; ++n) {
    if (p[n] >= 0)
      close(p[n]);
    if (pair[n] >= 0)
      close(pair[n]);
  }
  return failed;
}
static int blocking_interrupt(int kind) {
  int failed = 0, p[2] = {-1, -1}, q[2] = {-1, -1}, running = 0;
  char byte = 'x';
  struct iovec vector = {&byte, 1};
  struct pt_call call = {
      .kind = kind, .count = 1, .vectors = &vector, .vector_count = 1, .result = -2};
  pthread_t worker;
  CHECK(!pipe(p) && !pipe(q));
  call.input = p[0];
  call.output = q[1];
  if (kind == PT_VMSPLICE) {
    CHECK(!pt_fill(p[1], PT_CAPACITY, 43));
    CHECK(!fcntl(p[1], F_SETFL, O_NONBLOCK));
    call.input = p[1];
  }
  CHECK(!pthread_create(&worker, NULL, pt_worker, &call));
  running = 1;
  CHECK(!pt_wait(&call.ready, 5000));
  __atomic_store_n(&call.gate, 1, __ATOMIC_RELEASE);
  const int64_t until = pt_now() + 3000000000;
  while (!__atomic_load_n(&call.done, __ATOMIC_ACQUIRE) && pt_now() < until) {
    CHECK(!pthread_kill(worker, SIGUSR1));
    pt_pause(2);
  }
  CHECK(!pt_wait(&call.done, 1000) && call.result == -1 && call.error == EINTR);
  CHECK(!pt_join(worker, &call));
  running = 0;
  if (kind == PT_VMSPLICE)
    CHECK(!pt_read(p[0], PT_CAPACITY, 0, 43));
out:
  if (failed)
    pt_diagnostic(&call);
  if (running) {
    close(q[0]);
    q[0] = -1;
    close(p[0]);
    p[0] = -1;
    pthread_kill(worker, SIGUSR1);
    pt_join(worker, &call);
  }
  for (int n = 0; n < 2; ++n) {
    if (p[n] >= 0)
      close(p[n]);
    if (q[n] >= 0)
      close(q[n]);
  }
  return failed;
}
static int broken_pipe(int kind) {
  int failed = 0, p[2] = {-1, -1}, q[2] = {-1, -1};
  char byte = 'x';
  struct iovec vector = {&byte, 1};
  CHECK(!pipe(p) && !pipe(q) && !pt_fill(p[1], 8, 23));
  CHECK(!close(q[0]));
  q[0] = -1;
  __atomic_store_n(&pipe_signals, 0, __ATOMIC_RELEASE);
  ssize_t result = kind == PT_TEE        ? tee(p[0], q[1], 1, 0)
                   : kind == PT_VMSPLICE ? vmsplice(q[1], &vector, 1, 0)
                                         : splice(p[0], NULL, q[1], NULL, 1, 0);
  CHECK(result == -1 && errno == EPIPE);
  CHECK(!pt_wait(&pipe_signals, 1000) && __atomic_load_n(&pipe_signals, __ATOMIC_ACQUIRE) == 1);
  CHECK(!pt_read(p[0], 8, 0, 23));
out:
  for (int n = 0; n < 2; ++n) {
    if (p[n] >= 0)
      close(p[n]);
    if (q[n] >= 0)
      close(q[n]);
  }
  return failed;
}
static int fifo_reopen(void) {
  int failed = 0, reader = -1, writer = -1, pair[2] = {-1, -1}, running = 0, created = 0;
  char path[128], byte;
  struct pt_call call = {.kind = PT_SPLICE, .count = 32, .result = -2};
  pthread_t worker;
  snprintf(path, sizeof(path), "/tmp/pipe-transfer-reopen-%ld", (long)getpid());
  CHECK(!mkfifo(path, 0600));
  created = 1;
  CHECK((reader = open(path, O_RDONLY | O_NONBLOCK)) >= 0);
  CHECK((writer = open(path, O_WRONLY | O_NONBLOCK)) >= 0);
  CHECK(!pt_fill(writer, 32, 61) && !socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  ssize_t capacity = pt_socket_full(pair[0]);
  CHECK(capacity > 0);
  call.input = reader;
  call.output = pair[0];
  CHECK(!pthread_create(&worker, NULL, pt_worker, &call));
  running = 1;
  CHECK(!pt_wait(&call.ready, 5000));
  __atomic_store_n(&call.gate, 1, __ATOMIC_RELEASE);
  CHECK(!pt_wait_readiness(reader, POLLIN, 0));
  CHECK(!__atomic_load_n(&call.done, __ATOMIC_ACQUIRE));
  CHECK(!close(writer));
  writer = -1;
  CHECK((writer = open(path, O_WRONLY | O_NONBLOCK)) >= 0);
  CHECK(!pt_fill(writer, 8, 79) && pt_ready(reader, POLLIN, 0) == 0);
  const int64_t until = pt_now() + 3000000000;
  while (!__atomic_load_n(&call.done, __ATOMIC_ACQUIRE) && pt_now() < until) {
    CHECK(!pthread_kill(worker, SIGUSR1));
    pt_pause(2);
  }
  CHECK(!pt_wait(&call.done, 1000) && call.result == -1 && call.error == EINTR);
  CHECK(!pt_join(worker, &call));
  running = 0;
  CHECK(!pt_read(reader, 32, 0, 61) && !pt_read(reader, 8, 0, 79));
  CHECK(!pt_read(pair[1], capacity, 0, PT_FILLER));
  CHECK(!pt_fill(writer, 16, 43));
  CHECK(!close(writer));
  writer = -1;
  CHECK(!close(reader));
  reader = -1;
  CHECK((reader = open(path, O_RDONLY | O_NONBLOCK)) >= 0);
  CHECK((writer = open(path, O_WRONLY | O_NONBLOCK)) >= 0);
  CHECK(read(reader, &byte, 1) == -1 && errno == EAGAIN);
  CHECK(!pt_fill(writer, 3, 19) && !pt_read(reader, 3, 0, 19));
out:
  if (failed)
    pt_diagnostic(&call);
  if (running) {
    shutdown(pair[1], SHUT_RD);
    pt_join(worker, &call);
  }
  if (writer >= 0)
    close(writer);
  if (reader >= 0)
    close(reader);
  for (int n = 0; n < 2; ++n)
    if (pair[n] >= 0)
      close(pair[n]);
  if (created)
    unlink(path);
  return failed;
}
int pipe_transfer_readiness(void) {
  struct sigaction action = {.sa_handler = caught}, old_usr, old_pipe;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR1, &action, &old_usr))
    return 1;
  if (sigaction(SIGPIPE, &action, &old_pipe)) {
    sigaction(SIGUSR1, &old_usr, NULL);
    return 1;
  }
  int failed = cancel_reservation() || fifo_reopen();
  for (int kind = 0; !failed && kind < 3; ++kind)
    failed = blocking_interrupt(kind) || broken_pipe(kind);
  sigaction(SIGPIPE, &old_pipe, NULL);
  sigaction(SIGUSR1, &old_usr, NULL);
  return failed;
}
