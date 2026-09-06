#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

static int future_mappings(void) {
  int failed = 0;
  unsigned char* existing = MAP_FAILED;
  unsigned char* victim = MAP_FAILED;
  void* future = MAP_FAILED;
  void* populated = MAP_FAILED;
  void* locked = MAP_FAILED;
  existing = mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  victim = mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(existing != MAP_FAILED && victim != MAP_FAILED);
  victim[0] = 0x6d;
  CHECK(ml_limit(2 * ml_page) == 0);
  CHECK(mlock(existing, ml_page) == 0);
  CHECK(mlockall(MCL_FUTURE | MCL_ONFAULT) == 0);
  CHECK(ml_unprivileged() == 0);
  future = mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(future != MAP_FAILED && ml_resident(future, 1, 0, "future onfault mapping") == 0);
  errno = 0;
  CHECK(madvise(existing, ml_page, MADV_DONTNEED) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(mmap(NULL, ml_page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0) == MAP_FAILED &&
        errno == EAGAIN);
  errno = 0;
  CHECK(mmap(victim, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
             0) == MAP_FAILED &&
        errno == EAGAIN);
  CHECK(victim[0] == 0x6d);
  CHECK(munmap(future, ml_page) == 0);
  future = MAP_FAILED;
  future = mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(future != MAP_FAILED);
  CHECK(munlockall() == 0 && madvise(existing, ml_page, MADV_DONTNEED) == 0);
  CHECK(madvise(future, ml_page, MADV_DONTNEED) == 0);

  locked =
      mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0);
  CHECK(locked != MAP_FAILED && ml_resident(locked, 1, 1, "MAP_LOCKED eager mapping") == 0);
  errno = 0;
  CHECK(mmap(NULL, 2 * ml_page, PROT_READ, MAP_PRIVATE | MAP_ANONYMOUS | MAP_LOCKED, -1, 0) ==
            MAP_FAILED &&
        errno == EAGAIN);
  populated = mmap(NULL, 2 * ml_page, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_POPULATE, -1, 0);
  CHECK(populated != MAP_FAILED && ml_resident(populated, 2, 3, "MAP_POPULATE") == 0);
  CHECK(mlock(populated, ml_page) == 0);
  errno = 0;
  CHECK(mlock((unsigned char*)populated + ml_page, ml_page) == -1 && errno == ENOMEM);
out:
  munlockall();
  if (locked != MAP_FAILED)
    munmap(locked, ml_page);
  if (populated != MAP_FAILED)
    munmap(populated, 2 * ml_page);
  if (future != MAP_FAILED)
    munmap(future, ml_page);
  if (victim != MAP_FAILED)
    munmap(victim, ml_page);
  if (existing != MAP_FAILED)
    munmap(existing, ml_page);
  return failed;
}

static int current_mappings(void) {
  int failed = 0;
  void* lazy = MAP_FAILED;
  void* later = MAP_FAILED;
  CHECK(ml_limit(16 * 1024 * 1024) == 0);
  lazy = mmap(NULL, 3 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(lazy != MAP_FAILED && ml_resident(lazy, 3, 0, "before CURRENT") == 0);
  CHECK(mlockall(MCL_CURRENT | MCL_FUTURE | MCL_ONFAULT) == 0);
  CHECK(ml_resident(lazy, 3, 0, "CURRENT ONFAULT") == 0);
  errno = 0;
  CHECK(madvise(lazy, 3 * ml_page, MADV_DONTNEED) == -1 && errno == EINVAL);
  CHECK(mlockall(MCL_CURRENT) == 0);
  CHECK(ml_resident(lazy, 3, 7, "CURRENT eager population") == 0);
  CHECK(ml_limit(0) == 0);
  later = mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(later != MAP_FAILED && madvise(later, ml_page, MADV_DONTNEED) == 0);
  errno = 0;
  CHECK(madvise(lazy, ml_page, MADV_DONTNEED) == -1 && errno == EINVAL);
  CHECK(munlockall() == 0 && madvise(lazy, 3 * ml_page, MADV_DONTNEED) == 0);
out:
  munlockall();
  if (later != MAP_FAILED)
    munmap(later, ml_page);
  if (lazy != MAP_FAILED)
    munmap(lazy, 3 * ml_page);
  return failed;
}

static int remap_charge(void) {
  int failed = 0;
  unsigned char* source = MAP_FAILED;
  unsigned char* target = MAP_FAILED;
  void* extra = MAP_FAILED;
  size_t source_length = 2 * ml_page;
  int moved = 0;
  CHECK(ml_limit(2 * ml_page) == 0);
  source = mmap(NULL, source_length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  target = mmap(NULL, 3 * ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  extra = mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(source != MAP_FAILED && target != MAP_FAILED && extra != MAP_FAILED);
  source[0] = 0x39;
  source[ml_page] = 0x62;
  target[0] = target[2 * ml_page] = 0x74;
  CHECK(mlock(source, source_length) == 0);
  errno = 0;
  CHECK(mremap(source, source_length, 3 * ml_page, MREMAP_MAYMOVE | MREMAP_FIXED, target) ==
            MAP_FAILED &&
        errno == EAGAIN);
  CHECK(source[0] == 0x39 && source[ml_page] == 0x62 && target[0] == 0x74);
  CHECK(mremap(source, source_length, source_length, MREMAP_MAYMOVE | MREMAP_FIXED, target) ==
        target);
  source = target;
  moved = 1;
  CHECK(source[0] == 0x39 && source[ml_page] == 0x62 && target[2 * ml_page] == 0x74);
  CHECK(ml_resident(source, 2, 3, "locked remap retains residency") == 0);
  errno = 0;
  CHECK(mlock(extra, ml_page) == -1 && errno == ENOMEM);
  CHECK(mremap(source, source_length, ml_page, 0) == source);
  source_length = ml_page;
  CHECK(mlock(extra, ml_page) == 0);
  errno = 0;
  CHECK(madvise(source, ml_page, MADV_DONTNEED) == -1 && errno == EINVAL);
out:
  munlockall();
  if (extra != MAP_FAILED)
    munmap(extra, ml_page);
  if (source != MAP_FAILED && !moved)
    munmap(source, source_length);
  if (target != MAP_FAILED)
    munmap(target, 3 * ml_page);
  return failed;
}

static int file_charge(void) {
  int failed = 0;
  int fd = -1;
  unsigned char* shared = MAP_FAILED;
  unsigned char* private = MAP_FAILED;
  void* alias = MAP_FAILED;
  CHECK(ml_limit(4 * ml_page) == 0);
  fd = memfd_create("locked-file", MFD_CLOEXEC);
  CHECK(fd >= 0 && ftruncate(fd, 3 * ml_page) == 0);
  for (size_t n = 0; n < 3; ++n) {
    unsigned char value = 0x41 + n;
    CHECK(pwrite(fd, &value, 1, n * ml_page) == 1);
  }
  shared = mmap(NULL, 3 * ml_page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  private = mmap(NULL, ml_page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  alias = mmap(NULL, ml_page, PROT_READ, MAP_SHARED, fd, ml_page);
  CHECK(shared != MAP_FAILED && private != MAP_FAILED && alias != MAP_FAILED);
  CHECK(mlock(shared, 3 * ml_page) == 0 && mlock(private, ml_page) == 0);
  private[0] = 0x77;
  CHECK(shared[0] == 0x41);
  errno = 0;
  CHECK(mlock(alias, ml_page) == -1 && errno == ENOMEM);
  errno = 0;
  CHECK(msync(shared, ml_page, MS_SYNC | MS_INVALIDATE) == -1 && errno == EBUSY);
  CHECK(ftruncate(fd, ml_page + 137) == 0);
  CHECK(ml_resident(shared, 2, 3, "locked file retained prefix") == 0);
  CHECK(shared[0] == 0x41 && shared[ml_page] == 0x42 && private[0] == 0x77);
  errno = 0;
  CHECK(mlock(alias, ml_page) == -1 && errno == ENOMEM);
  CHECK(ftruncate(fd, 3 * ml_page) == 0);
  CHECK(shared[ml_page + 137] == 0 && shared[2 * ml_page] == 0);
  CHECK(munlock(shared, ml_page) == 0 && mlock(alias, ml_page) == 0);
  errno = 0;
  CHECK(madvise(shared, 3 * ml_page, MADV_DONTNEED) == -1 && errno == EINVAL);
  CHECK(shared[0] == 0x41 && private[0] == 0x77);
out:
  munlockall();
  if (alias != MAP_FAILED)
    munmap(alias, ml_page);
  if (private != MAP_FAILED)
    munmap(private, ml_page);
  if (shared != MAP_FAILED)
    munmap(shared, 3 * ml_page);
  if (fd >= 0)
    close(fd);
  return failed;
}

int ml_managed(void) {
  return future_mappings() || current_mappings() || remap_charge() || file_charge();
}
