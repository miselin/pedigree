#define _GNU_SOURCE
#include <unistd.h>

#include "contract.h"

struct flag_change {
  struct tf_transfer* transfer;
  volatile int ready, done;
  int result;
};
static void* set_append(void* argument) {
  struct flag_change* change = argument;
  __atomic_store_n(&change->ready, 1, __ATOMIC_RELEASE);
  if (!tf_wait(&change->transfer->gate, 5000))
    change->result = fcntl(change->transfer->output, F_SETFL, O_APPEND);
  __atomic_store_n(&change->done, 1, __ATOMIC_RELEASE);
  return NULL;
}
static int explicit_output_flags(void) {
  int failed = 0, copy_started = 0, flags_started = 0;
  struct tf_file input = {.fd = -1}, output = {.fd = -1};
  off_t from = 3, to = 24;
  struct tf_transfer transfer = {.kind = TF_COPY_RANGE,
                                 .input_offset = &from,
                                 .output_offset = &to,
                                 .count = 32,
                                 .result = -2};
  struct flag_change change = {.transfer = &transfer, .result = -2};
  pthread_t copy, flags;
  CHECK(!tf_create(&input, "/tmp", 128, 19) && !tf_create(&output, "/tmp", 128, 83));
  CHECK(lseek(output.fd, 17, SEEK_SET) == 17);
  transfer.input = input.fd;
  transfer.output = output.fd;
  CHECK(!pthread_create(&copy, NULL, tf_worker, &transfer));
  copy_started = 1;
  CHECK(!pthread_create(&flags, NULL, set_append, &change));
  flags_started = 1;
  CHECK(!tf_wait(&transfer.ready, 5000) && !tf_wait(&change.ready, 5000));
  __atomic_store_n(&transfer.gate, 1, __ATOMIC_RELEASE);
  CHECK(!tf_wait(&transfer.done, 5000) && !tf_wait(&change.done, 5000));
  CHECK(change.result == 0 && (fcntl(output.fd, F_GETFL) & O_APPEND));
  if (transfer.result == 32) {
    CHECK(from == 35 && to == 56 && !tf_verify(output.fd, 24, 32, 3, 19));
  } else {
    CHECK(transfer.result == -1 && transfer.error == EBADF && from == 3 && to == 24);
    CHECK(!tf_verify(output.fd, 0, 128, 0, 83));
  }
  CHECK(lseek(input.fd, 0, SEEK_CUR) == 0 && lseek(output.fd, 0, SEEK_CUR) == 17);
  CHECK(copy_file_range(input.fd, &from, output.fd, &to, 1, 0) == -1 && errno == EBADF);
out:
  if (failed)
    tf_diagnostic(&transfer);
  __atomic_store_n(&transfer.gate, 1, __ATOMIC_RELEASE);
  if (copy_started)
    tf_join(copy, &transfer);
  if (flags_started) {
    if (tf_wait(&change.done, 5000))
      _exit(61);
    pthread_join(flags, NULL);
  }
  tf_destroy(&output);
  tf_destroy(&input);
  return failed;
}

static int reversed_pair(int kind) {
  int failed = 0, started = 0;
  const size_t length = TF_CHUNK + 17;
  struct tf_file left = {.fd = -1}, right = {.fd = -1};
  struct tf_transfer transfers[2] = {{.kind = kind, .result = -2}, {.kind = kind, .result = -2}};
  pthread_t workers[2];
  CHECK(!tf_create(&left, "/tmp", 3 * length, 23) && !tf_create(&right, "/tmp", 3 * length, 67));
  transfers[0].input = left.fd;
  transfers[0].output = right.fd;
  transfers[1].input = right.fd;
  transfers[1].output = left.fd;
  for (int n = 0; n < 2; ++n) {
    transfers[n].count = length;
    CHECK(!pthread_create(&workers[n], NULL, tf_worker, &transfers[n]));
    ++started;
  }
  CHECK(!tf_wait(&transfers[0].ready, 5000) && !tf_wait(&transfers[1].ready, 5000));
  __atomic_store_n(&transfers[0].gate, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&transfers[1].gate, 1, __ATOMIC_RELEASE);
  CHECK(!tf_wait(&transfers[0].done, 5000) && !tf_wait(&transfers[1].done, 5000));
  CHECK(transfers[0].result == (ssize_t)length && transfers[1].result == (ssize_t)length);
  CHECK(lseek(left.fd, 0, SEEK_CUR) == (off_t)(2 * length) &&
        lseek(right.fd, 0, SEEK_CUR) == (off_t)(2 * length));
  unsigned char first;
  CHECK(pread(left.fd, &first, 1, 0) == 1);
  const int first_seed = first == tf_pattern(0, 23) ? 23 : 67;
  CHECK(!tf_verify(left.fd, 0, length, 0, first_seed));
  CHECK(!tf_verify(right.fd, 0, length, 0, first_seed));
  const int second_seed = first_seed == 23 ? 67 : 23;
  CHECK(!tf_verify(left.fd, length, length, length, second_seed));
  CHECK(!tf_verify(right.fd, length, length, length, second_seed));
  CHECK(!tf_verify(left.fd, 2 * length, length, 2 * length, 23));
  CHECK(!tf_verify(right.fd, 2 * length, length, 2 * length, 67));
out:
  if (failed) {
    tf_diagnostic(&transfers[0]);
    tf_diagnostic(&transfers[1]);
  }
  for (int n = 0; n < started; ++n)
    __atomic_store_n(&transfers[n].gate, 1, __ATOMIC_RELEASE);
  for (int n = 0; n < started; ++n)
    tf_join(workers[n], &transfers[n]);
  tf_destroy(&right);
  tf_destroy(&left);
  return failed;
}
int transfer_races(void) {
  for (int n = 0; n < 4; ++n)
    if (reversed_pair(TF_SENDFILE) || reversed_pair(TF_COPY_RANGE) || explicit_output_flags())
      return 1;
  return 0;
}
