#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/uio.h>

static int additive_and_size(void) {
  int failed = 0, fd = -1;
  char original[64];
  memset(original, 'a', sizeof(original));
  CHECK((fd = mf_make(sizeof(original))) >= 0);
  CHECK(pwrite(fd, original, sizeof(original), 0) == sizeof(original));
  CHECK(!fcntl(fd, F_ADD_SEALS, 0) && fcntl(fd, F_GET_SEALS) == 0);
  CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | 0x20) == -1 && errno == EINVAL);
  CHECK(fcntl(fd, F_GET_SEALS) == 0 && !mf_size(fd, 64));
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_GROW));
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_GROW));
  CHECK(ftruncate(fd, 65) == -1 && errno == EPERM);
  CHECK(pwrite(fd, "z", 1, 64) == -1 && errno == EPERM);
  CHECK(!mf_size(fd, 64) && !mf_contents(fd, 0, original, 64));
  CHECK(!ftruncate(fd, 32));
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_SHRINK));
  CHECK(ftruncate(fd, 31) == -1 && errno == EPERM);
  CHECK(!mf_size(fd, 32) && !mf_contents(fd, 0, original, 32));
  CHECK(!ftruncate(fd, 32));
  CHECK(lseek(fd, 31, SEEK_SET) == 31);
  CHECK(write(fd, "zz", 2) == -1 && errno == EPERM);
  CHECK(lseek(fd, 0, SEEK_CUR) == 31 && !mf_contents(fd, 0, original, 32));
  CHECK(pwrite(fd, "b", 1, 0) == 1);
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL));
  const int all = F_SEAL_GROW | F_SEAL_SHRINK | F_SEAL_WRITE | F_SEAL_FUTURE_WRITE | F_SEAL_SEAL;
  CHECK(fcntl(fd, F_GET_SEALS) == all);
  CHECK(fcntl(fd, F_ADD_SEALS, 0) == -1 && errno == EPERM);
  CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EPERM);
  CHECK(fcntl(fd, F_GET_SEALS) == all);
out:
  if (fd >= 0)
    close(fd);
  return failed;
}
static int denied_writes(int seal) {
  int failed = 0, fd = -1;
  const size_t page = sysconf(_SC_PAGESIZE);
  char* private = MAP_FAILED;
  char original[64], zeros[32] = {0};
  memset(original, 'q', sizeof(original));
  struct iovec vector[] = {{"x", 1}, {"y", 1}};
  CHECK((fd = mf_make(page)) >= 0);
  CHECK(pwrite(fd, original, sizeof(original), 0) == sizeof(original));
  CHECK(!fcntl(fd, F_ADD_SEALS, seal));
  CHECK(lseek(fd, 7, SEEK_SET) == 7);
  CHECK(write(fd, "x", 1) == -1 && errno == EPERM);
  CHECK(lseek(fd, 0, SEEK_CUR) == 7);
  CHECK(pwrite(fd, "x", 1, 0) == -1 && errno == EPERM);
  CHECK(writev(fd, vector, 2) == -1 && errno == EPERM);
  CHECK(pwritev(fd, vector, 2, 0) == -1 && errno == EPERM);
  CHECK(lseek(fd, 0, SEEK_CUR) == 7 && !mf_contents(fd, 0, original, 64));
  CHECK(!fcntl(fd, F_SETFL, O_APPEND));
  CHECK(write(fd, "x", 1) == -1 && errno == EPERM);
  CHECK(lseek(fd, 0, SEEK_CUR) == 7 && !mf_size(fd, page));
  CHECK(write(fd, "", 0) == 0 && pwrite(fd, "", 0, page + 1) == 0);
  CHECK(!mf_size(fd, page) && fcntl(fd, F_GET_SEALS) == seal);
  CHECK(!fcntl(fd, F_SETFL, 0));
  CHECK(!ftruncate(fd, 32) && !ftruncate(fd, 64));
  CHECK(!mf_contents(fd, 0, original, 32) && !mf_contents(fd, 32, zeros, 32));
  CHECK(!ftruncate(fd, page));
  private = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  CHECK(private != MAP_FAILED && private[0] == 'q');
  private[0] = 'p';
  CHECK(private[0] == 'p' && !mf_contents(fd, 0, "q", 1));
out:
  if (private != MAP_FAILED)
    munmap(private, page);
  if (fd >= 0)
    close(fd);
  return failed;
}
int memfd_seals(void) {
  return additive_and_size() || denied_writes(F_SEAL_WRITE) || denied_writes(F_SEAL_FUTURE_WRITE);
}
