#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"

static int atomic_capacity(void) {
  int failed = 0, p[2] = {-1, -1};
  CHECK(!pipe(p) && !fcntl(p[1], F_SETFL, O_NONBLOCK));
  CHECK(!pt_fill(p[1], PT_CAPACITY - 1, 17));
  CHECK(write(p[1], "xx", 2) == -1 && errno == EAGAIN);
  CHECK(!pt_read(p[0], PT_CAPACITY - 1, 0, 17));
  CHECK(!pt_fill(p[1], PT_CAPACITY, 43));
  CHECK(write(p[1], "x", 1) == -1 && errno == EAGAIN);
  CHECK(!pt_read(p[0], PT_CAPACITY, 0, 43));
out:
  if (p[0] >= 0)
    close(p[0]);
  if (p[1] >= 0)
    close(p[1]);
  return failed;
}
static int count_bytes(int fd, size_t maximum, size_t totals[2]) {
  unsigned char bytes[PT_CAPACITY];
  ssize_t count = read(fd, bytes, maximum);
  if (count <= 0)
    return -1;
  for (ssize_t n = 0; n < count; ++n) {
    if (bytes[n] != 'a' && bytes[n] != 'b') {
      errno = EILSEQ;
      return -1;
    }
    ++totals[bytes[n] - 'a'];
  }
  return count;
}
static int opposite_pair(int kind) {
  int failed = 0, p[2] = {-1, -1}, q[2] = {-1, -1}, started = 0;
  struct pt_call calls[2] = {{.kind = kind, .count = PT_CAPACITY, .result = -2},
                             {.kind = kind, .count = PT_CAPACITY, .result = -2}};
  pthread_t workers[2];
  char bytes[PT_CAPACITY];
  size_t totals[2] = {0};
  CHECK(!pipe(p) && !pipe(q));
  memset(bytes, 'a', sizeof(bytes));
  CHECK(write(p[1], bytes, sizeof(bytes)) == sizeof(bytes));
  memset(bytes, 'b', sizeof(bytes));
  CHECK(write(q[1], bytes, sizeof(bytes)) == sizeof(bytes));
  calls[0].input = p[0];
  calls[0].output = q[1];
  calls[1].input = q[0];
  calls[1].output = p[1];
  for (int n = 0; n < 2; ++n) {
    CHECK(!pthread_create(&workers[n], NULL, pt_worker, &calls[n]));
    ++started;
  }
  CHECK(!pt_wait(&calls[0].ready, 5000) && !pt_wait(&calls[1].ready, 5000));
  __atomic_store_n(&calls[0].gate, 1, __ATOMIC_RELEASE);
  __atomic_store_n(&calls[1].gate, 1, __ATOMIC_RELEASE);
  /* Leave source data available while creating room on both initially full destinations. */
  CHECK(count_bytes(p[0], 128, totals) == 128 && count_bytes(q[0], 128, totals) == 128);
  CHECK(!pt_wait(&calls[0].done, 5000) && !pt_wait(&calls[1].done, 5000));
  CHECK(calls[0].result > 0 && calls[0].result <= PT_CAPACITY && calls[1].result > 0 &&
        calls[1].result <= PT_CAPACITY);
  CHECK(!fcntl(p[0], F_SETFL, O_NONBLOCK) && !fcntl(q[0], F_SETFL, O_NONBLOCK));
  const int readers[] = {p[0], q[0]};
  for (int n = 0; n < 2; ++n) {
    while (count_bytes(readers[n], PT_CAPACITY, totals) > 0) {
    }
    CHECK(errno == EAGAIN);
  }
  CHECK(totals[0] == PT_CAPACITY + (kind == PT_TEE ? (size_t)calls[0].result : 0));
  CHECK(totals[1] == PT_CAPACITY + (kind == PT_TEE ? (size_t)calls[1].result : 0));
out:
  if (failed) {
    pt_diagnostic(&calls[0]);
    pt_diagnostic(&calls[1]);
  }
  for (int n = 0; n < started; ++n)
    __atomic_store_n(&calls[n].gate, 1, __ATOMIC_RELEASE);
  for (int n = 0; n < started; ++n)
    pt_join(workers[n], &calls[n]);
  for (int n = 0; n < 2; ++n) {
    if (p[n] >= 0)
      close(p[n]);
    if (q[n] >= 0)
      close(q[n]);
  }
  return failed;
}
int pipe_transfer_concurrency(void) {
  if (atomic_capacity())
    return 1;
  for (int n = 0; n < 4; ++n)
    if (opposite_pair(PT_SPLICE) || opposite_pair(PT_TEE))
      return 1;
  return 0;
}
