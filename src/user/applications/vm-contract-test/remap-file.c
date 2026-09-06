#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

static int subrange_and_file_victims(void) {
  int failed = 0, fd = -1;
  const size_t p = vm_page;
  unsigned char *source = MAP_FAILED, *target = MAP_FAILED, *backing = MAP_FAILED;
  unsigned char resident;
  CHECK((fd = vm_file(6, NULL)) >= 0);
  CHECK((source = mmap(NULL, 4 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, p)) != MAP_FAILED);
  CHECK((target = mmap(NULL, 5 * p, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) != MAP_FAILED);
  CHECK((backing = mmap(NULL, 6 * p, PROT_READ, MAP_SHARED, fd, 0)) != MAP_FAILED);
  source[0] = 0x99;
  source[2 * p] = 0x77;
  CHECK(close(fd) == 0);
  fd = -1;
  CHECK(mremap(source + p, 2 * p, 2 * p, MREMAP_FIXED | MREMAP_MAYMOVE, target + p) == target + p);
  CHECK(source[0] == 0x99 && source[3 * p] == 0x24);
  CHECK(target[0] == 0x20 && target[p] == 0x22 && target[2 * p] == 0x77 && target[3 * p] == 0x23 &&
        target[4 * p] == 0x24);
  CHECK(backing[p] == 0x21 && backing[3 * p] == 0x23);
  target[p + 1] = 0x78;
  CHECK(backing[2 * p + 1] == 0x22);
  CHECK(mincore(source + p, p, &resident) == -1 && errno == ENOMEM);
  CHECK(mmap(source + p, 2 * p, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == source + p);
  CHECK(vm_uniform(source + p, 2 * p, 0) && source[3 * p] == 0x24);
out:
  if (fd >= 0)
    close(fd);
  if (source != MAP_FAILED)
    munmap(source, 4 * p);
  if (target != MAP_FAILED)
    munmap(target, 5 * p);
  if (backing != MAP_FAILED)
    munmap(backing, 6 * p);
  return failed;
}

static int shared_growth_and_eof(void) {
  int failed = 0, fd = -1;
  const size_t p = vm_page;
  unsigned char *source = MAP_FAILED, *target = MAP_FAILED, *alias = MAP_FAILED;
  unsigned char resident[3];
  CHECK((fd = vm_file(3, NULL)) >= 0);
  CHECK((source = mmap(NULL, 2 * p, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p)) != MAP_FAILED);
  CHECK((alias = mmap(NULL, 3 * p, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) != MAP_FAILED);
  CHECK((target = mmap(NULL, 3 * p, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED);
  source[0] = 0xb0;
  CHECK(alias[p] == 0xb0 && close(fd) == 0);
  fd = -1;
  CHECK(mremap(source, 2 * p, 3 * p, MREMAP_FIXED | MREMAP_MAYMOVE, target) == target);
  CHECK(target[0] == 0xb0 && target[p] == 0x22);
  target[0] = 0xb1;
  alias[2 * p] = 0xb2;
  CHECK(alias[p] == 0xb1 && target[p] == 0xb2);
  CHECK(mincore(target, 3 * p, resident) == 0 && resident[2] == 0);
  CHECK(vm_fault(target + 2 * p, 0, SIGBUS) == 0);
out:
  if (fd >= 0)
    close(fd);
  if (source != MAP_FAILED)
    munmap(source, 2 * p);
  if (target != MAP_FAILED)
    munmap(target, 3 * p);
  if (alias != MAP_FAILED)
    munmap(alias, 3 * p);
  return failed;
}

static int retained_protections(void) {
  int failed = 0, fd = -1, readonly = -1;
  const size_t p = vm_page;
  unsigned char *source = MAP_FAILED, *target = MAP_FAILED;
  unsigned char resident[2];
  CHECK((fd = vm_file(3, &readonly)) >= 0);
  CHECK((source = mmap(NULL, 2 * p, PROT_READ, MAP_SHARED, readonly, p)) != MAP_FAILED);
  CHECK((target = mmap(NULL, 2 * p, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED);
  CHECK(source[0] == 0x21);
  CHECK(close(fd) == 0 && close(readonly) == 0);
  fd = readonly = -1;
  CHECK(mremap(source, 2 * p, 2 * p, MREMAP_FIXED | MREMAP_MAYMOVE, target) == target);
  CHECK(target[0] == 0x21 && target[p] == 0x22);
  CHECK(mprotect(target, 2 * p, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
  CHECK(vm_fault(target, 1, SIGSEGV) == 0);
  CHECK(mprotect(target, 2 * p, PROT_NONE) == 0);
  CHECK(mremap(target, 2 * p, 2 * p, MREMAP_FIXED | MREMAP_MAYMOVE, source) == source);
  CHECK(mincore(source, 2 * p, resident) == 0 && resident[0] == 1 && resident[1] == 1);
  CHECK(vm_fault(source, 0, SIGSEGV) == 0);
  CHECK(mprotect(source, 2 * p, PROT_READ) == 0 && source[0] == 0x21 && source[p] == 0x22);
  CHECK(mprotect(source, 2 * p, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
out:
  if (fd >= 0)
    close(fd);
  if (readonly >= 0)
    close(readonly);
  if (source != MAP_FAILED)
    munmap(source, 2 * p);
  if (target != MAP_FAILED)
    munmap(target, 2 * p);
  return failed;
}

int vm_test_remap_file(void) {
  return subrange_and_file_victims() || shared_growth_and_eof() || retained_protections();
}
