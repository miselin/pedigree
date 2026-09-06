#define _GNU_SOURCE
#include <poll.h>
#include <unistd.h>

#include "contract.h"

int pipe_transfer_tee(void) {
  int failed = 0, p[2] = {-1, -1}, q[2] = {-1, -1}, alias = -1, file = -1;
  CHECK(!pipe(p) && !pipe(q) && !pt_fill(p[1], PT_CAPACITY, 17));
  CHECK(tee(p[0], q[1], 13, SPLICE_F_MORE | SPLICE_F_MOVE | SPLICE_F_GIFT) == 13);
  CHECK(!pt_read(q[0], 13, 0, 17));
  CHECK(tee(p[0], q[1], 13, 0) == 13);
  CHECK(!pt_read(q[0], 13, 0, 17));
  CHECK(!pt_fill(q[1], PT_CAPACITY - 7, 73));
  ssize_t copied = tee(p[0], q[1], PT_CAPACITY, 0);
  CHECK(copied > 0 && copied <= 7);
  CHECK(!pt_read(q[0], PT_CAPACITY - 7, 0, 73));
  CHECK(!pt_read(q[0], copied, 0, 17));
  CHECK(!pt_fill(q[1], PT_CAPACITY, 91));
  CHECK(tee(p[0], q[1], 1, SPLICE_F_NONBLOCK) == -1 && errno == EAGAIN);
  CHECK(!fcntl(q[1], F_SETFL, O_NONBLOCK));
  CHECK(tee(p[0], q[1], 1, 0) == -1 && errno == EAGAIN);
  CHECK(!pt_read(p[0], PT_CAPACITY, 0, 17) && !pt_read(q[0], PT_CAPACITY, 0, 91));
  CHECK(tee(p[0], q[1], 1, SPLICE_F_NONBLOCK) == -1 && errno == EAGAIN);
  CHECK(!close(p[1]));
  p[1] = -1;
  CHECK(tee(p[0], q[1], 1, 0) == 0);
  CHECK((alias = dup(q[0])) >= 0);
  CHECK(tee(alias, q[1], 1, 0) == -1 && errno == EINVAL);
  CHECK(tee(q[1], q[0], 1, 0) == -1 && errno == EBADF);
  CHECK((file = pt_file(1, 0)) >= 0);
  CHECK(tee(file, q[1], 1, 0) == -1 && errno == EINVAL);
  CHECK(tee(q[0], p[0], 1, 0x80000000U) == -1 && errno == EINVAL);
  CHECK(tee(-1, -1, 0, 0) == 0);
  CHECK(tee(-1, -1, 0, 0x80000000U) == -1 && errno == EINVAL);
out:
  if (file >= 0)
    close(file);
  if (alias >= 0)
    close(alias);
  for (int n = 0; n < 2; ++n) {
    if (p[n] >= 0)
      close(p[n]);
    if (q[n] >= 0)
      close(q[n]);
  }
  return failed;
}
