#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/wait.h>

#define CHECK(expression)                                                                     \
  do {                                                                                        \
    if (!(expression)) {                                                                      \
      fprintf(stderr, "LARGE-MAPPING-CONTRACT: line=%d %s errno=%d\n", __LINE__, #expression, \
              errno);                                                                         \
      failed = 1;                                                                             \
      goto out;                                                                               \
    }                                                                                         \
  } while (0)

static size_t page_size;
static const size_t gib = (size_t)1 << 30;

static int64_t milliseconds(void) {
  struct timespec now;
  if (clock_gettime(CLOCK_MONOTONIC, &now))
    return -1;
  return (int64_t)now.tv_sec * 1000 + now.tv_nsec / 1000000;
}

static int reap(pid_t child, int timeout) {
  int64_t deadline = milliseconds() + timeout;
  while (milliseconds() < deadline) {
    int status;
    pid_t result = waitpid(child, &status, WNOHANG);
    if (result == child)
      return WIFEXITED(status) ? WEXITSTATUS(status) : 128 + WTERMSIG(status);
    if (result < 0 && errno != EINTR)
      return -1;
    struct timespec delay = {0, 5000000};
    nanosleep(&delay, NULL);
  }
  kill(child, SIGKILL);
  while (waitpid(child, NULL, 0) < 0 && errno == EINTR) {
  }
  return -1;
}

static int release_and_reuse(unsigned char* base, size_t length) {
  if (munmap(base, length))
    return -1;
  if (mmap(base, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) !=
      base)
    return -1;
  return munmap(base, length);
}

static int whole_unmap(void) {
  int failed = 0;
  unsigned char* base = mmap(NULL, gib, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(base != MAP_FAILED);
  CHECK(release_and_reuse(base, gib) == 0);
  base = MAP_FAILED;
out:
  if (base != MAP_FAILED)
    munmap(base, gib);
  return failed;
}

static int sparse_reservation(size_t length) {
  int failed = 0;
  unsigned char* base = mmap(NULL, length, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(base != MAP_FAILED);
  unsigned char* middle = base + length / 2;
  CHECK(mmap(middle, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
             0) == middle);
  CHECK(middle[0] == 0);
  middle[0] = 0x51;
  CHECK(mprotect(middle - 2 * page_size, page_size, PROT_READ | PROT_WRITE) == 0);
  CHECK(mprotect(middle + 2 * page_size, page_size, PROT_READ | PROT_WRITE) == 0);
  CHECK(middle[-2 * (ptrdiff_t)page_size] == 0 && middle[2 * page_size] == 0);
  middle[-2 * (ptrdiff_t)page_size] = 0x52;
  middle[2 * page_size] = 0x53;
#if defined(__x86_64__)
  unsigned char* code = middle + page_size;
  const unsigned char return_42[] = {0xb8, 42, 0, 0, 0, 0xc3};
  CHECK(mprotect(code, page_size, PROT_READ | PROT_WRITE) == 0);
  memcpy(code, return_42, sizeof(return_42));
  __builtin___clear_cache((char*)code, (char*)code + sizeof(return_42));
  CHECK(mprotect(code, page_size, PROT_READ | PROT_EXEC) == 0);
  CHECK(((int (*)(void))code)() == 42);
  CHECK(mprotect(code, page_size, PROT_NONE) == 0);
#endif
  unsigned char* hole = base + length / 4;
  CHECK(munmap(hole, page_size) == 0);
  CHECK(mmap(hole, page_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == hole);
  CHECK(hole[0] == 0);
  hole[0] = 0x54;
  errno = 0;
  CHECK(mmap(middle, page_size, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1,
             0) == MAP_FAILED &&
        errno == EEXIST);
  CHECK(middle[0] == 0x51 && middle[-2 * (ptrdiff_t)page_size] == 0x52 &&
        middle[2 * page_size] == 0x53 && hole[0] == 0x54);
  CHECK(release_and_reuse(base, length) == 0);
  base = MAP_FAILED;
out:
  if (base != MAP_FAILED)
    munmap(base, length);
  return failed;
}

static int reservation(void) {
  return sparse_reservation(gib);
}

static int sparse(void) {
  return sparse_reservation(64 * gib);
}

static int anonymous(void) {
  int failed = 0;
  pid_t child = -1;
  unsigned char* base = mmap(NULL, gib, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(base != MAP_FAILED);
  unsigned char* middle = base + gib / 2;
  base[0] = 0x61;
  base[gib - page_size] = 0x62;
  CHECK(mmap(middle, page_size, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1,
             0) == middle);
  CHECK(base[0] == 0x61 && base[gib - page_size] == 0x62 && middle[0] == 0);
  CHECK(base[2 * page_size] == 0);
  middle[0] = 0x63;
  CHECK(munmap(base + gib / 4, 2 * page_size) == 0);
  CHECK(mmap(base + gib / 4, 2 * page_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == base + gib / 4);
  CHECK(base[gib / 4] == 0 && base[0] == 0x61 && base[gib - page_size] == 0x62);
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(15);
    if (base[0] != 0x61 || base[gib - page_size] != 0x62 || base[2 * page_size] != 0)
      _exit(2);
    base[0] = 0x71;
    base[gib - page_size] = 0x72;
    base[2 * page_size] = 0x73;
    _exit(release_and_reuse(base, gib) ? 3 : 0);
  }
  int child_status = reap(child, 20000);
  child = -1;
  CHECK(child_status == 0);
  CHECK(base[0] == 0x61 && base[gib - page_size] == 0x62 && base[2 * page_size] == 0 &&
        middle[0] == 0x63);
  CHECK(release_and_reuse(base, gib) == 0);
  base = MAP_FAILED;
out:
  if (child > 0) {
    kill(child, SIGKILL);
    reap(child, 2000);
  }
  if (base != MAP_FAILED)
    munmap(base, gib);
  return failed;
}

static int backing_file(int memory, int* readonly) {
  char path[96];
  snprintf(path, sizeof(path), "/tmp/large-mapping-contract-%ld", (long)getpid());
  int fd = memory ? memfd_create("large-mapping-contract", MFD_CLOEXEC)
                  : open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
    return -1;
  unsigned char* bytes = malloc(page_size);
  int error = bytes == NULL;
  if (readonly)
    *readonly = -1;
  for (size_t n = 0; !error && n < 4; ++n) {
    memset(bytes, 0x40 + n, page_size);
    error = write(fd, bytes, page_size) != (ssize_t)page_size;
  }
  free(bytes);
  if (!error && readonly) {
    *readonly = open(path, O_RDONLY);
    error = *readonly < 0;
  }
  if (!memory && unlink(path))
    error = 1;
  if (error) {
    if (readonly && *readonly >= 0) {
      close(*readonly);
      *readonly = -1;
    }
    close(fd);
    return -1;
  }
  return fd;
}

static int file_mapping(int shared) {
  int failed = 0, fd = -1;
  unsigned char *base = MAP_FAILED, *alias = MAP_FAILED;
  CHECK((fd = backing_file(shared, NULL)) >= 0);
  CHECK((base = mmap(NULL, gib, PROT_READ | PROT_WRITE, shared ? MAP_SHARED : MAP_PRIVATE, fd,
                     page_size)) != MAP_FAILED);
  CHECK((alias = mmap(NULL, 3 * page_size, PROT_READ, MAP_SHARED, fd, page_size)) != MAP_FAILED);
  CHECK(base[0] == 0x41 && base[page_size] == 0x42 && base[2 * page_size] == 0x43);
  base[0] = 0x81;
  base[page_size] = 0x82;
  CHECK(alias[0] == (shared ? 0x81 : 0x41) && alias[page_size] == (shared ? 0x82 : 0x42));
  CHECK(close(fd) == 0);
  fd = -1;
  CHECK(mmap(base + page_size, page_size, PROT_READ | PROT_WRITE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == base + page_size);
  CHECK(base[0] == 0x81 && base[page_size] == 0 && base[2 * page_size] == 0x43);
  base[page_size] = 0x83;
  CHECK(munmap(base + 4 * page_size, page_size) == 0);
  CHECK(mmap(base + 4 * page_size, page_size, PROT_NONE,
             MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == base + 4 * page_size);
  CHECK(base[0] == 0x81 && base[2 * page_size] == 0x43 &&
        alias[page_size] == (shared ? 0x82 : 0x42));
  CHECK(release_and_reuse(base, gib) == 0);
  base = MAP_FAILED;
  CHECK(alias[0] == (shared ? 0x81 : 0x41) && alias[page_size] == (shared ? 0x82 : 0x42) &&
        alias[2 * page_size] == 0x43);
  CHECK(munmap(alias, 3 * page_size) == 0);
  alias = MAP_FAILED;
out:
  if (base != MAP_FAILED)
    munmap(base, gib);
  if (alias != MAP_FAILED)
    munmap(alias, 3 * page_size);
  if (fd >= 0)
    close(fd);
  return failed;
}

static int file(void) {
  return file_mapping(0);
}

static int shared(void) {
  return file_mapping(1);
}

static int failure(void) {
  int failed = 0, fd = -1, readonly = -1;
  unsigned char* base = MAP_FAILED;
  CHECK((fd = backing_file(0, &readonly)) >= 0);
  CHECK((base = mmap(NULL, gib, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0)) !=
        MAP_FAILED);
  unsigned char* middle = base + gib / 2;
  base[0] = 0x91;
  middle[0] = 0x92;
  base[gib - page_size] = 0x93;
  errno = 0;
  CHECK(mmap(middle, page_size, PROT_READ, MAP_PRIVATE | MAP_FIXED, -1, 0) == MAP_FAILED &&
        errno == EBADF);
  CHECK(base[0] == 0x91 && middle[0] == 0x92 && base[gib - page_size] == 0x93);
  errno = 0;
  CHECK(mmap(middle, page_size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, readonly, 0) ==
            MAP_FAILED &&
        errno == EACCES);
  CHECK(base[0] == 0x91 && middle[0] == 0x92 && base[gib - page_size] == 0x93);
  CHECK(release_and_reuse(base, gib) == 0);
  base = MAP_FAILED;
out:
  if (base != MAP_FAILED)
    munmap(base, gib);
  if (readonly >= 0)
    close(readonly);
  if (fd >= 0)
    close(fd);
  return failed;
}

static int run(const char* name, int (*test)(void)) {
  printf("LARGE-MAPPING-CONTRACT: BEGIN %s\n", name);
  fflush(stdout);
  pid_t child = fork();
  int result = -1;
  if (!child) {
    alarm(40);
    result = test();
    fflush(stdout);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  if (child > 0)
    result = reap(child, 45000);
  printf("LARGE-MAPPING-CONTRACT: %s %s status=%d\n", result ? "FAIL" : "PASS", name, result);
  fflush(stdout);
  return result;
}

int main(int argc, char** argv) {
  page_size = (size_t)sysconf(_SC_PAGESIZE);
  if (sizeof(size_t) < 8 || page_size < 512 || page_size > 65536 || (page_size & (page_size - 1)))
    return 2;
  static const struct {
    const char* name;
    int (*test)(void);
  } suites[] = {{"whole-unmap", whole_unmap},
                {"reservation", reservation},
                {"sparse", sparse},
                {"anonymous", anonymous},
                {"file", file},
                {"shared", shared},
                {"failure", failure}};
  int selected = 0;
  for (size_t n = 0; n < sizeof(suites) / sizeof(suites[0]); ++n) {
    if (argc > 1 && strcmp(argv[1], suites[n].name))
      continue;
    selected = 1;
    if (run(suites[n].name, suites[n].test))
      return 1;
  }
  if (!selected)
    return 2;
  puts("LARGE-MAPPING-CONTRACT: END PASS");
  return 0;
}
