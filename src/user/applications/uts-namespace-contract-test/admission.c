#define _GNU_SOURCE
#include <fcntl.h>
#include <sched.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/fsuid.h>
#include <sys/mman.h>

static int privileged_failures(void) {
  int failed = 0, saved = -1, regular = -1;
  struct ns_identity original, current;
  CHECK(ns_set("admission", "admission-domain") == 0);
  saved = open("/proc/thread-self/ns/uts", O_RDONLY | O_CLOEXEC);
  CHECK(saved >= 0 && ns_fd_identity(saved, &original) == 0);
  regular = memfd_create("namespace-admission", MFD_CLOEXEC);
  CHECK(regular >= 0);
  errno = 0;
  CHECK(unshare(1) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(unshare(CLONE_NEWNS) == -1 && errno == EOPNOTSUPP);
  errno = 0;
  CHECK(unshare(CLONE_NEWUTS | CLONE_NEWNS) == -1 && errno == EOPNOTSUPP);
  errno = 0;
  CHECK(setns(-1, CLONE_NEWUTS) == -1 && errno == EBADF);
  errno = 0;
  CHECK(setns(regular, 0) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(setns(saved, CLONE_NEWNS) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(setns(saved, CLONE_NEWUTS | CLONE_NEWNS) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(setns(saved, 1) == -1 && errno == EINVAL);
  CHECK(ns_path_identity("/proc/thread-self/ns/uts", &current) == 0 && ns_same(original, current));
  CHECK(ns_expect("admission", "admission-domain") == 0);
  CHECK(setns(saved, 0) == 0 && setns(saved, CLONE_NEWUTS) == 0);
  char byte = '?';
  CHECK(read(saved, &byte, 1) == -1);
  CHECK(write(saved, &byte, 1) == -1);
  void* invalid = mmap(NULL, ns_page, PROT_READ, MAP_SHARED, saved, 0);
  if (invalid != MAP_FAILED)
    munmap(invalid, ns_page);
  CHECK(invalid == MAP_FAILED);
  CHECK(ns_expect("admission", "admission-domain") == 0);
out:
  if (saved >= 0)
    close(saved);
  if (regular >= 0)
    close(regular);
  return failed;
}

static int unprivileged(int command, int report, void* argument) {
  (void)command;
  (void)report;
  int failed = 0, saved = *(int*)argument;
  struct ns_identity before, after;
  CHECK(ns_fd_identity(saved, &before) == 0);
  CHECK(setresuid(0, 1001, 0) == 0);
  setfsuid(0);
  CHECK(geteuid() == 1001 && setfsuid((uid_t)-1) == 0);
  CHECK(unshare(0) == 0);
  errno = 0;
  CHECK(unshare(CLONE_NEWUTS) == -1 && errno == EPERM);
  errno = 0;
  CHECK(setns(saved, 0) == -1 && errno == EPERM);
  errno = 0;
  CHECK(sethostname("denied", 6) == -1 && errno == EPERM);
  errno = 0;
  CHECK(setdomainname("denied", 6) == -1 && errno == EPERM);
  errno = 0;
  CHECK(sethostname(NULL, 65) == -1 && errno == EPERM);
  errno = 0;
  CHECK(setdomainname(NULL, 0) == -1 && errno == EPERM);
  errno = 0;
  CHECK(setns(-1, CLONE_NEWUTS) == -1 && errno == EBADF);
  errno = 0;
  CHECK(setns(saved, CLONE_NEWNS) == -1 && errno == EINVAL);
  CHECK(ns_path_identity("/proc/thread-self/ns/uts", &after) == 0 && ns_same(before, after));
  CHECK(ns_expect("admission", "admission-domain") == 0);
out:
  return failed;
}

static int administrative_credentials(void) {
  int failed = 0, saved = -1;
  struct ns_peer child = NS_PEER_INITIALIZER;
  saved = open("/proc/thread-self/ns/uts", O_RDONLY | O_CLOEXEC);
  CHECK(saved >= 0);
  CHECK(ns_spawn(&child, unprivileged, &saved) == 0 && ns_join(&child) == 0);
  CHECK(ns_expect("admission", "admission-domain") == 0);
  setfsuid(1001);
  setfsgid(1002);
  CHECK(geteuid() == 0 && setfsuid((uid_t)-1) == 1001);
  CHECK(unshare(CLONE_NEWUTS) == 0 && ns_set("admin-fs-override", "admin-fs-override") == 0);
  CHECK(setns(saved, 0) == 0 && ns_expect("admission", "admission-domain") == 0);
out:
  setfsuid(0);
  setfsgid(0);
  ns_cleanup(&child);
  if (saved >= 0)
    close(saved);
  return failed;
}

static int namespace_capacity(void) {
  int failed = 0, original = -1, held[300], count = 0, exhausted = 0;
  struct ns_identity before, after;
  original = open("/proc/thread-self/ns/uts", O_RDONLY | O_CLOEXEC);
  CHECK(original >= 0);
  while (count < (int)(sizeof(held) / sizeof(held[0]))) {
    int fd = open("/proc/thread-self/ns/uts", O_RDONLY | O_CLOEXEC);
    CHECK(fd >= 0);
    held[count++] = fd;
    CHECK(ns_fd_identity(fd, &before) == 0);
    errno = 0;
    if (unshare(CLONE_NEWUTS)) {
      CHECK(errno == ENOSPC);
      CHECK(ns_path_identity("/proc/thread-self/ns/uts", &after) == 0 && ns_same(before, after));
      exhausted = 1;
      break;
    }
  }
  CHECK(exhausted && count > 2);
  /* This namespace has no membership or alias left once its descriptor closes. */
  CHECK(close(held[1]) == 0);
  held[1] = -1;
  CHECK(unshare(CLONE_NEWUTS) == 0);
  CHECK(ns_path_identity("/proc/thread-self/ns/uts", &after) == 0 && !ns_same(before, after));
out:
  if (original >= 0) {
    if (setns(original, 0))
      failed = 1;
    close(original);
  }
  for (int i = 0; i < count; ++i)
    if (held[i] >= 0)
      close(held[i]);
  return failed;
}

int ns_admission(void) {
  return privileged_failures() || administrative_credentials() || namespace_capacity();
}
