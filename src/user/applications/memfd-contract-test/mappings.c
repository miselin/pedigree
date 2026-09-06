#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

static int write_capability(int protection) {
  int failed = 0, fd = -1;
  const size_t page = sysconf(_SC_PAGESIZE);
  char* shared = MAP_FAILED;
  char* private = MAP_FAILED;
  unsigned char resident[2];
  CHECK((fd = mf_make(2 * page)) >= 0);
  CHECK(pwrite(fd, "a", 1, 0) == 1);
  shared = mmap(NULL, 2 * page, protection, MAP_SHARED, fd, 0);
  CHECK(shared != MAP_FAILED);
  /* No access is made through shared: residency cannot stand in for write capability. */
  CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW) == -1 && errno == EBUSY);
  CHECK(fcntl(fd, F_GET_SEALS) == 0);
  CHECK(!mprotect(shared, 2 * page, PROT_NONE));
  CHECK(!madvise(shared, 2 * page, MADV_DONTNEED));
  CHECK(!mincore(shared, 2 * page, resident));
  CHECK(!ftruncate(fd, 3 * page));
  CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY);
  CHECK(!munmap(shared, 2 * page));
  shared = MAP_FAILED;
  private = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  CHECK(private != MAP_FAILED && private[0] == 'a');
  private[0] = 'p';
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE));
  CHECK(private[0] == 'p' && !mf_contents(fd, 0, "a", 1));
  shared = mmap(NULL, 2 * page, PROT_READ, MAP_SHARED, fd, 0);
  CHECK(shared != MAP_FAILED && shared[0] == 'a');
  CHECK(mprotect(shared, 2 * page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
  CHECK(mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED &&
        errno == EPERM);
out:
  if (private != MAP_FAILED)
    munmap(private, page);
  if (shared != MAP_FAILED)
    munmap(shared, 2 * page);
  if (fd >= 0)
    close(fd);
  return failed;
}
static int future_mappings(void) {
  int failed = 0, fd = -1, split = 0, prefix_moved = 0;
  const size_t page = sysconf(_SC_PAGESIZE);
  char *old = MAP_FAILED, *old_read = MAP_FAILED, *new_read = MAP_FAILED;
  char *private = MAP_FAILED, *victim = MAP_FAILED, *moved = MAP_FAILED;
  unsigned char residency;
  CHECK((fd = mf_make(3 * page)) >= 0);
  CHECK(pwrite(fd, "a", 1, 0) == 1 && pwrite(fd, "c", 1, 2 * page) == 1);
  old = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  old_read = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
  CHECK(old != MAP_FAILED && old_read != MAP_FAILED);
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE));
  old[0] = 'b';
  CHECK(!mf_contents(fd, 0, "b", 1));
  CHECK(!mprotect(old_read, page, PROT_READ | PROT_WRITE));
  old_read[0] = 'd';
  CHECK(old[0] == 'd' && !mf_contents(fd, 0, "d", 1));
  CHECK(pwrite(fd, "x", 1, 0) == -1 && errno == EPERM);
  CHECK(mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0) == MAP_FAILED &&
        errno == EPERM);
  new_read = mmap(NULL, page, PROT_READ, MAP_SHARED, fd, 0);
  CHECK(new_read != MAP_FAILED && new_read[0] == 'd');
  CHECK(mprotect(new_read, page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
  private = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  CHECK(private != MAP_FAILED);
  private[0] = 'p';
  CHECK(new_read[0] == 'd' && !mf_contents(fd, 0, "d", 1));
  victim = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(victim != MAP_FAILED);
  memset(victim, 0x5a, page);
  CHECK(mmap(victim, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, fd, 0) == MAP_FAILED &&
        errno == EPERM);
  CHECK(!mincore(victim, page, &residency) && (residency & 1));
  for (size_t n = 0; n < page; ++n)
    CHECK(victim[n] == 0x5a);
  CHECK(!mprotect(old, 3 * page, PROT_READ));
  CHECK(!munmap(old + page, page));
  split = 1;
  CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE | F_SEAL_GROW) == -1 && errno == EBUSY);
  CHECK(fcntl(fd, F_GET_SEALS) == F_SEAL_FUTURE_WRITE);
  moved = mremap(old, page, page, MREMAP_MAYMOVE | MREMAP_FIXED, victim);
  CHECK(moved == victim);
  prefix_moved = 1;
  victim = MAP_FAILED;
  CHECK(!madvise(moved, page, MADV_DONTNEED) && moved[0] == 'd');
  CHECK(!mprotect(moved, page, PROT_READ | PROT_WRITE));
  moved[0] = 'e';
  CHECK(new_read[0] == 'e' && !mf_contents(fd, 0, "e", 1) && private[0] == 'p');
  CHECK(!munmap(old_read, page));
  old_read = MAP_FAILED;
  CHECK(!munmap(moved, page));
  moved = MAP_FAILED;
  CHECK(fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE) == -1 && errno == EBUSY);
  /* Replacing the last old fragment retires its capability just as unmap does. */
  CHECK(mmap(old + 2 * page, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
             -1, 0) == old + 2 * page);
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_WRITE));
  CHECK(fcntl(fd, F_GET_SEALS) == (F_SEAL_WRITE | F_SEAL_FUTURE_WRITE));
  CHECK(new_read[0] == 'e' && private[0] == 'p');
out:
  if (moved != MAP_FAILED)
    munmap(moved, page);
  if (victim != MAP_FAILED)
    munmap(victim, page);
  if (private != MAP_FAILED)
    munmap(private, page);
  if (new_read != MAP_FAILED)
    munmap(new_read, page);
  if (old_read != MAP_FAILED)
    munmap(old_read, page);
  if (old != MAP_FAILED) {
    if (split) {
      if (!prefix_moved)
        munmap(old, page);
      munmap(old + 2 * page, page);
    } else
      munmap(old, 3 * page);
  }
  if (fd >= 0)
    close(fd);
  return failed;
}
static int resize_nonmutation(void) {
  int failed = 0, fd = -1;
  const size_t page = sysconf(_SC_PAGESIZE);
  char* mapping = MAP_FAILED;
  CHECK((fd = mf_make(2 * page)) >= 0);
  mapping = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK(mapping != MAP_FAILED);
  mapping[0] = 'a';
  mapping[page] = 'b';
  CHECK(!fcntl(fd, F_ADD_SEALS, F_SEAL_GROW | F_SEAL_SHRINK));
  CHECK(ftruncate(fd, page) == -1 && errno == EPERM);
  CHECK(ftruncate(fd, 3 * page) == -1 && errno == EPERM);
  CHECK(!mf_size(fd, 2 * page));
  CHECK(mapping[0] == 'a' && mapping[page] == 'b');
  CHECK(!madvise(mapping, 2 * page, MADV_DONTNEED));
  CHECK(mapping[0] == 'a' && mapping[page] == 'b');
  mapping[page] = 'c';
  CHECK(!mf_contents(fd, page, "c", 1));
out:
  if (mapping != MAP_FAILED)
    munmap(mapping, 2 * page);
  if (fd >= 0)
    close(fd);
  return failed;
}
int memfd_mappings(void) {
  return write_capability(PROT_READ) || write_capability(PROT_NONE) ||
         write_capability(PROT_READ | PROT_WRITE) || future_mappings() || resize_nonmutation();
}
