#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/stat.h>

static int invalid_ranges(void) {
  int failed = 0, shmid = -1;
  struct rp_file file = {.fd = -1};
  const size_t page = rp_page;
  unsigned char *mapping = MAP_FAILED, *private = MAP_FAILED, *hole = MAP_FAILED;
  unsigned char* shared_memory = (void*)-1;
  volatile unsigned char stack_byte = 0x73;
  CHECK(!rp_create(&file, RP_MEMFD));
  mapping = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
  private = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, file.fd, 0);
  hole = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
  CHECK(mapping != MAP_FAILED && private != MAP_FAILED && hole != MAP_FAILED);
  CHECK(remap_file_pages(mapping, page, PROT_READ, 1, 0) == -1 && errno == EINVAL);
  CHECK(remap_file_pages(mapping, 0, 0, 1, 0) == -1 && errno == EINVAL);
  CHECK(remap_file_pages(mapping, page - 1, 0, 1, 0) == -1 && errno == EINVAL);
  CHECK(remap_file_pages(mapping, 2 * page, 0, SIZE_MAX, 0) == -1 && errno == EINVAL);
  CHECK(remap_file_pages(mapping, page, 0, (size_t)INT64_MAX / page + 1, 0) == -1 &&
        errno == EOVERFLOW);
  CHECK(remap_file_pages((void*)(UINTPTR_MAX - page + 1), 2 * page, 0, 0, 0) == -1 &&
        errno == EINVAL);
  CHECK(remap_file_pages(NULL, page, 0, 0, 0) == -1 && errno == EINVAL);
  CHECK(rp_matches(mapping, 0, 4 * page, 0));
  private[71] = 0xd6;
  CHECK(remap_file_pages(private, page, 0, 1, 0) == -1 && errno == EINVAL);
  CHECK(private[71] == 0xd6 && mapping[71] == rp_pattern(71));
  CHECK(!munmap(hole + page, page));
  CHECK(remap_file_pages(hole, 3 * page, 0, 3, 0) == -1 && errno == EINVAL);
  CHECK(rp_matches(hole, 0, page, 0) && rp_matches(hole + 2 * page, 2 * page, page, 0));
  CHECK(!mprotect(mapping + page, page, PROT_READ));
  CHECK(remap_file_pages(mapping, 2 * page, 0, 4, 0) == -1 && errno == EINVAL);
  CHECK(rp_matches(mapping, 0, 4 * page, 0));
  CHECK(!mprotect(mapping, 2 * page, PROT_READ | PROT_WRITE));
  CHECK(!remap_file_pages(mapping, 2 * page, 0, 4, 0));
  CHECK(rp_matches(mapping, 4 * page, 2 * page, 0));
  CHECK(!mprotect(mapping, 2 * page, PROT_READ));
  CHECK(!remap_file_pages(mapping, 2 * page, 0, 2, 0));
  CHECK(rp_matches(mapping, 2 * page, 2 * page, 0) && !rp_fault(mapping, SIGSEGV, 1));
  CHECK(!mprotect(mapping, page, PROT_NONE));
  CHECK(!remap_file_pages(mapping, page, 0, 6, 0));
  CHECK(!rp_fault(mapping, SIGSEGV, 0));
  CHECK(!mprotect(mapping, page, PROT_READ));
  CHECK(rp_matches(mapping, 6 * page, page, 0));

  CHECK(remap_file_pages((void*)&stack_byte, page, 0, 0, 0) == -1 && errno == EOPNOTSUPP);
  CHECK(stack_byte == 0x73);
  CHECK((shmid = shmget(IPC_PRIVATE, page, IPC_CREAT | 0600)) >= 0);
  CHECK((shared_memory = shmat(shmid, NULL, 0)) != (void*)-1);
  CHECK(!shmctl(shmid, IPC_RMID, NULL));
  shmid = -1;
  shared_memory[0] = 0x49;
  CHECK(remap_file_pages(shared_memory, page, 0, 0, 0) == -1 && errno == EOPNOTSUPP);
  CHECK(shared_memory[0] == 0x49);
out:
  if (shared_memory != (void*)-1)
    shmdt(shared_memory);
  if (shmid >= 0)
    shmctl(shmid, IPC_RMID, NULL);
  if (hole != MAP_FAILED)
    munmap(hole, 3 * page);
  if (private != MAP_FAILED)
    munmap(private, page);
  if (mapping != MAP_FAILED)
    munmap(mapping, 4 * page);
  rp_close(&file);
  return failed;
}

static int separate_opens(int backend) {
  int failed = 0, other = -1, readonly = -1;
  struct rp_file file = {.fd = -1};
  const size_t page = rp_page;
  unsigned char *area = MAP_FAILED, *read_mapping = MAP_FAILED;
  struct stat first, second;
  CHECK(!rp_create(&file, backend));
  CHECK((other = rp_open_alias(&file)) >= 0 && (readonly = open(file.path, O_RDONLY)) >= 0);
  CHECK(!fstat(file.fd, &first) && !fstat(other, &second) && first.st_ino == second.st_ino &&
        first.st_dev == second.st_dev);
  area = mmap(NULL, 2 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(area != MAP_FAILED);
  CHECK(mmap(area, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, file.fd, 0) == area);
  CHECK(mmap(area + page, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, other, page) ==
        area + page);
  CHECK(remap_file_pages(area, 2 * page, 0, 4, 0) == -1 && errno == EINVAL);
  CHECK(rp_matches(area, 0, 2 * page, 0));
  read_mapping = mmap(NULL, page, PROT_READ, MAP_SHARED, readonly, 0);
  CHECK(read_mapping != MAP_FAILED);
  CHECK(remap_file_pages(read_mapping, page, 0, 1, 0) == -1 && errno == EINVAL);
  CHECK(rp_matches(read_mapping, 0, page, 0));
out:
  if (read_mapping != MAP_FAILED)
    munmap(read_mapping, page);
  if (area != MAP_FAILED)
    munmap(area, 2 * page);
  if (readonly >= 0)
    close(readonly);
  if (other >= 0)
    close(other);
  rp_close(&file);
  return failed;
}

static int seals(void) {
  int failed = 0;
  struct rp_file file = {.fd = -1};
  const size_t page = rp_page;
  unsigned char* mapping = MAP_FAILED;
  CHECK(!rp_create(&file, RP_MEMFD));
  mapping = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
  CHECK(mapping != MAP_FAILED && rp_matches(mapping, 0, 3 * page, 0));
  CHECK(!fcntl(file.fd, F_ADD_SEALS, F_SEAL_FUTURE_WRITE));
  CHECK(remap_file_pages(mapping, 3 * page, 0, 2, 0) == -1 && errno == EPERM);
  CHECK(rp_matches(mapping, 0, 3 * page, 0));
  CHECK(!mprotect(mapping, 3 * page, PROT_READ));
  CHECK(mmap(mapping + 2 * page, page, PROT_READ, MAP_SHARED | MAP_FIXED, file.fd, 2 * page) ==
        mapping + 2 * page);
  CHECK(remap_file_pages(mapping, 3 * page, 0, 2, 0) == -1 && errno == EINVAL);
  CHECK(rp_matches(mapping, 0, 3 * page, 0));
  CHECK(!remap_file_pages(mapping, 2 * page, 0, 2, MAP_NONBLOCK));
  CHECK(!remap_file_pages(mapping, 3 * page, 0, 2, MAP_NONBLOCK));
  CHECK(rp_matches(mapping, 2 * page, 3 * page, 0));
  CHECK(mprotect(mapping, 3 * page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
  CHECK(!fcntl(file.fd, F_ADD_SEALS, F_SEAL_WRITE));
  CHECK(!remap_file_pages(mapping, 3 * page, 0, 4, 0));
  CHECK(rp_matches(mapping, 4 * page, 3 * page, 0));
out:
  if (mapping != MAP_FAILED)
    munmap(mapping, 3 * page);
  rp_close(&file);
  return failed;
}

int rp_admission(void) {
  return invalid_ranges() || separate_opens(RP_RAMFS) || separate_opens(RP_EXT2) || seals();
}
