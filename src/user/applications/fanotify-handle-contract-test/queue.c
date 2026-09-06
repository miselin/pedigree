#define _GNU_SOURCE
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/epoll.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/uio.h>

static int copies(void) {
  int failed = 0, group = -1, queued = -1;
  struct fh_file first = {.fd = -1}, second = {.fd = -1};
  struct fh_record record;
  unsigned char bytes[256];
  void* bad = MAP_FAILED;
  CHECK(!fh_create(&first) && !fh_create(&second));
  CHECK(fh_record_size(&first.handle) == fh_record_size(&second.handle));
  size_t length = fh_record_size(&first.handle);
  CHECK(length <= sizeof(bytes));
  group = fh_group(1);
  CHECK(group >= 0 && !fh_mark(group, &first, FAN_MARK_ADD, FAN_MODIFY) &&
        !fh_mark(group, &second, FAN_MARK_ADD, FAN_MODIFY));
  errno = 0;
  CHECK(read(group, bytes, sizeof(bytes)) == -1 && errno == EAGAIN);
  CHECK(!fh_modify(&first));
  CHECK(!ioctl(group, FIONREAD, &queued) && queued == 24);
  errno = 0;
  CHECK(read(group, bytes, length - 1) == -1 && errno == EINVAL);
  CHECK(!ioctl(group, FIONREAD, &queued) && queued == 24);
  CHECK(!fh_event(group, &first, FAN_MODIFY, getpid()));
  bad = mmap(NULL, fh_page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(bad != MAP_FAILED && !fh_modify(&first));
  errno = 0;
  CHECK(read(group, bad, length) == -1 && errno == EFAULT);
  CHECK(!ioctl(group, FIONREAD, &queued) && queued == 0);
  CHECK(!fh_modify(&first));
  struct iovec split[] = {{bytes, 3}, {bytes + 3, length - 3}};
  CHECK(readv(group, split, 2) == (ssize_t)length);
  CHECK(!fh_parse(bytes, length, &record) && fh_equal(&record.handle, &first.handle));
  CHECK(!fh_modify(&first));
  struct iovec partial[] = {{bytes, 3}, {bad, length - 3}};
  errno = 0;
  CHECK(readv(group, partial, 2) == -1 && errno == EFAULT);
  CHECK(!ioctl(group, FIONREAD, &queued) && queued == 0);
  CHECK(!fh_modify(&first) && !fh_modify(&second));
  CHECK(!ioctl(group, FIONREAD, &queued) && queued == 48);
  memset(bytes, 0, sizeof(bytes));
  struct iovec fault[] = {{bytes, length}, {bad, length}};
  errno = 0;
  CHECK(readv(group, fault, 2) == -1 && errno == EFAULT);
  CHECK(!fh_parse(bytes, length, &record) && fh_equal(&record.handle, &first.handle));
  CHECK(!ioctl(group, FIONREAD, &queued) && queued == 0);
  errno = 0;
  CHECK(read(group, bytes, sizeof(bytes)) == -1 && errno == EAGAIN);
out:
  if (bad != MAP_FAILED)
    munmap(bad, fh_page);
  if (group >= 0)
    close(group);
  fh_close(&second);
  fh_close(&first);
  return failed;
}

static int readiness(void) {
  int failed = 0, group = -1, epoll = -1;
  struct fh_file file = {.fd = -1};
  struct epoll_event event = {.events = EPOLLIN | EPOLLET, .data.u64 = 0xf1d}, output;
  CHECK(!fh_create(&file));
  group = fh_group(1);
  epoll = epoll_create1(EPOLL_CLOEXEC);
  CHECK(group >= 0 && epoll >= 0 && !fh_mark(group, &file, FAN_MARK_ADD, FAN_MODIFY));
  CHECK(!epoll_ctl(epoll, EPOLL_CTL_ADD, group, &event));
  CHECK(fh_readable(group, 0) == 0 && epoll_wait(epoll, &output, 1, 0) == 0);
  CHECK(!fh_modify(&file) && fh_readable(group, 0) == 1);
  CHECK(epoll_wait(epoll, &output, 1, 1000) == 1 && (output.events & EPOLLIN) &&
        output.data.u64 == event.data.u64);
  CHECK(epoll_wait(epoll, &output, 1, 0) == 0);
  CHECK(!fh_event(group, &file, FAN_MODIFY, getpid()));
  CHECK(fh_readable(group, 0) == 0);
  CHECK(!fh_modify(&file));
  CHECK(epoll_wait(epoll, &output, 1, 1000) == 1);
  CHECK(!fh_event(group, &file, FAN_MODIFY, getpid()));
  event.events = EPOLLIN;
  CHECK(!epoll_ctl(epoll, EPOLL_CTL_MOD, group, &event));
  CHECK(!fh_modify(&file));
  CHECK(epoll_wait(epoll, &output, 1, 1000) == 1);
  CHECK(epoll_wait(epoll, &output, 1, 0) == 1);
  CHECK(!fh_event(group, &file, FAN_MODIFY, getpid()));
  CHECK(epoll_wait(epoll, &output, 1, 0) == 0);
out:
  if (epoll >= 0)
    close(epoll);
  if (group >= 0)
    close(group);
  fh_close(&file);
  return failed;
}

static int overflow(void) {
  enum { Targets = 32, Producers = 9 };
  int failed = 0, group = -1, queued = -1, overflow_count = 0, ordinary = 0, recovery = -1;
  pid_t child = -1, producers[Producers] = {0};
  struct fh_file files[Targets];
  memset(files, 0, sizeof(files));
  for (int n = 0; n < Targets; ++n)
    files[n].fd = -1;
  group = fh_group(1);
  CHECK(group >= 0);
  for (int n = 0; n < Targets; ++n) {
    CHECK(!fh_create(&files[n]));
    CHECK(!fh_mark(group, &files[n], FAN_MARK_ADD, FAN_OPEN));
  }
  for (int producer = 0; producer < Producers; ++producer) {
    child = fork();
    CHECK(child >= 0);
    if (!child) {
      alarm(10);
      for (int target = 0; target < Targets; ++target) {
        int fd = open(files[target].path, O_RDONLY | O_CLOEXEC);
        if (fd < 0 || close(fd)) {
          fprintf(stderr, "overflow producer=%d target=%d errno=%d\n", producer, target, errno);
          _exit(10);
        }
      }
      _exit(0);
    }
    producers[producer] = child;
    /* Reaping fixes producer order without adding writes to the marked files. */
    int status = fh_reap(child, 12000);
    child = -1;
    if (status)
      fprintf(stderr, "overflow producer=%d pid=%d status=%d\n", producer, producers[producer],
              status);
    CHECK(status == 0);
  }
  CHECK(!ioctl(group, FIONREAD, &queued) && queued == (FH_QUEUE_LIMIT + 1) * 24);
  for (int n = 0; n < FH_QUEUE_LIMIT + 1; ++n) {
    struct fh_record record;
    CHECK(!fh_take(group, &files[0].handle, &record));
    if (record.mask & FAN_Q_OVERFLOW) {
      CHECK(n == FH_QUEUE_LIMIT && record.mask == FAN_Q_OVERFLOW);
      ++overflow_count;
    } else {
      CHECK(ordinary < FH_QUEUE_LIMIT && record.mask == FAN_OPEN);
      CHECK(record.pid == producers[ordinary / Targets]);
      CHECK(fh_equal(&record.handle, &files[ordinary % Targets].handle));
      ++ordinary;
    }
  }
  CHECK(ordinary == FH_QUEUE_LIMIT && overflow_count == 1 && fh_readable(group, 0) == 0);
  recovery = open(files[0].path, O_RDONLY | O_CLOEXEC);
  CHECK(recovery >= 0 && !fh_event(group, &files[0], FAN_OPEN, getpid()));
  CHECK(fh_readable(group, 0) == 0);
out:
  if (child > 0)
    fh_reap(child, 100);
  if (recovery >= 0)
    close(recovery);
  if (group >= 0)
    close(group);
  for (int n = 0; n < Targets; ++n)
    fh_close(&files[n]);
  return failed;
}
int fh_queue(void) {
  return copies() || readiness() || overflow();
}
