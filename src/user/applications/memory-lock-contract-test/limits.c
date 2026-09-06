#define _GNU_SOURCE
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>

int ml_limits(void) {
  int failed = 0;
  struct rlimit original, current, request;
  void* inaccessible = MAP_FAILED;
  void* readonly = MAP_FAILED;
  unsigned char* pages = MAP_FAILED;
  CHECK(getrlimit(RLIMIT_MEMLOCK, &original) == 0);
  CHECK(original.rlim_cur == 16 * 1024 * 1024 && original.rlim_max == original.rlim_cur);
  inaccessible = mmap(NULL, ml_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  readonly = mmap(NULL, ml_page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  pages = mmap(NULL, 2 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(inaccessible != MAP_FAILED && readonly != MAP_FAILED && pages != MAP_FAILED);

  request = original;
  request.rlim_cur = 4 * ml_page;
  CHECK(setrlimit(RLIMIT_MEMLOCK, &request) == 0);
  CHECK(prlimit(getpid(), RLIMIT_MEMLOCK, NULL, &current) == 0);
  CHECK(current.rlim_cur == request.rlim_cur && current.rlim_max == request.rlim_max);
  request.rlim_cur = 3 * ml_page;
  CHECK(syscall(SYS_setrlimit, RLIMIT_MEMLOCK, &request) == 0);
  request.rlim_cur = 2 * ml_page;
  CHECK(prlimit(0, RLIMIT_MEMLOCK, &request, &request) == 0);
  CHECK(request.rlim_cur == 3 * ml_page);
  CHECK(getrlimit(RLIMIT_MEMLOCK, &current) == 0 && current.rlim_cur == 2 * ml_page);

  errno = 0;
  CHECK(setrlimit(RLIMIT_MEMLOCK, inaccessible) == -1 && errno == EFAULT);
  errno = 0;
  CHECK(syscall(SYS_setrlimit, RLIMIT_MEMLOCK, inaccessible) == -1 && errno == EFAULT);
  CHECK(getrlimit(RLIMIT_MEMLOCK, &current) == 0 && current.rlim_cur == 2 * ml_page);
  request = current;
  request.rlim_cur = ml_page;
  errno = 0;
  CHECK(prlimit(0, RLIMIT_MEMLOCK, &request, readonly) == -1 && errno == EFAULT);
  CHECK(getrlimit(RLIMIT_MEMLOCK, &current) == 0 && current.rlim_cur == ml_page);
  errno = 0;
  CHECK(prlimit(0, RLIMIT_MEMLOCK, NULL, readonly) == -1 && errno == EFAULT);
  request.rlim_cur = 3 * ml_page;
  request.rlim_max = 2 * ml_page;
  errno = 0;
  CHECK(setrlimit(RLIMIT_MEMLOCK, &request) == -1 && errno == EINVAL);
  CHECK(getrlimit(RLIMIT_MEMLOCK, &current) == 0 && current.rlim_cur == ml_page);

  request.rlim_cur = request.rlim_max = 2 * ml_page;
  CHECK(setrlimit(RLIMIT_MEMLOCK, &request) == 0 && ml_unprivileged() == 0);
  request.rlim_max = 3 * ml_page;
  errno = 0;
  CHECK(setrlimit(RLIMIT_MEMLOCK, &request) == -1 && errno == EPERM);
  CHECK(mlock(pages, ml_page) == 0);
  CHECK(ml_limit(0) == 0);
  errno = 0;
  CHECK(madvise(pages, ml_page, MADV_DONTNEED) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(mlock(pages + ml_page, ml_page) == -1 && errno == EPERM);
  CHECK(munlockall() == 0 && madvise(pages, ml_page, MADV_DONTNEED) == 0);
  CHECK(ml_limit(ml_page) == 0 && mlock(pages + ml_page, ml_page) == 0);
out:
  munlockall();
  if (pages != MAP_FAILED)
    munmap(pages, 2 * ml_page);
  if (readonly != MAP_FAILED)
    munmap(readonly, ml_page);
  if (inaccessible != MAP_FAILED)
    munmap(inaccessible, ml_page);
  return failed;
}
