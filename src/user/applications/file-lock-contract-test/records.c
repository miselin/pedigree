#define _GNU_SOURCE
#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

static int ranges(void) {
  int failed = 0, a = -1, b = -1;
  char path[128] = {0};
  CHECK((a = fl_file(path, "/tmp")) >= 0 && (b = open(path, O_RDWR)) >= 0);
  CHECK(fl_record(a, F_OFD_SETLK, F_WRLCK, SEEK_SET, 100, 20) == 0);
  CHECK(fl_query(b, F_GETLK, 110, 1, F_WRLCK, 100, 20, -1) == 0);
  CHECK(fl_query(a, F_OFD_GETLK, 110, 1, F_UNLCK, 0, 0, 0) == 0);
  struct flock own = {.l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 110, .l_len = 1};
  CHECK(fcntl(a, F_OFD_GETLK, &own) == 0 && own.l_type == F_WRLCK && own.l_whence == SEEK_SET &&
        own.l_start == 100 && own.l_len == 20 && own.l_pid == -1);
  own = (struct flock){.l_type = F_UNLCK, .l_whence = SEEK_SET, .l_start = 110, .l_len = 1};
  CHECK(fcntl(b, F_OFD_GETLK, &own) == 0 && own.l_type == F_UNLCK);
  CHECK(fl_record(b, F_SETLK, F_WRLCK, SEEK_SET, 110, 1) == -1 && errno == EAGAIN);
  CHECK(fl_lock(a, FL_OFD, F_UNLCK, 0) == 0);
  CHECK(fl_record(a, F_SETLK, F_WRLCK, SEEK_SET, 20, 10) == 0);
  CHECK(fl_query(b, F_OFD_GETLK, 20, 1, F_WRLCK, 20, 10, getpid()) == 0);
  CHECK(fl_query(b, F_GETLK, 20, 1, F_UNLCK, 0, 0, 0) == 0);
  CHECK(fl_lock(a, FL_CLASSIC, F_UNLCK, 0) == 0);

  CHECK(lseek(a, 40, SEEK_SET) == 40);
  CHECK(fl_record(a, F_OFD_SETLK, F_WRLCK, SEEK_CUR, -10, -10) == 0);
  CHECK(fl_query(b, F_OFD_GETLK, 20, 1, F_WRLCK, 20, 10, -1) == 0);
  CHECK(lseek(a, 0, SEEK_CUR) == 40);
  CHECK(fl_lock(a, FL_OFD, F_UNLCK, 0) == 0);
  CHECK(fl_record(a, F_OFD_SETLK, F_WRLCK, SEEK_END, -8, 0) == 0);
  CHECK(ftruncate(a, 256) == 0);
  CHECK(fl_query(b, F_GETLK, 1000, 1, F_WRLCK, 120, 0, -1) == 0);
  CHECK(fl_lock(a, FL_OFD, F_UNLCK, 0) == 0);

  CHECK(fl_record(a, F_OFD_SETLK, F_WRLCK, SEEK_SET, 0, 100) == 0);
  CHECK(fl_record(a, F_OFD_SETLK, F_RDLCK, SEEK_SET, 20, 20) == 0);
  CHECK(fl_record(b, F_OFD_SETLK, F_RDLCK, SEEK_SET, 20, 20) == 0);
  CHECK(fl_record(a, F_OFD_SETLK, F_WRLCK, SEEK_SET, 20, 20) == -1 && errno == EAGAIN);
  CHECK(fl_lock(b, FL_OFD, F_UNLCK, 0) == 0);
  CHECK(fl_query(b, F_GETLK, 0, 1, F_WRLCK, 0, 20, -1) == 0);
  CHECK(fl_query(b, F_GETLK, 20, 1, F_RDLCK, 20, 20, -1) == 0);
  CHECK(fl_query(b, F_GETLK, 40, 1, F_WRLCK, 40, 60, -1) == 0);
  CHECK(fl_record(a, F_OFD_SETLK, F_UNLCK, SEEK_SET, 25, 10) == 0);
  CHECK(fl_record(b, F_OFD_SETLK, F_WRLCK, SEEK_SET, 25, 10) == 0);
  CHECK(fl_record(a, F_OFD_SETLK, F_WRLCK, SEEK_SET, 20, 20) == -1 && errno == EAGAIN);
  CHECK(fl_query(b, F_GETLK, 20, 1, F_RDLCK, 20, 5, -1) == 0);
  CHECK(fl_query(b, F_GETLK, 35, 1, F_RDLCK, 35, 5, -1) == 0);
  CHECK(fl_lock(b, FL_OFD, F_UNLCK, 0) == 0);
  CHECK(fl_record(a, F_OFD_SETLK, F_WRLCK, SEEK_SET, 20, 20) == 0);
  CHECK(fl_query(b, F_GETLK, 80, 1, F_WRLCK, 0, 100, -1) == 0);
out:
  if (b >= 0)
    close(b);
  if (a >= 0)
    close(a);
  if (*path)
    unlink(path);
  return failed;
}

static int errors(void) {
  int failed = 0, a = -1, b = -1, ro = -1, wo = -1;
  char path[128] = {0};
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  struct flock* wire = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(wire != MAP_FAILED);
  CHECK((a = fl_file(path, "/tmp")) >= 0 && (b = open(path, O_RDWR)) >= 0);
  CHECK((ro = open(path, O_RDONLY)) >= 0 && (wo = open(path, O_WRONLY)) >= 0);
  CHECK(fl_record(-1, F_SETLK, F_WRLCK, SEEK_SET, 0, 1) == -1 && errno == EBADF);
  CHECK(fl_record(a, F_SETLK, 99, SEEK_SET, 0, 1) == -1 && errno == EINVAL);
  CHECK(fl_record(a, F_SETLK, F_WRLCK, 99, 0, 1) == -1 && errno == EINVAL);
  CHECK(fl_record(a, F_SETLK, F_WRLCK, SEEK_SET, -1, 1) == -1 && errno == EINVAL);
  CHECK(fl_record(a, F_SETLK, F_WRLCK, SEEK_SET, 0, -1) == -1 && errno == EINVAL);
  CHECK(fl_record(a, F_SETLK, F_WRLCK, SEEK_SET, INT64_MAX, 2) == -1 && errno == EOVERFLOW);
  CHECK(lseek(a, 1, SEEK_SET) == 1);
  CHECK(fl_record(a, F_SETLK, F_WRLCK, SEEK_CUR, INT64_MAX, 1) == -1 && errno == EOVERFLOW);
  for (int n = 0; n < 2; ++n) {
    const int command = n ? F_OFD_SETLK : F_SETLK;
    CHECK(fl_record(ro, command, F_WRLCK, SEEK_SET, 0, 1) == -1 && errno == EBADF);
    CHECK(fl_record(wo, command, F_RDLCK, SEEK_SET, 0, 1) == -1 && errno == EBADF);
  }
  CHECK(fl_query(ro, F_OFD_GETLK, 0, 1, F_UNLCK, 0, 0, 0) == 0);
  *wire = (struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1, .l_pid = 9};
  CHECK(fcntl(a, F_OFD_SETLK, wire) == -1 && errno == EINVAL);
  CHECK(fcntl(a, F_OFD_GETLK, wire) == -1 && errno == EINVAL);
  CHECK(fcntl(a, F_OFD_SETLKW, wire) == -1 && errno == EINVAL);

  memset(wire, 0xa5, sizeof(*wire));
  wire->l_type = F_WRLCK;
  wire->l_whence = SEEK_SET;
  wire->l_start = 7;
  wire->l_len = 3;
  wire->l_pid = 0;
  struct flock expected;
  memcpy(&expected, wire, sizeof(expected));
  expected.l_type = F_UNLCK;
  CHECK(fcntl(a, F_OFD_GETLK, wire) == 0 && !memcmp(wire, &expected, sizeof(expected)));
  CHECK(fl_record(a, F_OFD_SETLK, F_WRLCK, SEEK_SET, 0, 10) == 0);
  *wire = (struct flock){.l_type = F_WRLCK, .l_whence = SEEK_SET, .l_len = 1};
  CHECK(mprotect(wire, page, PROT_READ) == 0);
  CHECK(fcntl(b, F_OFD_GETLK, wire) == -1 && errno == EFAULT);
  CHECK(fl_query(b, F_OFD_GETLK, 0, 1, F_WRLCK, 0, 10, -1) == 0);
  CHECK(mprotect(wire, page, PROT_NONE) == 0);
  CHECK(fcntl(b, F_SETLK, wire) == -1 && errno == EFAULT);
  CHECK(fcntl(b, F_GETLK, wire) == -1 && errno == EFAULT);
out:
  if (wire != MAP_FAILED)
    munmap(wire, page);
  if (wo >= 0)
    close(wo);
  if (ro >= 0)
    close(ro);
  if (b >= 0)
    close(b);
  if (a >= 0)
    close(a);
  if (*path)
    unlink(path);
  return failed;
}
int file_lock_records(void) {
  return ranges() || errors();
}
