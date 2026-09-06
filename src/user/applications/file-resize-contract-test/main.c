#define _GNU_SOURCE
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>

size_t fr_page;

static int64_t now(void) {
  struct timespec time;
  return clock_gettime(CLOCK_MONOTONIC, &time) ? -1
                                               : (int64_t)time.tv_sec * 1000000000 + time.tv_nsec;
}
int fr_reap(pid_t child, int milliseconds) {
  int64_t deadline = now() + (int64_t)milliseconds * 1000000;
  while (now() < deadline) {
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
static void expected_bus(int signal) {
  (void)signal;
  _exit(0);
}
int fr_fault(void* address) {
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child) {
    alarm(3);
    struct sigaction action = {.sa_handler = expected_bus};
    sigemptyset(&action.sa_mask);
    if (sigaction(SIGBUS, &action, NULL))
      _exit(2);
    volatile unsigned char value = *(volatile unsigned char*)address;
    (void)value;
    _exit(1);
  }
  int result = fr_reap(child, 4000);
  if (result)
    fprintf(stderr, "FILE-RESIZE-CONTRACT: expected SIGBUS at %p, child status=%d\n", address,
            result);
  return result;
}
int fr_send(int fd, char byte) {
  ssize_t result;
  do {
    result = write(fd, &byte, 1);
  } while (result < 0 && errno == EINTR);
  return result == 1 ? 0 : -1;
}
int fr_receive(int fd, char expected) {
  int64_t deadline = now() + 5000000000;
  while (now() < deadline) {
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
unsigned char fr_pattern(size_t offset) {
  return (unsigned char)(0x31 + offset * 37 + (offset >> 8) * 11);
}
int fr_matches(const volatile unsigned char* bytes, size_t offset, size_t length, int zero) {
  for (size_t n = 0; n < length; ++n) {
    unsigned char expected = zero ? 0 : fr_pattern(offset + n);
    unsigned char actual = bytes[n];
    if (actual != expected) {
      fprintf(stderr, "FILE-RESIZE-CONTRACT: byte offset=%zu got=%u expected=%u\n", offset + n,
              (unsigned)actual, (unsigned)expected);
      return 0;
    }
  }
  return 1;
}
int fr_contents(int fd, size_t offset, size_t length, int zero) {
  unsigned char bytes[512];
  while (length) {
    size_t amount = length < sizeof(bytes) ? length : sizeof(bytes);
    if (pread(fd, bytes, amount, offset) != (ssize_t)amount ||
        !fr_matches(bytes, offset, amount, zero))
      return -1;
    offset += amount;
    length -= amount;
  }
  return 0;
}
int fr_size(int fd, size_t size) {
  struct stat metadata;
  if (fstat(fd, &metadata))
    return -1;
  if (metadata.st_size == (off_t)size)
    return 0;
  fprintf(stderr, "FILE-RESIZE-CONTRACT: size=%lld expected=%zu\n", (long long)metadata.st_size,
          size);
  return -1;
}
int fr_resident(void* address, size_t pages, unsigned bits, const char* stage) {
  unsigned char vector[4] = {0xa5, 0xa5, 0xa5, 0xa5};
  if (pages > sizeof(vector) || mincore(address, pages * fr_page, vector)) {
    fprintf(stderr, "FILE-RESIZE-CONTRACT: %s mincore failed errno=%d\n", stage, errno);
    return -1;
  }
  for (size_t n = 0; n < pages; ++n) {
    if ((vector[n] & 1) != ((bits >> n) & 1)) {
      fprintf(stderr, "FILE-RESIZE-CONTRACT: %s residency=%u,%u,%u,%u pages=%zu expected bits=%x\n",
              stage, vector[0], vector[1], vector[2], vector[3], pages, bits);
      return -1;
    }
  }
  return 0;
}
int fr_create(struct fr_file* file, int backend) {
  static unsigned sequence;
  file->fd = -1;
  file->backend = backend;
  file->path[0] = 0;
  if (backend == FR_MEMFD)
    file->fd = memfd_create("file-resize", MFD_CLOEXEC | MFD_ALLOW_SEALING);
  else {
    snprintf(file->path, sizeof(file->path), "%s/file-resize-%ld-%u",
             backend == FR_RAMFS ? "/tmp" : "", (long)getpid(), ++sequence);
    file->fd = open(file->path, O_CREAT | O_EXCL | O_RDWR | O_CLOEXEC, 0600);
    if (file->fd < 0) {
      fprintf(stderr, "FILE-RESIZE-CONTRACT: create %s failed errno=%d\n", file->path, errno);
      file->path[0] = 0;
    }
  }
  if (file->fd < 0)
    return -1;
  unsigned char* bytes = malloc(fr_page);
  int error = !bytes || ftruncate(file->fd, 4 * fr_page);
  for (size_t offset = 0; !error && offset < 4 * fr_page; offset += fr_page) {
    for (size_t n = 0; n < fr_page; ++n)
      bytes[n] = fr_pattern(offset + n);
    error = pwrite(file->fd, bytes, fr_page, offset) != (ssize_t)fr_page;
  }
  free(bytes);
  if (error) {
    int saved = errno;
    fr_close(file);
    errno = saved;
    return -1;
  }
  return 0;
}
int fr_open_alias(const struct fr_file* file) {
  if (file->backend == FR_EXT2) {
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
void fr_close(struct fr_file* file) {
  if (file->fd >= 0)
    close(file->fd);
  if (file->path[0])
    unlink(file->path);
  file->fd = -1;
  file->path[0] = 0;
}
static int run(const char* name, int backend) {
  printf("FILE-RESIZE-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  pid_t child = fork();
  if (child < 0)
    return -1;
  if (!child) {
    alarm(40);
    int result = backend < 0 ? fr_seals() : fr_shared(backend) || fr_private(backend);
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  int result = fr_reap(child, 45000);
  printf("FILE-RESIZE-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", name, result);
  fflush(stdout);
  return result;
}
int main(int argc, char** argv) {
  fr_page = (size_t)sysconf(_SC_PAGESIZE);
  if (fr_page < 512 || fr_page > 65536 || (fr_page & (fr_page - 1)))
    return 2;
  signal(SIGPIPE, SIG_IGN);
  static const char* names[] = {"memfd", "ramfs", "ext2", "sealed"};
  int selected = 0;
  for (int n = 0; n < 4; ++n) {
    if (argc > 1 && strcmp(argv[1], names[n]))
      continue;
    selected = 1;
    if (run(names[n], n == 3 ? -1 : n))
      return 1;
  }
  if (!selected)
    return 2;
  puts("FILE-RESIZE-CONTRACT: END PASS");
  return 0;
}
