#define _GNU_SOURCE
#include <fcntl.h>
#include <grp.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/wait.h>

static int64_t now(void) {
  struct timespec time;
  return clock_gettime(CLOCK_MONOTONIC, &time) ? -1
                                               : (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}

int rp_reap(pid_t child, int milliseconds) {
  int64_t begin = now();
  int64_t deadline = begin + (int64_t)milliseconds * 1000000;
  while (begin >= 0 && (begin = now()) >= 0 && begin < deadline) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec pause = {0, 5000000};
    nanosleep(&pause, NULL);
  }
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}

int rp_send(int fd, char byte) {
  ssize_t result;
  do {
    result = write(fd, &byte, 1);
  } while (result < 0 && errno == EINTR);
  return result == 1 ? 0 : -1;
}

int rp_receive(int fd, char expected) {
  int64_t begin = now(), deadline = begin + 5000000000;
  while (begin >= 0 && (begin = now()) >= 0 && begin < deadline) {
    struct pollfd watch = {.fd = fd, .events = POLLIN};
    int result = poll(&watch, 1, 100);
    if (result < 0 && errno == EINTR)
      continue;
    if (result < 0)
      return -1;
    if (!result)
      continue;
    char byte;
    ssize_t amount = read(fd, &byte, 1);
    if (amount < 0 && errno == EINTR)
      continue;
    if (amount == 1 && byte == expected)
      return 0;
    errno = EIO;
    return -1;
  }
  errno = ETIMEDOUT;
  return -1;
}

static void expected_fault(int signal) {
  (void)signal;
  _exit(0);
}

int rp_fault(void* address, int expected_signal, int write_access) {
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child) {
    alarm(3);
    struct sigaction action = {.sa_handler = expected_fault};
    sigemptyset(&action.sa_mask);
    if (sigaction(expected_signal, &action, NULL))
      _exit(2);
    volatile unsigned char* byte = address;
    if (write_access)
      *byte = 0xb9;
    else {
      volatile unsigned char value = *byte;
      (void)value;
    }
    _exit(1);
  }
  int result = rp_reap(child, 4000);
  if (result)
    fprintf(stderr, "REMAP-FILE-PAGES-CONTRACT: expected signal=%d address=%p write=%d status=%d\n",
            expected_signal, address, write_access, result);
  return result;
}

unsigned char rp_pattern(size_t offset) {
  return (unsigned char)(0x31 + (offset / rp_page) * 23 + (offset % rp_page) * 37 +
                         ((offset % rp_page) >> 8) * 11);
}

int rp_matches(const volatile unsigned char* bytes, size_t offset, size_t length, int zero) {
  for (size_t n = 0; n < length; ++n) {
    unsigned char expected = zero ? 0 : rp_pattern(offset + n);
    unsigned char actual = bytes[n];
    if (actual != expected) {
      fprintf(stderr, "REMAP-FILE-PAGES-CONTRACT: offset=%zu got=%u expected=%u\n", offset + n,
              (unsigned)actual, (unsigned)expected);
      return 0;
    }
  }
  return 1;
}

int rp_contents(int fd, size_t offset, size_t length, int zero) {
  unsigned char bytes[512];
  while (length) {
    size_t amount = length < sizeof(bytes) ? length : sizeof(bytes);
    if (pread(fd, bytes, amount, offset) != (ssize_t)amount ||
        !rp_matches(bytes, offset, amount, zero))
      return -1;
    offset += amount;
    length -= amount;
  }
  return 0;
}

int rp_size(int fd, size_t size) {
  struct stat metadata;
  if (fstat(fd, &metadata))
    return -1;
  if (metadata.st_size == (off_t)size)
    return 0;
  fprintf(stderr, "REMAP-FILE-PAGES-CONTRACT: size=%lld expected=%zu\n",
          (long long)metadata.st_size, size);
  return -1;
}

int rp_resident(void* address, size_t pages, unsigned bits, const char* stage) {
  unsigned char vector[8];
  memset(vector, 0xa5, sizeof(vector));
  if (pages > sizeof(vector) || mincore(address, pages * rp_page, vector)) {
    fprintf(stderr, "REMAP-FILE-PAGES-CONTRACT: %s mincore errno=%d\n", stage, errno);
    return -1;
  }
  for (size_t n = 0; n < pages; ++n) {
    if ((vector[n] & 1) != ((bits >> n) & 1)) {
      fprintf(stderr, "REMAP-FILE-PAGES-CONTRACT: %s page=%zu resident=%u expected=%u\n", stage, n,
              vector[n], (bits >> n) & 1);
      return -1;
    }
  }
  return 0;
}

int rp_create(struct rp_file* file, int backend) {
  static unsigned sequence;
  file->fd = -1;
  file->backend = backend;
  file->path[0] = 0;
  if (backend == RP_MEMFD)
    file->fd = memfd_create("remap-file-pages", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  else {
    snprintf(file->path, sizeof(file->path), "%s/remap-pages-%ld-%u",
             backend == RP_RAMFS ? "/tmp" : "", (long)getpid(), ++sequence);
    file->fd = open(file->path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (file->fd < 0) {
      fprintf(stderr, "REMAP-FILE-PAGES-CONTRACT: create %s errno=%d\n", file->path, errno);
      file->path[0] = 0;
    }
  }
  if (file->fd < 0)
    return -1;
  unsigned char* bytes = malloc(rp_page);
  int error = !bytes || ftruncate(file->fd, 8 * rp_page);
  for (size_t offset = 0; !error && offset < 8 * rp_page; offset += rp_page) {
    for (size_t n = 0; n < rp_page; ++n)
      bytes[n] = rp_pattern(offset + n);
    error = pwrite(file->fd, bytes, rp_page, offset) != (ssize_t)rp_page;
  }
  free(bytes);
  if (error) {
    int saved = errno;
    rp_close(file);
    errno = saved;
    return -1;
  }
  return 0;
}

int rp_open_alias(const struct rp_file* file) {
  if (file->backend == RP_EXT2) {
    char alias[160];
    snprintf(alias, sizeof(alias), "%s.alias", file->path);
    if (link(file->path, alias))
      return -1;
    int fd = open(alias, O_RDWR | O_CLOEXEC);
    int saved = errno;
    if (unlink(alias)) {
      saved = errno;
      if (fd >= 0)
        close(fd);
      fd = -1;
    }
    errno = saved;
    return fd;
  }
  return file->path[0] ? open(file->path, O_RDWR | O_CLOEXEC) : dup(file->fd);
}

void rp_close(struct rp_file* file) {
  if (file->fd >= 0)
    close(file->fd);
  if (file->path[0])
    unlink(file->path);
  file->fd = -1;
  file->path[0] = 0;
}

int rp_limit(size_t bytes) {
  struct rlimit limit;
  if (getrlimit(RLIMIT_MEMLOCK, &limit))
    return -1;
  limit.rlim_cur = bytes;
  return setrlimit(RLIMIT_MEMLOCK, &limit);
}

int rp_unprivileged(void) {
  if (!geteuid() && (setgroups(0, NULL) || setgid(65534) || setuid(65534)))
    return -1;
  if (geteuid())
    return 0;
  errno = EPERM;
  return -1;
}
