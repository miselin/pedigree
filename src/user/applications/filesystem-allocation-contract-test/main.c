#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>

#define CHECK(condition)                                                                      \
  do {                                                                                        \
    if (!(condition)) {                                                                       \
      fprintf(stderr, "FILESYSTEM-ALLOCATION-CONTRACT: line=%d errno=%d\n", __LINE__, errno); \
      goto fail;                                                                              \
    }                                                                                         \
  } while (0)

int main(int argc, char** argv) {
  const char* directory = argc > 1 ? argv[1] : "/tmp";
  char path[512];
  snprintf(path, sizeof(path), "%s/allocation-%ld", directory, (long)getpid());
  int fd = -1, readonly = -1, pathfd = -1, pipes[2] = {-1, -1};
  int result = 1;
  const long page = sysconf(_SC_PAGESIZE);
  const unsigned char prefix[] = {13, 29, 47, 61, 83};
  unsigned char* mapping = MAP_FAILED;
  unsigned char* bytes = NULL;
  struct stat before, reserved, after;
  CHECK(page > 0);
  const off_t offset = 5 * page + 11, length = 2 * page + 17, end = offset + length;
  bytes = malloc((size_t)end);
  CHECK(bytes != NULL);
  fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(fd >= 0);
  CHECK(write(fd, prefix, sizeof(prefix)) == sizeof(prefix));
  CHECK(lseek(fd, 37, SEEK_SET) == 37);
  CHECK(fstat(fd, &before) == 0);

  CHECK(fallocate(fd, FALLOC_FL_KEEP_SIZE, offset, length) == 0);
  CHECK(fstat(fd, &reserved) == 0 && reserved.st_size == sizeof(prefix));
  CHECK(reserved.st_blocks >= before.st_blocks + 2 * page / 512);
  CHECK(lseek(fd, 0, SEEK_CUR) == 37);
  CHECK(close(fd) == 0);
  fd = open(path, O_RDWR);
  CHECK(fd >= 0 && fstat(fd, &after) == 0);
  CHECK(after.st_blocks == reserved.st_blocks && after.st_size == sizeof(prefix));
  CHECK(fallocate(fd, FALLOC_FL_KEEP_SIZE, offset, length) == 0);
  CHECK(fstat(fd, &after) == 0 && after.st_blocks == reserved.st_blocks);

  mapping = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK(mapping != MAP_FAILED);
  /* Dirty bytes beyond EOF must not become file data when allocation extends it. */
  memset(mapping + sizeof(prefix), 0xe7, (size_t)page - sizeof(prefix));
  CHECK(fallocate(fd, 0, offset, length) == 0);
  CHECK(fstat(fd, &after) == 0 && after.st_size == end);
  CHECK(after.st_blocks == reserved.st_blocks);
  CHECK(memcmp(mapping, prefix, sizeof(prefix)) == 0);
  for (size_t i = sizeof(prefix); i < (size_t)page; ++i)
    CHECK(mapping[i] == 0);
  CHECK(munmap(mapping, page) == 0);
  mapping = MAP_FAILED;
  memset(bytes, 0xcc, (size_t)end);
  CHECK(pread(fd, bytes, (size_t)end, 0) == end);
  CHECK(memcmp(bytes, prefix, sizeof(prefix)) == 0);
  for (size_t i = sizeof(prefix); i < (size_t)end; ++i)
    CHECK(bytes[i] == 0);

  errno = EDOM;
  CHECK(posix_fallocate(fd, offset, length) == 0 && errno == EDOM);
  CHECK(fstat(fd, &before) == 0);
  CHECK(pwrite(fd, prefix, sizeof(prefix), offset) == sizeof(prefix));
  CHECK(fstat(fd, &after) == 0 && after.st_blocks == before.st_blocks);
  CHECK(fsync(fd) == 0);
  CHECK(pread(fd, bytes, sizeof(prefix), offset) == sizeof(prefix));
  CHECK(memcmp(bytes, prefix, sizeof(prefix)) == 0);

  readonly = open(path, O_RDONLY);
  pathfd = open(path, O_PATH);
  CHECK(readonly >= 0 && pathfd >= 0);
  errno = 0;
  CHECK(fallocate(readonly, 0, 0, 1) == -1 && errno == EBADF);
  errno = 0;
  CHECK(fallocate(pathfd, 0, 0, 1) == -1 && errno == EBADF);
  errno = 0;
  CHECK(fallocate(-1, 0, 0, 1) == -1 && errno == EBADF);
  errno = 0;
  CHECK(fallocate(fd, 0, -1, 1) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(fallocate(fd, 0, 0, 0) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(fallocate(fd, 0, INT64_MAX, 1) == -1 && errno == EFBIG);
  errno = 0;
  CHECK(fallocate(fd, FALLOC_FL_PUNCH_HOLE | FALLOC_FL_KEEP_SIZE, 0, page) == -1 &&
        errno == EOPNOTSUPP);
  CHECK(pipe(pipes) == 0);
  errno = 0;
  CHECK(fallocate(pipes[1], 0, 0, 1) == -1 && errno == ESPIPE);
  errno = EDOM;
  CHECK(posix_fallocate(fd, 0, -1) == EINVAL && errno == EDOM);

  CHECK(ftruncate(fd, 0) == 0);
  CHECK(fstat(fd, &after) == 0 && after.st_blocks == 0);
  CHECK(fallocate(fd, FALLOC_FL_KEEP_SIZE, offset, length) == 0);
  CHECK(fstat(fd, &after) == 0 && after.st_size == 0 && after.st_blocks > 0);
  CHECK(ftruncate(fd, 0) == 0);
  CHECK(fstat(fd, &after) == 0 && after.st_blocks == 0);
  CHECK(unlink(path) == 0);
  CHECK(fallocate(fd, 0, 0, page) == 0);
  CHECK(fstat(fd, &after) == 0 && after.st_size == page);
  result = 0;

fail:
  if (mapping != MAP_FAILED)
    munmap(mapping, page);
  if (fd >= 0)
    close(fd);
  if (readonly >= 0)
    close(readonly);
  if (pathfd >= 0)
    close(pathfd);
  if (pipes[0] >= 0)
    close(pipes[0]);
  if (pipes[1] >= 0)
    close(pipes[1]);
  unlink(path);
  free(bytes);
  if (!result)
    puts("FILESYSTEM-ALLOCATION-CONTRACT: PASS");
  return result;
}
