#define _GNU_SOURCE
#include <unistd.h>

#include "contract.h"
#include <sys/file.h>

int file_lock_flock(void) {
  int failed = 0, a = -1, b = -1, readonly = -1;
  char path[128] = {0}, byte = 'a';
  CHECK((a = fl_file(path, "/tmp")) >= 0);
  CHECK((b = open(path, O_RDWR)) >= 0 && (readonly = open(path, O_RDONLY)) >= 0);
  CHECK(flock(-1, LOCK_EX) == -1 && errno == EBADF);
  CHECK(flock(a, 0) == -1 && errno == EINVAL);
  CHECK(flock(a, LOCK_SH | LOCK_EX) == -1 && errno == EINVAL);
  CHECK(flock(a, LOCK_EX | 0x1000) == -1 && errno == EINVAL);
  CHECK(flock(a, LOCK_EX | LOCK_NB) == 0);
  CHECK(flock(b, LOCK_EX | LOCK_NB) == -1 && errno == EAGAIN);
  CHECK(pwrite(b, &byte, 1, 0) == 1 && pread(a, &byte, 1, 0) == 1 && byte == 'a');
  CHECK(fl_lock(b, FL_OFD, F_WRLCK, 0) == 0);
  CHECK(fl_lock(b, FL_OFD, F_UNLCK, 0) == 0);
  CHECK(flock(a, LOCK_SH) == 0 && flock(b, LOCK_SH) == 0);
  CHECK(flock(a, LOCK_EX | LOCK_NB) == -1 && errno == EAGAIN);
  // A failed flock upgrade has released a's old shared grant.
  CHECK(flock(b, LOCK_EX | LOCK_NB) == 0);
  CHECK(flock(a, LOCK_SH | LOCK_NB) == -1 && errno == EAGAIN);
  CHECK(flock(b, LOCK_UN) == 0 && flock(b, LOCK_UN) == 0);
  CHECK(flock(readonly, LOCK_EX | LOCK_NB) == 0);
  CHECK(flock(a, LOCK_SH | LOCK_NB) == -1 && errno == EAGAIN);
  CHECK(flock(readonly, LOCK_UN) == 0);
out:
  if (readonly >= 0)
    close(readonly);
  if (b >= 0)
    close(b);
  if (a >= 0)
    close(a);
  if (*path)
    unlink(path);
  return failed;
}
