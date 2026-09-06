#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>

static int attachment_move(void) {
  int failed = 0, id = -1;
  const size_t p = vm_page;
  pid_t child = -1;
  unsigned char *view = (void*)-1, *alias = (void*)-1, *target = MAP_FAILED;
  struct shmid_ds status;
  CHECK((id = shmget(IPC_PRIVATE, 3 * p, 0600)) >= 0);
  CHECK((view = shmat(id, NULL, 0)) != (void*)-1 &&
        (alias = shmat(id, NULL, SHM_RDONLY)) != (void*)-1);
  for (int n = 0; n < 3; ++n)
    memset(view + n * p, 0xa1 + n, p);
  CHECK(shmctl(id, IPC_RMID, NULL) == 0);
  CHECK((target = mmap(NULL, 3 * p, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) != MAP_FAILED);
  void* old = view;
  CHECK(mremap(view, 3 * p, 3 * p, MREMAP_MAYMOVE | MREMAP_FIXED, target) == target);
  view = target;
  target = MAP_FAILED;
  CHECK(shmdt(old) == -1 && errno == EINVAL);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 2 && status.shm_segsz == 3 * p);
  CHECK(vm_uniform(view, p, 0xa1) && vm_uniform(alias + 2 * p, p, 0xa3));
  view[0] = 0xa4;
  CHECK(alias[0] == 0xa4 && (child = fork()) >= 0);
  if (!child) {
    alarm(5);
    if (shmctl(id, IPC_STAT, &status) || status.shm_nattch != 4 || view[0] != 0xa4 || shmdt(view) ||
        shmctl(id, IPC_STAT, &status) || status.shm_nattch != 3)
      _exit(10);
    _exit(0);
  }
  int result = vm_reap(child, 3000);
  child = -1;
  CHECK(result == 0 && shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 2);
  CHECK(mremap(view, 3 * p, 2 * p, 0) == view);
  CHECK(view[0] == 0xa4 && view[p] == 0xa2 && alias[2 * p] == 0xa3);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 2 && status.shm_segsz == 3 * p);
  CHECK(shmdt(view) == 0);
  view = (void*)-1;
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 1);
  CHECK(shmdt(alias) == 0);
  alias = (void*)-1;
  CHECK(shmctl(id, IPC_STAT, &status) == -1 && errno == EINVAL);
  id = -1;
out:
  if (child > 0) {
    kill(child, SIGKILL);
    vm_reap(child, 1000);
  }
  if (target != MAP_FAILED)
    munmap(target, 3 * p);
  if (view != (void*)-1)
    shmdt(view);
  if (alias != (void*)-1)
    shmdt(alias);
  if (id >= 0)
    shmctl(id, IPC_RMID, NULL);
  return failed;
}

static int rejected_attachment_forms(void) {
  int failed = 0, id = -1;
  const size_t p = vm_page;
  unsigned char* view = (void*)-1;
  struct shmid_ds status;
  CHECK((id = shmget(IPC_PRIVATE, 3 * p, 0600)) >= 0 && (view = shmat(id, NULL, 0)) != (void*)-1);
  memset(view, 0xb1, 3 * p);
  CHECK(mremap(view + p, p, p, MREMAP_MAYMOVE) == MAP_FAILED && errno == EOPNOTSUPP);
  CHECK(mremap(view, 3 * p, 4 * p, MREMAP_MAYMOVE) == MAP_FAILED && errno == EOPNOTSUPP);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 1 &&
        vm_uniform(view, 3 * p, 0xb1));
  CHECK(munmap(view + p, p) == 0);
  CHECK(mremap(view, p, p, MREMAP_MAYMOVE) == MAP_FAILED && errno == EOPNOTSUPP);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 1 && vm_uniform(view, p, 0xb1) &&
        vm_uniform(view + 2 * p, p, 0xb1));
  CHECK(shmdt(view) == 0);
  view = (void*)-1;
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 0);
out:
  if (view != (void*)-1)
    shmdt(view);
  if (id >= 0)
    shmctl(id, IPC_RMID, NULL);
  return failed;
}

static int attachment_victims(void) {
  int failed = 0, id = -1, attached = 0;
  const size_t p = vm_page;
  unsigned char *view = (void*)-1, *alias = (void*)-1, *source = MAP_FAILED;
  struct shmid_ds status;
  CHECK((id = shmget(IPC_PRIVATE, 3 * p, 0600)) >= 0);
  CHECK((view = shmat(id, NULL, 0)) != (void*)-1);
  attached = 1;
  CHECK((alias = shmat(id, NULL, 0)) != (void*)-1);
  memset(view, 0xc1, 3 * p);
  const int order[3] = {1, 0, 2};
  for (int n = 0; n < 3; ++n) {
    CHECK((source = mmap(NULL, p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) !=
          MAP_FAILED);
    memset(source, 0xd1 + n, p);
    unsigned char* victim = view + order[n] * p;
    CHECK(mremap(source, p, p, MREMAP_MAYMOVE | MREMAP_FIXED, victim) == victim);
    source = MAP_FAILED;
    if (n == 2)
      attached = 0;
    CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == (n == 2 ? 1U : 2U));
    CHECK(vm_uniform(victim, p, 0xd1 + n) && vm_uniform(alias, 3 * p, 0xc1));
  }
  CHECK(shmdt(view) == -1 && errno == EINVAL);
  CHECK(vm_uniform(view, p, 0xd2) && vm_uniform(view + p, p, 0xd1) &&
        vm_uniform(view + 2 * p, p, 0xd3));
out:
  if (source != MAP_FAILED)
    munmap(source, p);
  if (attached)
    shmdt(view);
  if (view != (void*)-1)
    munmap(view, 3 * p);
  if (alias != (void*)-1)
    shmdt(alias);
  if (id >= 0)
    shmctl(id, IPC_RMID, NULL);
  return failed;
}

int vm_test_remap_shm(void) {
  return attachment_move() || rejected_attachment_forms() || attachment_victims();
}
