#define _GNU_SOURCE
#include <errno.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/ipc.h>
#include <sys/mman.h>
#include <sys/shm.h>
#include <sys/wait.h>

#define CHECK(condition)                                                                 \
  do {                                                                                   \
    if (!(condition)) {                                                                  \
      fprintf(stderr, "shared-memory:%d: %s (errno=%d)\n", __LINE__, #condition, errno); \
      failed = 1;                                                                        \
      goto out;                                                                          \
    }                                                                                    \
  } while (0)

static int reap(pid_t child) {
  int status = 0;
  for (int n = 0; n < 500; ++n) {
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
    if (result < 0 && errno != EINTR)
      return -1;
    usleep(10000);
  }
  kill(child, SIGKILL);
  waitpid(child, &status, 0);
  return -1;
}

int ipc_test_shared_memory_exec(int argc, char** argv) {
  if (argc != 4)
    return 1;
  int id = atoi(argv[2]);
  unsigned long expected = strtoul(argv[3], NULL, 10);
  struct shmid_ds status;
  alarm(5);
  if (shmctl(id, IPC_STAT, &status) || status.shm_nattch != expected)
    return 2;
  char* view = shmat(id, NULL, SHM_RDONLY);
  if (view == (void*)-1 || strcmp(view, "child-shared"))
    return 3;
  if (shmdt(view) || shmctl(id, IPC_STAT, &status) || status.shm_nattch != expected)
    return 4;
  return 0;
}

int ipc_test_shared_memory(void) {
  int failed = 0, id = -1, separate = -1, split_id = -1;
  char *view = (void*)-1, *readonly = (void*)-1, *remapped = (void*)-1;
  char *split = (void*)-1, *replacement = MAP_FAILED, *middle = MAP_FAILED;
  void *reserved = MAP_FAILED, *bad = MAP_FAILED;
  const size_t page = (size_t)sysconf(_SC_PAGESIZE);
  const size_t length = page * 3;
  key_t key = (key_t)(0x62000000U | (unsigned)getpid());
  struct shmid_ds status, changed;
  struct shminfo limits;
  struct shm_info usage;
  alarm(30);

  CHECK(shmget(IPC_PRIVATE, 0, 0600) == -1 && errno == EINVAL);
  id = shmget(key, length - 7, IPC_CREAT | IPC_EXCL | 0600);
  CHECK(id >= 0);
  CHECK(shmget(key, 0, 0600) == id);
  CHECK(shmget(key, length, 0600) == -1 && errno == EINVAL);
  CHECK(shmget(key, 1, IPC_CREAT | IPC_EXCL | 0600) == -1 && errno == EEXIST);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_segsz == length - 7 &&
        status.shm_nattch == 0 && status.shm_cpid == getpid());
  view = shmat(id, NULL, 0);
  readonly = shmat(id, NULL, SHM_RDONLY);
  CHECK(view != (void*)-1 && readonly != (void*)-1 && view != readonly);
  CHECK(view[0] == 0 && view[length - 1] == 0);
  strcpy(view, "parent-shared");
  view[page * 2] = 42;
  CHECK(!strcmp(readonly, "parent-shared") && readonly[page * 2] == 42);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 2 && status.shm_atime != 0);
  CHECK(mprotect(readonly, length, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
  CHECK(shmdt(view + page) == -1 && errno == EINVAL);

  pid_t child = fork();
  CHECK(child >= 0);
  if (!child) {
    alarm(5);
    if (strcmp(readonly, "parent-shared"))
      _exit(1);
    strcpy(view, "child-shared");
    char* extra = shmat(id, NULL, 0);
    if (extra == (void*)-1)
      _exit(2);
    if (shmctl(id, IPC_STAT, &status) || status.shm_nattch != 5)
      _exit(3);
    extra[page] = 73;
    if (shmdt(extra))
      _exit(4);
    _exit(0);
  }
  CHECK(reap(child) == 0);
  CHECK(!strcmp(view, "child-shared") && readonly[page] == 73);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 2 && status.shm_dtime != 0);

  child = fork();
  CHECK(child >= 0);
  if (!child) {
    char identifier[32];
    snprintf(identifier, sizeof(identifier), "%d", id);
    execl("/applications/ipc-contract-test", "ipc-contract-test", "shm-exec", identifier, "2",
          NULL);
    _exit(127);
  }
  CHECK(reap(child) == 0);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 2);

  reserved = mmap(NULL, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(reserved != MAP_FAILED);
  CHECK(shmat(id, reserved, 0) == (void*)-1 && errno == EINVAL);
  CHECK(shmat(id, (char*)reserved + 3, 0) == (void*)-1 && errno == EINVAL);
  CHECK(shmat(id, NULL, SHM_REMAP) == (void*)-1 && errno == EINVAL);
  remapped = shmat(id, (char*)reserved + 3, SHM_RND | SHM_REMAP);
  CHECK(remapped == reserved && !strcmp(remapped, "child-shared"));
  reserved = MAP_FAILED;
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 3);
  CHECK(shmdt(remapped) == 0);
  remapped = (void*)-1;

  bad = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED);
  CHECK(shmctl(id, IPC_STAT, bad) == -1 && errno == EFAULT);
  CHECK(shmctl(id, IPC_SET, bad) == -1 && errno == EFAULT);
  CHECK(shmctl(id, IPC_STAT, &changed) == 0 && (changed.shm_perm.mode & 0777) == 0600);
  changed.shm_perm.mode = 0640;
  CHECK(shmctl(id, IPC_SET, &changed) == 0);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && (status.shm_perm.mode & 0777) == 0640);
  int highest = shmctl(0, IPC_INFO, (struct shmid_ds*)&limits);
  CHECK(highest >= 0 && limits.shmmax >= length && limits.shmmni > 0);
  int found_index = -1;
  for (int index = 0; index <= highest; ++index) {
    if (shmctl(index, SHM_STAT, &status) == id)
      found_index = index;
  }
  CHECK(found_index >= 0);
  CHECK(shmctl(found_index, SHM_STAT_ANY, &status) == id);
  CHECK(shmctl(0, SHM_INFO, (struct shmid_ds*)&usage) >= 0 && usage.used_ids >= 1 &&
        usage.shm_tot >= 3 && usage.shm_rss >= 3);
  CHECK(shmctl(id, SHM_LOCK, NULL) == 0);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && (status.shm_perm.mode & SHM_LOCKED));
  CHECK(shmctl(id, SHM_UNLOCK, NULL) == 0);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && !(status.shm_perm.mode & SHM_LOCKED));

  if (geteuid() == 0) {
    child = fork();
    CHECK(child >= 0);
    if (!child) {
      alarm(5);
      if (setgid(65534) || setuid(65534))
        _exit(1);
      if (shmat(id, NULL, SHM_RDONLY) != (void*)-1 || errno != EACCES)
        _exit(2);
      if (shmget(key, 0, 0400) != -1 || errno != EACCES)
        _exit(3);
      if (shmctl(id, IPC_STAT, &status) != -1 || errno != EACCES)
        _exit(4);
      if (shmctl(id, IPC_RMID, NULL) != -1 || errno != EPERM)
        _exit(5);
      _exit(shmctl(found_index, SHM_STAT_ANY, &status) == id ? 0 : 6);
    }
    CHECK(reap(child) == 0);
  }

  CHECK(shmctl(id, IPC_RMID, NULL) == 0);
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && (status.shm_perm.mode & SHM_DEST));
  CHECK(shmget(key, 0, 0600) == -1 && errno == ENOENT);
  separate = shmget(key, page, IPC_CREAT | IPC_EXCL | 0600);
  CHECK(separate >= 0 && separate != id);
  remapped = shmat(id, NULL, 0);
  CHECK(remapped != (void*)-1 && !strcmp(remapped, "child-shared"));
  CHECK(shmdt(remapped) == 0);
  remapped = (void*)-1;
  CHECK(shmdt(readonly) == 0);
  readonly = (void*)-1;
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 1);
  // Replacing the last older attachment must not retire a segment while the
  // same shmat call is installing its successor.
  CHECK(shmat(id, view, SHM_REMAP) == view && !strcmp(view, "child-shared"));
  CHECK(shmctl(id, IPC_STAT, &status) == 0 && status.shm_nattch == 1);
  CHECK(shmdt(view) == 0);
  view = (void*)-1;
  CHECK(shmctl(id, IPC_STAT, &status) == -1 && errno == EINVAL);
  id = -1;

  split_id = shmget(IPC_PRIVATE, length, 0600);
  CHECK(split_id >= 0);
  split = shmat(split_id, NULL, 0);
  CHECK(split != (void*)-1);
  CHECK(shmctl(split_id, IPC_RMID, NULL) == 0);
  CHECK(mprotect(split + page, page, PROT_READ) == 0);
  CHECK(shmctl(split_id, IPC_STAT, &status) == 0 && status.shm_nattch == 1);
  child = fork();
  CHECK(child >= 0);
  if (!child) {
    alarm(5);
    _exit(shmctl(split_id, IPC_STAT, &status) == 0 && status.shm_nattch == 2 ? 0 : 1);
  }
  CHECK(reap(child) == 0);
  CHECK(shmctl(split_id, IPC_STAT, &status) == 0 && status.shm_nattch == 1);
  CHECK(munmap(split, page) == 0);
  replacement =
      mmap(split, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0);
  CHECK(replacement == split);
  replacement[0] = 19;
  middle = mmap(split + page, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED,
                -1, 0);
  CHECK(middle == split + page);
  middle[0] = 29;
  CHECK(shmdt(split) == 0);
  split = (void*)-1;
  CHECK(replacement[0] == 19 && middle[0] == 29);
  CHECK(shmctl(split_id, IPC_STAT, &status) == -1 && errno == EINVAL);
  split_id = -1;

  split_id = shmget(IPC_PRIVATE, length, 0600);
  CHECK(split_id >= 0);
  split = shmat(split_id, NULL, 0);
  CHECK(split != (void*)-1);
  split[page] = 31;
  CHECK(munmap(split, page) == 0);
  remapped = shmat(separate, split, 0);
  CHECK(remapped == split);
  remapped[0] = 43;
  CHECK(shmdt(remapped) == 0);
  remapped = (void*)-1;
  CHECK(shmctl(separate, IPC_STAT, &status) == 0 && status.shm_nattch == 0);
  CHECK(shmctl(split_id, IPC_STAT, &status) == 0 && status.shm_nattch == 1);
  CHECK(split[page] == 31);
  CHECK(shmdt(split) == 0);
  split = (void*)-1;
  CHECK(shmctl(split_id, IPC_RMID, NULL) == 0);
  split_id = -1;

out:
  if (view != (void*)-1)
    shmdt(view);
  if (readonly != (void*)-1)
    shmdt(readonly);
  if (remapped != (void*)-1)
    shmdt(remapped);
  if (split != (void*)-1)
    shmdt(split);
  if (id >= 0)
    shmctl(id, IPC_RMID, NULL);
  if (separate >= 0)
    shmctl(separate, IPC_RMID, NULL);
  if (split_id >= 0)
    shmctl(split_id, IPC_RMID, NULL);
  if (reserved != MAP_FAILED)
    munmap(reserved, length);
  if (replacement != MAP_FAILED)
    munmap(replacement, page);
  if (middle != MAP_FAILED)
    munmap(middle, page);
  if (bad != MAP_FAILED)
    munmap(bad, page);
  alarm(0);
  if (!failed)
    puts("ipc-contract-test: shared-memory passed");
  return failed;
}
