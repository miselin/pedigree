#define _GNU_SOURCE
#include <poll.h>
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/sendfile.h>
#include <sys/socket.h>

static volatile int pipe_seen;
static void caught(int number) {
  if (number == SIGPIPE)
    __atomic_store_n(&pipe_seen, 1, __ATOMIC_RELEASE);
}
static int nonblocking(void) {
  int failed = 0, pair[2] = {-1, -1};
  struct tf_file input = {.fd = -1};
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  ssize_t capacity = tf_saturate(pair[0]);
  CHECK(capacity > 0 && !tf_socket_read(pair[1], capacity, 0, TF_FILLER));
  CHECK(!tf_create(&input, "/tmp", capacity + TF_CHUNK, 53));
  CHECK(!fcntl(pair[0], F_SETFL, O_NONBLOCK));
  ssize_t result = sendfile(pair[0], input.fd, NULL, capacity + TF_CHUNK);
  CHECK(result > 0 && result < capacity + TF_CHUNK);
  CHECK(lseek(input.fd, 0, SEEK_CUR) == result);
  /* Fill any unused tail before demanding the zero-progress EAGAIN case. */
  ssize_t filler = tf_saturate(pair[0]);
  CHECK(filler >= 0);
  CHECK(sendfile(pair[0], input.fd, NULL, 1) == -1 && errno == EAGAIN);
  CHECK(lseek(input.fd, 0, SEEK_CUR) == result);
  CHECK(!tf_socket_read(pair[1], result, 0, 53));
  CHECK(!tf_socket_read(pair[1], filler, 0, TF_FILLER));
  off_t offset = 3;
  CHECK(sendfile(pair[0], input.fd, &offset, 7) == 7);
  CHECK(offset == 10 && lseek(input.fd, 0, SEEK_CUR) == result);
  CHECK(!tf_socket_read(pair[1], 7, 3, 53));
out:
  if (pair[0] >= 0)
    close(pair[0]);
  if (pair[1] >= 0)
    close(pair[1]);
  tf_destroy(&input);
  return failed;
}
static int interrupted(int after_progress) {
  int failed = 0, pair[2] = {-1, -1}, running = 0;
  struct tf_file input = {.fd = -1};
  struct tf_transfer transfer = {.kind = TF_SENDFILE, .result = -2};
  pthread_t worker;
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  ssize_t capacity = tf_saturate(pair[0]);
  CHECK(capacity > 0);
  if (after_progress)
    CHECK(!tf_socket_read(pair[1], capacity, 0, TF_FILLER));
  CHECK(!tf_create(&input, "/tmp", capacity + 2 * TF_CHUNK, 37));
  transfer.input = input.fd;
  transfer.output = pair[0];
  transfer.count = capacity + 2 * TF_CHUNK;
  CHECK(!pthread_create(&worker, NULL, tf_worker, &transfer));
  running = 1;
  CHECK(!tf_wait(&transfer.ready, 5000));
  __atomic_store_n(&transfer.gate, 1, __ATOMIC_RELEASE);
  if (after_progress) {
    struct pollfd entry = {.fd = pair[1], .events = POLLIN};
    /* This queue started empty; readability proves actual output acceptance. */
    CHECK(poll(&entry, 1, 5000) == 1 && (entry.revents & POLLIN));
    CHECK(!__atomic_load_n(&transfer.done, __ATOMIC_ACQUIRE));
  }
  const int64_t until = tf_now() + 3000000000;
  while (!__atomic_load_n(&transfer.done, __ATOMIC_ACQUIRE) && tf_now() < until) {
    CHECK(!pthread_kill(worker, SIGUSR1));
    tf_pause(2);
  }
  CHECK(!tf_wait(&transfer.done, 1000));
  CHECK(!tf_join(worker, &transfer));
  running = 0;
  if (after_progress) {
    CHECK(transfer.result > 0 && transfer.result < (ssize_t)transfer.count && transfer.error == 0);
    CHECK(lseek(input.fd, 0, SEEK_CUR) == transfer.result);
    CHECK(!tf_socket_read(pair[1], transfer.result, 0, 37));
  } else {
    CHECK(transfer.result == -1 && transfer.error == EINTR);
    CHECK(lseek(input.fd, 0, SEEK_CUR) == 0);
    CHECK(!tf_socket_read(pair[1], capacity, 0, TF_FILLER));
  }
out:
  if (failed)
    tf_diagnostic(&transfer);
  if (running) {
    shutdown(pair[1], SHUT_RD);
    tf_join(worker, &transfer);
  }
  if (pair[0] >= 0)
    close(pair[0]);
  if (pair[1] >= 0)
    close(pair[1]);
  tf_destroy(&input);
  return failed;
}
static int broken_peer(void) {
  int failed = 0, pair[2] = {-1, -1};
  struct tf_file input = {.fd = -1};
  CHECK(!tf_create(&input, "/tmp", 32, 5) && !socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  CHECK(!close(pair[1]));
  pair[1] = -1;
  __atomic_store_n(&pipe_seen, 0, __ATOMIC_RELEASE);
  CHECK(sendfile(pair[0], input.fd, NULL, 1) == -1 && errno == EPIPE);
  CHECK(!tf_wait(&pipe_seen, 1000));
  CHECK(lseek(input.fd, 0, SEEK_CUR) == 0);
out:
  if (pair[0] >= 0)
    close(pair[0]);
  if (pair[1] >= 0)
    close(pair[1]);
  tf_destroy(&input);
  return failed;
}
static int excluded_endpoints(void) {
  int failed = 0, pipefd[2] = {-1, -1}, datagram = -1, device = -1, directory = -1,
      unconnected = -1;
  struct tf_file input = {.fd = -1}, output = {.fd = -1};
  CHECK(!tf_create(&input, "/tmp", 32, 9) && !tf_create(&output, "/tmp", 32, 71));
  CHECK(!pipe(pipefd));
  CHECK((datagram = socket(AF_UNIX, SOCK_DGRAM, 0)) >= 0 &&
        (device = open("/dev/null", O_RDWR)) >= 0 && (directory = open("/tmp", O_RDONLY)) >= 0);
  CHECK((unconnected = socket(AF_UNIX, SOCK_STREAM, 0)) >= 0);
  for (int kind = 0; kind < 2; ++kind) {
    CHECK(tf_copy(kind, input.fd, NULL, pipefd[1], NULL, 1) == -1 && errno == EINVAL);
    CHECK(tf_copy(kind, pipefd[0], NULL, output.fd, NULL, 1) == -1 && errno == EINVAL);
    CHECK(tf_copy(kind, input.fd, NULL, datagram, NULL, 1) == -1 && errno == EINVAL);
    CHECK(tf_copy(kind, input.fd, NULL, device, NULL, 1) == -1 && errno == EINVAL);
    CHECK(tf_copy(kind, device, NULL, output.fd, NULL, 1) == -1 && errno == EINVAL);
    CHECK(tf_copy(kind, directory, NULL, output.fd, NULL, 1) == -1 && errno == EISDIR);
  }
  off_t offset = 0;
  CHECK(sendfile(output.fd, pipefd[0], &offset, 1) == -1 && errno == ESPIPE);
  CHECK(sendfile(unconnected, input.fd, NULL, 1) == -1 && errno == ENOTCONN);
  CHECK(sendfile(unconnected, input.fd, NULL, 0) == -1 && errno == ENOTCONN);
  CHECK(lseek(input.fd, 0, SEEK_CUR) == 0 && lseek(output.fd, 0, SEEK_CUR) == 0);
  CHECK(!tf_verify(output.fd, 0, 32, 0, 71));
out:
  if (unconnected >= 0)
    close(unconnected);
  if (directory >= 0)
    close(directory);
  if (device >= 0)
    close(device);
  if (datagram >= 0)
    close(datagram);
  if (pipefd[0] >= 0)
    close(pipefd[0]);
  if (pipefd[1] >= 0)
    close(pipefd[1]);
  tf_destroy(&output);
  tf_destroy(&input);
  return failed;
}
int transfer_stream(void) {
  struct sigaction action = {.sa_handler = caught}, previous_usr, previous_pipe;
  sigemptyset(&action.sa_mask);
  if (sigaction(SIGUSR1, &action, &previous_usr))
    return 1;
  if (sigaction(SIGPIPE, &action, &previous_pipe)) {
    sigaction(SIGUSR1, &previous_usr, NULL);
    return 1;
  }
  int failed =
      nonblocking() || interrupted(0) || interrupted(1) || broken_peer() || excluded_endpoints();
  sigaction(SIGPIPE, &previous_pipe, NULL);
  sigaction(SIGUSR1, &previous_usr, NULL);
  return failed;
}
