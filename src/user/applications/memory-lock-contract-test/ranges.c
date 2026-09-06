#define _GNU_SOURCE
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/syscall.h>

int ml_ranges(void) {
  int failed = 0;
  unsigned char* pages = MAP_FAILED;
  unsigned char* hole = MAP_FAILED;
  void* denied = MAP_FAILED;
  pages = mmap(NULL, 4 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  hole = mmap(NULL, 2 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  denied = mmap(NULL, ml_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(pages != MAP_FAILED && hole != MAP_FAILED && denied != MAP_FAILED);
  CHECK(munmap(hole + ml_page, ml_page) == 0);
  CHECK(ml_limit(3 * ml_page + 137) == 0 && ml_unprivileged() == 0);
  CHECK(ml_resident(pages, 4, 0, "anonymous before eager lock") == 0);
  CHECK(mlock(pages + 1, ml_page) == 0);
  CHECK(ml_resident(pages, 4, 3, "unaligned eager lock") == 0);
  CHECK(mlock(pages, 2 * ml_page) == 0);
  CHECK(mlock(pages + 2 * ml_page, ml_page) == 0);
  errno = 0;
  CHECK(mlock(pages + 3 * ml_page, ml_page) == -1 && errno == ENOMEM);
  CHECK(munlock(pages + 1, ml_page) == 0);
  CHECK(madvise(pages, 2 * ml_page, MADV_DONTNEED) == 0);
  errno = 0;
  CHECK(madvise(pages + 2 * ml_page, ml_page, MADV_DONTNEED) == -1 && errno == EINVAL);
  CHECK(mlock(pages, 3 * ml_page) == 0);
  CHECK(munlock(pages, ml_page) == 0 && munlock(pages, ml_page) == 0);
  CHECK(mlock(pages + 3 * ml_page, ml_page) == 0);

  CHECK(munlockall() == 0 && madvise(pages, 4 * ml_page, MADV_DONTNEED) == 0);
  CHECK(mlock2(pages, 3 * ml_page, MLOCK_ONFAULT) == 0);
  CHECK(syscall(SYS_mlock2, pages, ml_page, MLOCK_ONFAULT) == 0);
  CHECK(ml_resident(pages, 4, 0, "onfault lock does not populate") == 0);
  errno = 0;
  CHECK(mlock2(pages + 3 * ml_page, ml_page, MLOCK_ONFAULT) == -1 && errno == ENOMEM);
  pages[ml_page] = 0x63;
  CHECK(ml_resident(pages, 4, 2, "one onfault page touched") == 0);
  CHECK(munlock(pages + ml_page, ml_page) == 0);
  CHECK(madvise(pages + ml_page, ml_page, MADV_DONTNEED) == 0);
  CHECK(mlock(pages + 3 * ml_page, ml_page) == 0);
  CHECK(munlockall() == 0);

  errno = 0;
  CHECK(mlock2(pages, ml_page, MLOCK_ONFAULT << 1) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(mlockall(0) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(mlockall(MCL_ONFAULT) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(mlock(hole, 2 * ml_page) == -1 && errno == ENOMEM);
  CHECK(munlockall() == 0);
  errno = 0;
  CHECK(mlock(denied, ml_page) == -1 && errno == ENOMEM);
  errno = 0;
  CHECK(madvise(denied, ml_page, MADV_DONTNEED) == -1 && errno == EINVAL);
  CHECK(munlock(denied, ml_page) == 0);
  CHECK(mprotect(denied, ml_page, PROT_READ | PROT_WRITE) == 0);
  CHECK(mlock(denied, ml_page) == 0);
out:
  munlockall();
  if (denied != MAP_FAILED)
    munmap(denied, ml_page);
  if (hole != MAP_FAILED)
    munmap(hole, 2 * ml_page);
  if (pages != MAP_FAILED)
    munmap(pages, 4 * ml_page);
  return failed;
}
