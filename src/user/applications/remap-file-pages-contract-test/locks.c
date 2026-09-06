#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

static int replacement_charge(void) {
  int failed = 0;
  struct rp_file file = {.fd = -1};
  unsigned char* mapping = MAP_FAILED;
  void* extra = MAP_FAILED;
  CHECK(rp_create(&file, RP_MEMFD) == 0);
  mapping = mmap(NULL, 2 * rp_page, PROT_READ, MAP_SHARED, file.fd, 0);
  extra = mmap(NULL, 2 * rp_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(mapping != MAP_FAILED && extra != MAP_FAILED);
  CHECK(rp_limit(2 * rp_page) == 0 && mlock(mapping, 2 * rp_page) == 0);
  errno = 0;
  CHECK(remap_file_pages(mapping, 2 * rp_page, 0, 4, MAP_NONBLOCK) == -1 && errno == EAGAIN);
  CHECK(rp_matches(mapping, 0, 2 * rp_page, 0));
  CHECK(rp_limit(4 * rp_page) == 0);
  CHECK(remap_file_pages(mapping, 2 * rp_page, 0, 4, MAP_NONBLOCK) == 0);
  CHECK(rp_matches(mapping, 4 * rp_page, 2 * rp_page, 0));
  // The replacement consumes two pages after commit, despite admitting four before it.
  CHECK(mlock(extra, 2 * rp_page) == 0);
  errno = 0;
  CHECK(msync(mapping, 2 * rp_page, MS_SYNC | MS_INVALIDATE) == -1 && errno == EBUSY);
  CHECK(munlock(extra, 2 * rp_page) == 0 && rp_limit(0) == 0);
  errno = 0;
  CHECK(remap_file_pages(mapping, 2 * rp_page, 0, 0, 0) == -1 && errno == EPERM);
  CHECK(rp_matches(mapping, 4 * rp_page, 2 * rp_page, 0));
  CHECK(munlock(mapping, 2 * rp_page) == 0);
  CHECK(msync(mapping, 2 * rp_page, MS_SYNC | MS_INVALIDATE) == 0);
  CHECK(rp_limit(2 * rp_page) == 0 && mlock(extra, 2 * rp_page) == 0);
out:
  munlockall();
  if (extra != MAP_FAILED)
    munmap(extra, 2 * rp_page);
  if (mapping != MAP_FAILED)
    munmap(mapping, 2 * rp_page);
  rp_close(&file);
  return failed;
}

static int future_zero_limit(void) {
  int failed = 0;
  struct rp_file file = {.fd = -1};
  unsigned char* mapping = MAP_FAILED;
  CHECK(rp_create(&file, RP_MEMFD) == 0);
  mapping = mmap(NULL, rp_page, PROT_READ, MAP_SHARED, file.fd, 0);
  CHECK(mapping != MAP_FAILED && rp_matches(mapping, 0, rp_page, 0));
  CHECK(rp_limit(2 * rp_page) == 0 && mlockall(MCL_FUTURE | MCL_ONFAULT) == 0);
  CHECK(rp_limit(0) == 0);
  errno = 0;
  CHECK(remap_file_pages(mapping, rp_page, 0, 4, 0) == -1 && errno == EAGAIN);
  CHECK(rp_matches(mapping, 0, rp_page, 0));
  CHECK(munlockall() == 0);
  CHECK(remap_file_pages(mapping, rp_page, 0, 4, MAP_NONBLOCK) == 0);
  CHECK(rp_matches(mapping, 4 * rp_page, rp_page, 0));
out:
  munlockall();
  if (mapping != MAP_FAILED)
    munmap(mapping, rp_page);
  rp_close(&file);
  return failed;
}

static int lock_mode_conversion(void) {
  int failed = 0;
  struct rp_file file = {.fd = -1};
  unsigned char* mapping = MAP_FAILED;
  CHECK(rp_create(&file, RP_MEMFD) == 0 && rp_limit(8 * rp_page) == 0);
  mapping = mmap(NULL, 2 * rp_page, PROT_READ, MAP_SHARED, file.fd, 0);
  CHECK(mapping != MAP_FAILED);
  CHECK(mlock2(mapping, rp_page, MLOCK_ONFAULT) == 0);
  CHECK(mlock(mapping + rp_page, rp_page) == 0);
  errno = 0;
  CHECK(remap_file_pages(mapping, 2 * rp_page, 0, 4, 0) == -1 && errno == EINVAL);
  CHECK(rp_matches(mapping, 0, 2 * rp_page, 0));
  CHECK(remap_file_pages(mapping, rp_page, 0, 4, MAP_NONBLOCK) == 0);
  CHECK(rp_matches(mapping, 4 * rp_page, rp_page, 0));
  // Equal adjacent lock modes admit a spanning remap; this observes ONFAULT becoming eager.
  CHECK(remap_file_pages(mapping, 2 * rp_page, 0, 2, 0) == 0);
  CHECK(rp_matches(mapping, 2 * rp_page, 2 * rp_page, 0));
  errno = 0;
  CHECK(msync(mapping, 2 * rp_page, MS_SYNC | MS_INVALIDATE) == -1 && errno == EBUSY);
  CHECK(rp_fault(mapping, SIGSEGV, 1) == 0);
  CHECK(mprotect(mapping, rp_page, PROT_NONE) == 0);
  CHECK(rp_fault(mapping, SIGSEGV, 0) == 0);
  CHECK(mprotect(mapping, rp_page, PROT_READ) == 0);
  CHECK(rp_matches(mapping, 2 * rp_page, rp_page, 0));
  CHECK(munlock(mapping, rp_page) == 0);
  CHECK(msync(mapping, rp_page, MS_SYNC | MS_INVALIDATE) == 0);
  errno = 0;
  CHECK(msync(mapping + rp_page, rp_page, MS_SYNC | MS_INVALIDATE) == -1 && errno == EBUSY);
  CHECK(munlock(mapping + rp_page, rp_page) == 0);
  CHECK(msync(mapping + rp_page, rp_page, MS_SYNC | MS_INVALIDATE) == 0);
out:
  munlockall();
  if (mapping != MAP_FAILED)
    munmap(mapping, 2 * rp_page);
  rp_close(&file);
  return failed;
}

static int future_onfault_mode(void) {
  int failed = 0;
  struct rp_file file = {.fd = -1};
  unsigned char* mapping = MAP_FAILED;
  CHECK(rp_create(&file, RP_MEMFD) == 0 && rp_limit(8 * rp_page) == 0);
  mapping = mmap(NULL, 2 * rp_page, PROT_READ, MAP_SHARED, file.fd, 0);
  CHECK(mapping != MAP_FAILED && mlock2(mapping, 2 * rp_page, MLOCK_ONFAULT) == 0);
  CHECK(mlockall(MCL_FUTURE | MCL_ONFAULT) == 0);
  CHECK(remap_file_pages(mapping, rp_page, 0, 4, 0) == 0);
  // FUTURE ONFAULT keeps the changed fragment compatible with its untouched neighbor.
  CHECK(remap_file_pages(mapping, 2 * rp_page, 0, 2, 0) == 0);
  CHECK(rp_matches(mapping, 2 * rp_page, 2 * rp_page, 0));
  errno = 0;
  CHECK(msync(mapping, 2 * rp_page, MS_SYNC | MS_INVALIDATE) == -1 && errno == EBUSY);
  CHECK(munlockall() == 0);
  CHECK(msync(mapping, 2 * rp_page, MS_SYNC | MS_INVALIDATE) == 0);
out:
  munlockall();
  if (mapping != MAP_FAILED)
    munmap(mapping, 2 * rp_page);
  rp_close(&file);
  return failed;
}

int rp_locks(void) {
  if (rp_unprivileged())
    return 1;
  return replacement_charge() || future_zero_limit() || lock_mode_conversion() ||
         future_onfault_mode();
}
