#define _POSIX_C_SOURCE 200809L
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/stat.h>

static const char* directory = "/pedigree-dir-sync";

static void die(const char* phase) {
  fprintf(stderr, "IOBENCH FAIL phase=%s errno=%d error=%s\n", phase, errno, strerror(errno));
  exit(1);
}

static uint64_t now_ns(void) {
  struct timespec t;
  if (clock_gettime(CLOCK_MONOTONIC, &t))
    die("clock");
  return (uint64_t)t.tv_sec * 1000000000ULL + (uint64_t)t.tv_nsec;
}

static void result(const char* phase, uint64_t start, int status, int error) {
  uint64_t elapsed = now_ns() - start;
  printf("IOBENCH metric phase=%s elapsed_us=%llu bytes=0\n", phase,
         (unsigned long long)(elapsed / 1000));
  printf("IOBENCH syscall phase=%s status=%d errno=%d\n", phase, status, status ? error : 0);
  if (status) {
    errno = error;
    die(phase);
  }
}

static void sync_fd(const char* phase, int fd) {
  uint64_t start = now_ns();
  int status = fsync(fd);
  int error = errno;
  result(phase, start, status, error);
}

static void full_write(int fd, const char* text, size_t length) {
  while (length) {
    ssize_t written = write(fd, text, length);
    if (written < 0 && errno == EINTR)
      continue;
    if (written <= 0) {
      if (!written)
        errno = EIO;
      die("file-write");
    }
    text += written;
    length -= (size_t)written;
  }
}

static void paths(unsigned index, char pending[1024], char committed[1024]) {
  if (snprintf(pending, 1024, "%s/pending-%u", directory, index) >= 1024 ||
      snprintf(committed, 1024, "%s/committed-%u", directory, index) >= 1024) {
    errno = ENAMETOOLONG;
    die("directory-path");
  }
}

static void verify(unsigned count) {
  for (unsigned i = 0; i < count; ++i) {
    char pending[1024], committed[1024], expected[80], actual[80];
    paths(i, pending, committed);
    struct stat st;
    errno = 0;
    if (!lstat(pending, &st) || errno != ENOENT) {
      errno = EIO;
      die("renamed-source-absent");
    }
    size_t length =
        (size_t)snprintf(expected, sizeof(expected), "pedigree directory sync generation %u\n", i);
    int fd = open(committed, O_RDONLY | O_NOFOLLOW);
    if (fd < 0)
      die("verify-open");
    if (fstat(fd, &st))
      die("verify-stat");
    if (!S_ISREG(st.st_mode) || st.st_size != (off_t)length) {
      errno = EIO;
      die("verify-type-length");
    }
    size_t received = 0;
    while (received < length) {
      ssize_t n = read(fd, actual + received, length - received);
      if (n < 0 && errno == EINTR)
        continue;
      if (n <= 0) {
        if (!n)
          errno = EIO;
        die("verify-read");
      }
      received += (size_t)n;
    }
    if (memcmp(actual, expected, length)) {
      errno = EIO;
      die("verify-content");
    }
    if (close(fd))
      die("verify-close");
    printf("IOBENCH verified path=%s bytes=%zu source_absent=1\n", committed, length);
  }
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  setvbuf(stderr, NULL, _IONBF, 0);
  unsigned count = 1;
  int count_set = 0, verify_existing = 0;
  for (int i = 1; i < argc; ++i) {
    if (!strcmp(argv[i], "verify-existing")) {
      verify_existing = 1;
    } else {
      char* end;
      unsigned long value = strtoul(argv[i], &end, 10);
      if (!*argv[i] || *end || value < 1 || value > 8 || count_set) {
        fprintf(stderr, "usage: %s [1..8] [verify-existing]\n", argv[0]);
        return 2;
      }
      count = (unsigned)value;
      count_set = 1;
    }
  }
  const char* override = getenv("IOBENCH_DIRECTORY");
  if (override && *override)
    directory = override;
  size_t length = strlen(directory);
  if (directory[0] != '/' || length < 2 || length >= 1000 || directory[length - 1] == '/') {
    fprintf(stderr, "IOBENCH_DIRECTORY must be an absolute path without a trailing slash\n");
    return 2;
  }
  printf("IOBENCH BEGIN mode=directory-sync%s files=%u path=%s\n",
         verify_existing ? "-verify-existing" : "", count, directory);
  if (!verify_existing) {
    char parent[1024];
    memcpy(parent, directory, length + 1);
    char* slash = strrchr(parent, '/');
    slash[slash == parent ? 1 : 0] = '\0';
    int parent_fd = open(parent, O_RDONLY | O_DIRECTORY);
    if (parent_fd < 0)
      die("parent-open");
    if (mkdir(directory, 0700))
      die("directory-create");
    /* Persist the new directory's own name before testing entries inside it. */
    sync_fd("directory_parent_fsync", parent_fd);
    if (close(parent_fd))
      die("parent-close");
  }
  int directory_fd = open(directory, O_RDONLY | O_DIRECTORY | O_NOFOLLOW);
  if (directory_fd < 0)
    die("directory-open");
  if (!verify_existing) {
    for (unsigned i = 0; i < count; ++i) {
      char pending[1024], committed[1024], payload[80], phase[64];
      paths(i, pending, committed);
      size_t bytes =
          (size_t)snprintf(payload, sizeof(payload), "pedigree directory sync generation %u\n", i);
      int fd = open(pending, O_WRONLY | O_CREAT | O_EXCL, 0600);
      if (fd < 0)
        die("file-create");
      full_write(fd, payload, bytes);
      snprintf(phase, sizeof(phase), "directory_file_fsync_%u", i);
      sync_fd(phase, fd);
      if (close(fd))
        die("file-close");
      uint64_t start = now_ns();
      int status = rename(pending, committed);
      int error = errno;
      snprintf(phase, sizeof(phase), "directory_rename_%u", i);
      result(phase, start, status, error);
      snprintf(phase, sizeof(phase), "directory_fsync_%u", i);
      sync_fd(phase, directory_fd);
    }
    for (unsigned i = 0; i < 3; ++i) {
      char phase[64];
      snprintf(phase, sizeof(phase), "directory_clean_fsync_%u", i);
      sync_fd(phase, directory_fd);
    }
  }
  verify(count);
  if (close(directory_fd))
    die("directory-close");
  printf("IOBENCH retained path=%s files=%u\n", directory, count);
  printf("IOBENCH PASS END\n");
  return 0;
}
