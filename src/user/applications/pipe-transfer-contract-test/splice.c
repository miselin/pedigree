#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/socket.h>

static int regular_directions(void) {
  int failed = 0, input = -1, output = -1, p[2] = {-1, -1};
  CHECK((input = pt_file(2 * PT_CAPACITY + 17, 11)) >= 0 && (output = pt_file(0, 0)) >= 0);
  CHECK(!pipe(p));
  size_t copied = 0;
  while (copied < 2 * PT_CAPACITY + 17) {
    ssize_t amount =
        splice(input, NULL, p[1], NULL, 8192, SPLICE_F_MORE | SPLICE_F_MOVE | SPLICE_F_GIFT);
    CHECK(amount > 0 && amount <= PT_CAPACITY);
    CHECK(splice(p[0], NULL, output, NULL, amount, 0) == amount);
    CHECK(!pt_verify(output, copied, amount, copied, 11));
    copied += amount;
  }
  CHECK(lseek(input, 0, SEEK_CUR) == (off_t)copied && lseek(output, 0, SEEK_CUR) == (off_t)copied);
  CHECK(splice(input, NULL, p[1], NULL, 1, 0) == 0);
  off_t from = 3, to = 7;
  CHECK(splice(input, &from, p[1], NULL, 9, 0) == 9 && from == 12);
  CHECK(splice(p[0], NULL, output, &to, 9, 0) == 9 && to == 16);
  CHECK(!pt_verify(output, 7, 9, 3, 11));
  CHECK(lseek(input, 0, SEEK_CUR) == (off_t)copied && lseek(output, 0, SEEK_CUR) == (off_t)copied);
  CHECK(!fcntl(p[0], F_SETFL, O_NONBLOCK));
  CHECK(splice(p[0], NULL, output, NULL, 1, 0) == -1 && errno == EAGAIN);
  CHECK(!close(p[1]));
  p[1] = -1;
  CHECK(splice(p[0], NULL, output, NULL, 1, 0) == 0);
out:
  if (p[0] >= 0)
    close(p[0]);
  if (p[1] >= 0)
    close(p[1]);
  if (output >= 0)
    close(output);
  if (input >= 0)
    close(input);
  return failed;
}
static int validation_and_offsets(void) {
  int failed = 0, file = -1, device = -1, datagram = -1, p[2] = {-1, -1}, q[2] = {-1, -1};
  const size_t page = sysconf(_SC_PAGESIZE);
  off_t* offset = MAP_FAILED;
  CHECK((file = pt_file(64, 29)) >= 0 && !pipe(p) && !pipe(q));
  offset = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(offset != MAP_FAILED);
  *offset = 3;
  CHECK(!mprotect(offset, page, PROT_READ));
  CHECK(splice(file, offset, p[1], NULL, 4, 0) == -1 && errno == EFAULT);
  CHECK(*offset == 3 && lseek(file, 0, SEEK_CUR) == 0 && !pt_read(p[0], 4, 3, 29));
  CHECK(!pt_fill(p[1], 4, 53));
  CHECK(splice(p[0], NULL, file, offset, 4, 0) == -1 && errno == EFAULT);
  CHECK(*offset == 3 && lseek(file, 0, SEEK_CUR) == 0 && !pt_verify(file, 3, 4, 0, 53));
  CHECK(splice(p[0], NULL, file, offset, 1, SPLICE_F_NONBLOCK) == -1 && errno == EAGAIN);
  CHECK(!mprotect(offset, page, PROT_READ | PROT_WRITE));
  *offset = 64;
  CHECK(!mprotect(offset, page, PROT_READ));
  CHECK(splice(file, offset, p[1], NULL, 1, 0) == -1 && errno == EFAULT);
  CHECK(!mprotect(offset, page, PROT_NONE));
  CHECK(splice(file, offset, p[1], NULL, 1, 0) == -1 && errno == EFAULT);
  CHECK(splice(p[0], offset, q[1], NULL, 1, 0) == -1 && errno == ESPIPE);
  CHECK(splice(file, NULL, p[1], offset, 1, 0) == -1 && errno == ESPIPE);
  CHECK(splice(p[0], NULL, p[1], NULL, 1, 0) == -1 && errno == EINVAL);
  CHECK(splice(p[1], NULL, q[1], NULL, 1, 0) == -1 && errno == EBADF);
  CHECK(splice(file, NULL, file, NULL, 1, 0) == -1 && errno == EINVAL);
  CHECK(splice(-1, NULL, -1, NULL, 0, 0x80000000U) == 0);
  CHECK(splice(file, NULL, p[1], NULL, 1, 0x80000000U) == -1 && errno == EINVAL);
  CHECK(!pt_fill(p[1], 8, 71) && !fcntl(file, F_SETFL, O_APPEND));
  CHECK(splice(p[0], NULL, file, NULL, 8, 0) == -1 && errno == EINVAL);
  CHECK(!pt_read(p[0], 8, 0, 71));
  CHECK((device = open("/dev/null", O_RDWR)) >= 0 &&
        (datagram = socket(AF_UNIX, SOCK_DGRAM, 0)) >= 0);
  CHECK(splice(device, NULL, p[1], NULL, 1, 0) == -1 && errno == EINVAL);
  CHECK(splice(p[0], NULL, datagram, NULL, 1, 0) == -1 && errno == EINVAL);
out:
  if (datagram >= 0)
    close(datagram);
  if (device >= 0)
    close(device);
  if (offset != MAP_FAILED)
    munmap(offset, page);
  for (int n = 0; n < 2; ++n) {
    if (p[n] >= 0)
      close(p[n]);
    if (q[n] >= 0)
      close(q[n]);
  }
  if (file >= 0)
    close(file);
  return failed;
}
static int sealed_output(int seal) {
  int failed = 0, file = -1, p[2] = {-1, -1};
  CHECK((file = pt_file(8, 13)) >= 0 && !pipe(p) && !pt_fill(p[1], 16, 31));
  CHECK(!fcntl(file, F_ADD_SEALS, seal));
  CHECK(splice(p[0], NULL, file, NULL, 16, 0) == -1 && errno == EPERM);
  CHECK(lseek(file, 0, SEEK_CUR) == 0 && !pt_verify(file, 0, 8, 0, 13));
  CHECK(!pt_read(p[0], 16, 0, 31));
out:
  if (p[0] >= 0)
    close(p[0]);
  if (p[1] >= 0)
    close(p[1]);
  if (file >= 0)
    close(file);
  return failed;
}
static int socket_short(void) {
  int failed = 0, p[2] = {-1, -1}, pair[2] = {-1, -1};
  CHECK(!pipe(p) && !socketpair(AF_UNIX, SOCK_STREAM, 0, pair));
  CHECK(!pt_fill(p[1], PT_CAPACITY, 41));
  ssize_t capacity = pt_socket_full(pair[0]);
  CHECK(capacity > 8 && !pt_read(pair[1], 7, 0, PT_FILLER));
  CHECK(!fcntl(pair[0], F_SETFL, O_NONBLOCK));
  ssize_t result = splice(p[0], NULL, pair[0], NULL, PT_CAPACITY, 0);
  CHECK(result > 0 && result <= 7);
  CHECK(!pt_read(pair[1], capacity - 7, 0, PT_FILLER));
  CHECK(!pt_read(pair[1], result, 0, 41));
  CHECK(!pt_read(p[0], PT_CAPACITY - result, result, 41));
out:
  for (int n = 0; n < 2; ++n) {
    if (p[n] >= 0)
      close(p[n]);
    if (pair[n] >= 0)
      close(pair[n]);
  }
  return failed;
}
int pipe_transfer_splice(void) {
  return regular_directions() || validation_and_offsets() || sealed_output(F_SEAL_WRITE) ||
         sealed_output(F_SEAL_FUTURE_WRITE) || sealed_output(F_SEAL_GROW) || socket_short();
}
