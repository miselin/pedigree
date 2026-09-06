#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>

static int anonymous_discard(void) {
  int failed = 0;
  const size_t p = vm_page;
  unsigned char* area =
      mmap(NULL, 4 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char* target = MAP_FAILED;
  unsigned char vector[4];
  CHECK(area != MAP_FAILED);
  memset(area, 0x71, 4 * p);
  CHECK(mprotect(area + p, p, PROT_READ) == 0 && mprotect(area + 2 * p, p, PROT_NONE) == 0);
  CHECK(madvise(area + p, 2 * p, MADV_DONTNEED) == 0);
  CHECK(mincore(area, 4 * p, vector) == 0 && vector[0] == 1 && vector[1] == 0 && vector[2] == 0 &&
        vector[3] == 1);
  CHECK(vm_uniform(area, p, 0x71) && vm_uniform(area + p, p, 0) &&
        vm_uniform(area + 3 * p, p, 0x71));
  CHECK(vm_fault(area + p, 1, SIGSEGV) == 0 && vm_fault(area + 2 * p, 0, SIGSEGV) == 0);
  CHECK(mprotect(area + 2 * p, p, PROT_READ | PROT_WRITE) == 0 && vm_uniform(area + 2 * p, p, 0));
  CHECK(madvise(area + 1, p, MADV_DONTNEED) == -1 && errno == EINVAL);
  CHECK(madvise(area, p, MADV_FREE) == -1 && errno == EINVAL);
  CHECK(madvise(area, p, 0x7fffffff) == -1 && errno == EINVAL);
  CHECK(madvise(area, 0, MADV_DONTNEED) == 0 && vm_uniform(area, p, 0x71));
  CHECK(munmap(area + 3 * p, p) == 0);
  CHECK(madvise(area, 4 * p, MADV_DONTNEED) == -1 && errno == ENOMEM);
  CHECK(vm_uniform(area, p, 0x71));
  CHECK(madvise(area, p, MADV_DONTNEED) == 0);
  CHECK((target = mmap(NULL, p, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED);
  CHECK(mremap(area, p, p, MREMAP_FIXED | MREMAP_MAYMOVE, target) == target);
  CHECK(mincore(target, p, vector) == 0 && vector[0] == 0 && vm_uniform(target, p, 0));
out:
  if (target != MAP_FAILED)
    munmap(target, p);
  if (area != MAP_FAILED)
    munmap(area, 4 * p);
  return failed;
}

static int file_discard(void) {
  int failed = 0, fd = -1;
  const size_t p = vm_page;
  unsigned char *private = MAP_FAILED, *shared = MAP_FAILED, *alias = MAP_FAILED,
                *target = MAP_FAILED;
  CHECK((fd = vm_file(3, NULL)) >= 0);
  CHECK((private = mmap(NULL, 2 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, p)) != MAP_FAILED);
  CHECK((shared = mmap(NULL, 2 * p, PROT_READ | PROT_WRITE, MAP_SHARED, fd, p)) != MAP_FAILED);
  CHECK((alias = mmap(NULL, 3 * p, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0)) != MAP_FAILED);
  private[0] = 0xe1;
  shared[p] = 0xe2;
  CHECK(alias[p] == 0x21 && alias[2 * p] == 0xe2);
  CHECK(close(fd) == 0);
  fd = -1;
  CHECK(madvise(private, 2 * p, MADV_DONTNEED) == 0 && private[0] == 0x21 && private[p] == 0xe2);
  CHECK(madvise(shared, 2 * p, MADV_DONTNEED) == 0 && shared[0] == 0x21 && shared[p] == 0xe2);
  CHECK(alias[2 * p] == 0xe2);
  alias[p] = 0xe3;
  CHECK(shared[0] == 0xe3);
  CHECK(mprotect(private, 2 * p, PROT_READ) == 0 && madvise(private, 2 * p, MADV_DONTNEED) == 0);
  CHECK(private[0] == 0xe3 && private[p] == 0xe2);
  CHECK(vm_fault(private, 1, SIGSEGV) == 0);
  CHECK((target = mmap(NULL, 2 * p, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED);
  CHECK(mremap(private, 2 * p, 2 * p, MREMAP_MAYMOVE | MREMAP_FIXED, target) == target);
  CHECK(target[0] == 0xe3 && target[p] == 0xe2 && vm_fault(target, 1, SIGSEGV) == 0);
out:
  if (fd >= 0)
    close(fd);
  if (private != MAP_FAILED)
    munmap(private, 2 * p);
  if (shared != MAP_FAILED)
    munmap(shared, 2 * p);
  if (alias != MAP_FAILED)
    munmap(alias, 3 * p);
  if (target != MAP_FAILED)
    munmap(target, 2 * p);
  return failed;
}

static int shm_discard(void) {
  int failed = 0, id = -1;
  const size_t p = vm_page;
  unsigned char *view = (void*)-1, *alias = (void*)-1;
  struct shmid_ds status;
  CHECK((id = shmget(IPC_PRIVATE, 2 * p, 0600)) >= 0);
  CHECK((view = shmat(id, NULL, 0)) != (void*)-1 && (alias = shmat(id, NULL, 0)) != (void*)-1);
  memset(view, 0xf1, 2 * p);
  if (geteuid() == 0)
    CHECK(shmctl(id, SHM_LOCK, NULL) == 0);
  CHECK(madvise(view, 2 * p, MADV_DONTNEED) == 0 && vm_uniform(alias, 2 * p, 0xf1) &&
        vm_uniform(view, 2 * p, 0xf1));
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 2 && status.shm_segsz == 2 * p);
  if (geteuid() == 0)
    CHECK(shmctl(id, SHM_UNLOCK, NULL) == 0);
  CHECK(shmctl(id, IPC_RMID, NULL) == 0);
  CHECK(madvise(view, 2 * p, MADV_DONTNEED) == 0 && madvise(alias, 2 * p, MADV_DONTNEED) == 0);
  CHECK(vm_uniform(view, 2 * p, 0xf1));
  alias[0] = 0xf2;
  CHECK(view[0] == 0xf2);
out:
  if (view != (void*)-1)
    shmdt(view);
  if (alias != (void*)-1)
    shmdt(alias);
  if (id >= 0)
    shmctl(id, IPC_RMID, NULL);
  return failed;
}

static int fork_discard(void) {
  int failed = 0, gate[2] = {-1, -1};
  const size_t p = vm_page;
  pid_t child = -1;
  unsigned char* area =
      mmap(NULL, 2 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(area != MAP_FAILED);
  memset(area, 0x81, 2 * p);
  CHECK(pipe(gate) == 0 && (child = fork()) >= 0);
  if (!child) {
    alarm(5);
    char byte;
    if (read(gate[0], &byte, 1) != 1 || !vm_uniform(area, 2 * p, 0x81))
      _exit(10);
    for (int n = 0; n < 8; ++n) {
      if (madvise(area, 2 * p, MADV_DONTNEED) || !vm_uniform(area, 2 * p, 0))
        _exit(11);
      memset(area, 0xd0 + n, 2 * p);
      vm_pause(2);
      if (!vm_uniform(area, 2 * p, 0xd0 + n))
        _exit(12);
    }
    _exit(0);
  }
  CHECK(write(gate[1], "g", 1) == 1);
  for (int n = 0; n < 8; ++n) {
    memset(area, 0xa0 + n, 2 * p);
    vm_pause(2);
    CHECK(vm_uniform(area, 2 * p, 0xa0 + n));
  }
  int result = vm_reap(child, 3000);
  child = -1;
  CHECK(result == 0 && vm_uniform(area, 2 * p, 0xa7));
out:
  if (child > 0) {
    kill(child, SIGKILL);
    vm_reap(child, 1000);
  }
  if (area != MAP_FAILED)
    munmap(area, 2 * p);
  for (int n = 0; n < 2; ++n)
    if (gate[n] >= 0)
      close(gate[n]);
  return failed;
}

int vm_test_discard(void) {
  return anonymous_discard() || file_discard() || shm_discard() || fork_discard();
}
