#define _GNU_SOURCE
#include <poll.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/inotify.h>
#include <sys/mman.h>
#include <sys/stat.h>

static int copying(void) {
  int failed = 0, p[2] = {-1, -1}, file = -1;
  const size_t page = sysconf(_SC_PAGESIZE);
  char* source = MAP_FAILED;
  char output[8] = {0};
  CHECK(!pipe(p));
  source = mmap(NULL, 2 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(source != MAP_FAILED);
  memcpy(source, "abcdefgh", 8);
  struct iovec vectors[] = {{source, 3}, {source + 3, 5}};
  CHECK(vmsplice(p[1], vectors, 2, SPLICE_F_MORE | SPLICE_F_MOVE) == 8);
  memset(source, 'z', 8);
  CHECK(!munmap(source, 2 * page));
  source = MAP_FAILED;
  struct iovec destinations[] = {{output, 3}, {output + 3, 5}};
  CHECK(vmsplice(p[0], destinations, 2, SPLICE_F_GIFT) == 8 && !memcmp(output, "abcdefgh", 8));
  CHECK(vmsplice(p[0], destinations, 2, SPLICE_F_NONBLOCK) == -1 && errno == EAGAIN);
  CHECK(vmsplice(p[1], NULL, 0, 0) == 0);
  CHECK((file = pt_file(0, 0)) >= 0);
  CHECK(vmsplice(file, NULL, 0, 0) == 0);
  CHECK(vmsplice(file, destinations, 2, 0) == -1 && errno == EBADF);
  CHECK(vmsplice(-1, NULL, 0, 0) == -1 && errno == EBADF);
  CHECK(!close(p[1]));
  p[1] = -1;
  CHECK(vmsplice(p[0], destinations, 2, 0) == 0);
out:
  if (file >= 0)
    close(file);
  if (source != MAP_FAILED)
    munmap(source, 2 * page);
  if (p[0] >= 0)
    close(p[0]);
  if (p[1] >= 0)
    close(p[1]);
  return failed;
}
static int faults_and_limits(void) {
  int failed = 0, p[2] = {-1, -1};
  const size_t page = sysconf(_SC_PAGESIZE);
  void* denied = MAP_FAILED;
  char first[3] = {0}, tail[5] = {0}, large[PT_CAPACITY + 7];
  CHECK(!pipe(p));
  denied = mmap(NULL, page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(denied != MAP_FAILED);
  struct iovec bad = {denied, 8};
  CHECK(vmsplice(p[1], &bad, 1, 0) == -1 && errno == EFAULT);
  CHECK(vmsplice(p[1], denied, 1, 0) == -1 && errno == EFAULT);
  CHECK(vmsplice(p[1], denied, 1025, 0) == -1 && errno == EINVAL);
  struct iovec overflow = {first, SIZE_MAX};
  CHECK(vmsplice(p[1], &overflow, 1, 0) == -1 && errno == EINVAL);
  struct iovec incoming[] = {{"abc", 3}, {denied, 5}};
  CHECK(vmsplice(p[1], incoming, 2, 0) == 3);
  CHECK(read(p[0], first, 3) == 3 && !memcmp(first, "abc", 3));
  CHECK(write(p[1], "abcdefgh", 8) == 8);
  CHECK(vmsplice(p[0], &bad, 1, 0) == -1 && errno == EFAULT);
  struct iovec outgoing[] = {{first, 3}, {denied, 5}};
  CHECK(vmsplice(p[0], outgoing, 2, 0) == 3 && !memcmp(first, "abc", 3));
  struct iovec retry = {tail, 5};
  CHECK(vmsplice(p[0], &retry, 1, 0) == 5 && !memcmp(tail, "defgh", 5));
  struct iovec empty_first[] = {{denied, 0}, {"abc", 3}};
  CHECK(vmsplice(p[1], empty_first, 2, 0) == 3);
  CHECK(read(p[0], first, 3) == 3 && !memcmp(first, "abc", 3));
  for (size_t n = 0; n < sizeof(large); ++n)
    large[n] = pt_pattern(n, 61);
  struct iovec bounded = {large, sizeof(large)};
  ssize_t amount = vmsplice(p[1], &bounded, 1, 0);
  CHECK(amount > 0 && amount <= PT_CAPACITY && !pt_read(p[0], amount, 0, 61));
  CHECK(vmsplice(p[1], &retry, 1, 0x80000000U) == -1 && errno == EINVAL);
  CHECK(vmsplice(p[1], &retry, 1, SPLICE_F_GIFT) == -1 && errno == EINVAL);
out:
  if (denied != MAP_FAILED)
    munmap(denied, page);
  if (p[0] >= 0)
    close(p[0]);
  if (p[1] >= 0)
    close(p[1]);
  return failed;
}
static int aligned_gift(void) {
  int failed = 0, p[2] = {-1, -1};
  const size_t page = sysconf(_SC_PAGESIZE);
  unsigned char* memory = MAP_FAILED;
  CHECK(!pipe(p) && page == PT_CAPACITY);
  memory = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(memory != MAP_FAILED);
  for (size_t n = 0; n < page; ++n)
    memory[n] = pt_pattern(n, 79);
  struct iovec vector = {memory, page};
  CHECK(vmsplice(p[1], &vector, 1, SPLICE_F_GIFT) == (ssize_t)page);
  /* Gift permits reuse by the kernel; this implementation still leaves the mapping accessible. */
  CHECK(memory[0] == pt_pattern(0, 79) && memory[page - 1] == pt_pattern(page - 1, 79));
  CHECK(!pt_read(p[0], page, 0, 79));
out:
  if (memory != MAP_FAILED)
    munmap(memory, page);
  if (p[0] >= 0)
    close(p[0]);
  if (p[1] >= 0)
    close(p[1]);
  return failed;
}
static int duplex_fifo(void) {
  int failed = 0, fd = -1, notify = -1, created = 0;
  char path[128], bytes[3];
  struct stat metadata;
  snprintf(path, sizeof(path), "/tmp/pipe-transfer-duplex-%ld", (long)getpid());
  mode_t previous_mask = umask(0077);
  int create_result = mkfifo(path, 0666);
  int create_errno = errno;
  umask(previous_mask);
  errno = create_errno;
  CHECK(!create_result);
  created = 1;
  CHECK((fd = open(path, O_RDWR | O_NONBLOCK)) >= 0);
  CHECK(!fstat(fd, &metadata) && S_ISFIFO(metadata.st_mode) && (metadata.st_mode & 07777) == 0600 &&
        metadata.st_uid == geteuid() && metadata.st_gid == getegid());
  CHECK(mkfifo(path, 0666) == -1 && errno == EEXIST);
  CHECK((notify = inotify_init1(IN_NONBLOCK | IN_CLOEXEC)) >= 0);
  int watch = inotify_add_watch(notify, path, IN_MODIFY);
  CHECK(watch >= 0);
  struct iovec vector = {"abc", 3};
  CHECK(vmsplice(fd, &vector, 1, SPLICE_F_NONBLOCK) == 3);
  union {
    struct inotify_event event;
    char bytes[256];
  } event;
  CHECK(pt_ready(notify, POLLIN, 1000) == 1 &&
        read(notify, event.bytes, sizeof(event.bytes)) >= (ssize_t)sizeof(event.event) &&
        event.event.wd == watch && (event.event.mask & IN_MODIFY));
  CHECK(read(fd, bytes, sizeof(bytes)) == sizeof(bytes) && !memcmp(bytes, "abc", sizeof(bytes)));
out:
  if (notify >= 0)
    close(notify);
  if (fd >= 0)
    close(fd);
  if (created)
    unlink(path);
  return failed;
}
int pipe_transfer_vmsplice(void) {
  return copying() || faults_and_limits() || aligned_gift() || duplex_fifo();
}
