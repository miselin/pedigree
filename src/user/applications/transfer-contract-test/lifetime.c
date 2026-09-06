#define _GNU_SOURCE
#include <poll.h>
#include <unistd.h>

#include "contract.h"
#include <sys/socket.h>

static int collect(int socket, struct tf_transfer* transfer) {
  const int64_t until = tf_now() + 5000000000;
  size_t total = 0;
  unsigned char bytes[4096];
  while (tf_now() < until) {
    if (__atomic_load_n(&transfer->done, __ATOMIC_ACQUIRE)) {
      if (transfer->result <= 0 || total > (size_t)transfer->result)
        return -1;
      if (total == (size_t)transfer->result)
        return 0;
    }
    struct pollfd entry = {.fd = socket, .events = POLLIN};
    int ready = poll(&entry, 1, 100);
    if (ready < 0 && errno == EINTR)
      continue;
    if (ready < 0)
      return -1;
    if (!ready)
      continue;
    ssize_t count = read(socket, bytes, sizeof(bytes));
    if (count <= 0)
      return -1;
    for (ssize_t n = 0; n < count; ++n)
      if (bytes[n] != tf_pattern(total + n, 47))
        return -1;
    total += count;
    if (total > transfer->count)
      return -1;
  }
  return -1;
}
int transfer_lifetime(void) {
  int failed = 0, pair[2] = {-1, -1}, replacement[2] = {-1, -1};
  int input_alias = -1, output_alias = -1, running = 0;
  struct tf_file input = {.fd = -1}, other = {.fd = -1};
  struct tf_transfer transfer = {.kind = TF_SENDFILE, .result = -2};
  pthread_t worker;
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  ssize_t capacity = tf_saturate(pair[0]);
  CHECK(capacity > 0 && !tf_socket_read(pair[1], capacity, 0, TF_FILLER));
  CHECK(!tf_create(&input, "/tmp", capacity + 2 * TF_CHUNK, 47));
  CHECK(!tf_create(&other, "/tmp", 64, 89));
  CHECK(!socketpair(AF_UNIX, SOCK_STREAM, 0, replacement));
  CHECK((input_alias = dup(input.fd)) >= 0 && (output_alias = dup(pair[0])) >= 0);
  transfer.input = input.fd;
  transfer.output = pair[0];
  transfer.count = capacity + 2 * TF_CHUNK;
  CHECK(!pthread_create(&worker, NULL, tf_worker, &transfer));
  running = 1;
  CHECK(!tf_wait(&transfer.ready, 5000));
  __atomic_store_n(&transfer.gate, 1, __ATOMIC_RELEASE);
  struct pollfd readable = {.fd = pair[1], .events = POLLIN};
  /* Only this syscall can make the initially empty receive queue readable. */
  CHECK(poll(&readable, 1, 5000) == 1 && (readable.revents & POLLIN));
  CHECK(!__atomic_load_n(&transfer.done, __ATOMIC_ACQUIRE));
  CHECK(dup2(other.fd, input.fd) == input.fd);
  CHECK(dup2(replacement[0], pair[0]) == pair[0]);
  CHECK(!collect(pair[1], &transfer) && !tf_join(worker, &transfer));
  running = 0;
  CHECK(transfer.result > 0 && transfer.result <= (ssize_t)transfer.count && transfer.error == 0);
  CHECK(lseek(input_alias, 0, SEEK_CUR) == transfer.result);
  CHECK(lseek(input.fd, 0, SEEK_CUR) == 0 && !tf_verify(other.fd, 0, 64, 0, 89));
  CHECK(!fcntl(replacement[1], F_SETFL, O_NONBLOCK));
  char byte;
  CHECK(read(replacement[1], &byte, 1) == -1 && errno == EAGAIN);
out:
  if (failed)
    tf_diagnostic(&transfer);
  if (running) {
    shutdown(pair[1], SHUT_RD);
    tf_join(worker, &transfer);
  }
  if (output_alias >= 0)
    close(output_alias);
  if (input_alias >= 0)
    close(input_alias);
  for (int n = 0; n < 2; ++n) {
    if (pair[n] >= 0)
      close(pair[n]);
    if (replacement[n] >= 0)
      close(replacement[n]);
  }
  tf_destroy(&other);
  tf_destroy(&input);
  return failed;
}
