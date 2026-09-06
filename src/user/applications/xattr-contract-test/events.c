#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/xattr.h>

static int same_time(struct timespec first, struct timespec second) {
  return first.tv_sec == second.tv_sec && first.tv_nsec == second.tv_nsec;
}

static int next_second(time_t previous) {
  int64_t start = xa_now();
  if (start < 0)
    return -1;
  do {
    struct timespec now;
    if (clock_gettime(CLOCK_REALTIME, &now))
      return -1;
    if (now.tv_sec > previous)
      return 0;
    const struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
  } while (xa_now() - start < 3000000000);
  errno = ETIMEDOUT;
  return -1;
}

static int event(int notify, int watch, int directory, int expected) {
  if (notify < 0)
    return 0;
  struct inotify_event received;
  errno = 0;
  ssize_t length = read(notify, &received, sizeof(received));
  if (!expected)
    return length == -1 && errno == EAGAIN ? 0 : -1;
  if (length != sizeof(received) || received.wd != watch || received.len || received.cookie ||
      received.mask != (uint32_t)(IN_ATTRIB | (directory ? IN_ISDIR : 0)))
    return -1;
  errno = 0;
  return read(notify, &received, sizeof(received)) == -1 && errno == EAGAIN ? 0 : -1;
}

static int metadata_case(int backend, int directory) {
  int failed = 0, notify = -1, watch = -1;
  struct xa_file file = {.fd = -1};
  void* inaccessible = MAP_FAILED;
  struct stat before, after;
  static const unsigned char first[] = {0, 1, 0xff, 7};
  static const unsigned char second[] = {9, 0, 2, 0xfe};
  CHECK(!xa_create(&file, backend, directory));
  inaccessible = mmap(NULL, xa_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(inaccessible != MAP_FAILED);
  if (file.path[0]) {
    notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC);
    CHECK(notify >= 0);
    watch = inotify_add_watch(notify, file.path, IN_ATTRIB | IN_MODIFY | IN_ACCESS);
    CHECK(watch >= 0);
  }
  CHECK(!fsetxattr(file.fd, "user.event", first, sizeof(first), XATTR_CREATE));
  CHECK(!event(notify, watch, directory, 1));
  CHECK(!fstat(file.fd, &before));
  // These filesystems expose second-resolution inode timestamps.
  CHECK(!next_second(before.st_ctim.tv_sec));
  CHECK(!fsetxattr(file.fd, "user.event", second, sizeof(second), XATTR_REPLACE));
  CHECK(!event(notify, watch, directory, 1));
  CHECK(!fstat(file.fd, &after));
  CHECK(after.st_ctim.tv_sec > before.st_ctim.tv_sec);
  CHECK(same_time(before.st_mtim, after.st_mtim) && same_time(before.st_atim, after.st_atim));
  before = after;
  errno = 0;
  CHECK(fsetxattr(file.fd, "user.event", first, sizeof(first), XATTR_CREATE) == -1 &&
        errno == EEXIST);
  errno = 0;
  CHECK(fsetxattr(file.fd, "user.event", inaccessible, 1, 0) == -1 && errno == EFAULT);
  errno = 0;
  CHECK(fremovexattr(file.fd, "user.absent") == -1 && errno == ENODATA);
  CHECK(!xa_value(&file, XA_FD, "user.event", second, sizeof(second)));
  const char* names[] = {"user.event"};
  CHECK(!xa_names(&file, XA_FD, names, 1));
  CHECK(!fstat(file.fd, &after));
  CHECK(same_time(before.st_ctim, after.st_ctim) && same_time(before.st_mtim, after.st_mtim) &&
        same_time(before.st_atim, after.st_atim));
  CHECK(!event(notify, watch, directory, 0));
  CHECK(!next_second(before.st_ctim.tv_sec));
  CHECK(!fremovexattr(file.fd, "user.event"));
  CHECK(!event(notify, watch, directory, 1));
  CHECK(!fstat(file.fd, &after));
  CHECK(after.st_ctim.tv_sec > before.st_ctim.tv_sec);
  CHECK(same_time(before.st_mtim, after.st_mtim) && same_time(before.st_atim, after.st_atim));
out:
  if (notify >= 0)
    close(notify);
  if (inaccessible != MAP_FAILED)
    munmap(inaccessible, xa_page);
  xa_close(&file);
  return failed;
}

int xa_events(void) {
  for (int backend = XA_MEMFD; backend <= XA_EXT2; ++backend) {
    if (metadata_case(backend, 0) || (backend != XA_MEMFD && metadata_case(backend, 1)))
      return 1;
  }
  return 0;
}
