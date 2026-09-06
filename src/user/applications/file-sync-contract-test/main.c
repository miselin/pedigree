#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>

#define CHECK(condition)                                                          \
  do {                                                                            \
    if (!(condition)) {                                                           \
      fprintf(stderr, "FILE-SYNC-CONTRACT: line=%d errno=%d\n", __LINE__, errno); \
      goto fail;                                                                  \
    }                                                                             \
  } while (0)

int main(int argc, char** argv) {
  const char* directory = argc > 1 ? argv[1] : "/tmp";
  char path[512];
  snprintf(path, sizeof(path), "%s/file-sync-%ld", directory, (long)getpid());
  const long page = sysconf(_SC_PAGESIZE);
  const size_t length = 3 * (size_t)page;
  int fd = -1, readonly = -1, writeonly = -1, pathfd = -1, pipes[2] = {-1, -1};
  unsigned char* mapping = MAP_FAILED;
  unsigned char resident[3] = {0};
  unsigned char byte = 0;
  int result = 1;

  CHECK(page > 0);
  fd = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(fd >= 0 && ftruncate(fd, length) == 0);
  readonly = open(path, O_RDONLY);
  writeonly = open(path, O_WRONLY);
  pathfd = open(path, O_PATH);
  CHECK(readonly >= 0 && writeonly >= 0 && pathfd >= 0);
  CHECK(lseek(fd, 19, SEEK_SET) == 19 && lseek(readonly, 23, SEEK_SET) == 23);

  mapping = mmap(NULL, length, PROT_READ, MAP_PRIVATE, readonly, 0);
  CHECK(mapping != MAP_FAILED);
  CHECK(readahead(readonly, page + 7, 1) == 0);
  CHECK(mincore(mapping, length, resident) == 0 && (resident[1] & 1));
  errno = EDOM;
  CHECK(posix_fadvise(readonly, 2 * page + 11, 1, POSIX_FADV_WILLNEED) == 0);
  CHECK(errno == EDOM);
  CHECK(mincore(mapping, length, resident) == 0 && (resident[2] & 1));
  CHECK(readahead(readonly, 0, 0) == 0);
  CHECK(mincore(mapping, length, resident) == 0 && (resident[0] & 1));
  CHECK(lseek(readonly, 0, SEEK_CUR) == 23);
  CHECK(readahead(readonly, length + page, 1) == 0);
  CHECK(posix_fadvise(writeonly, 0, 0, POSIX_FADV_WILLNEED) == 0);
  CHECK(munmap(mapping, length) == 0);
  mapping = MAP_FAILED;

  mapping = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK(mapping != MAP_FAILED);
  mapping[0] = 31;
  mapping[page] = 47;
  mapping[2 * page] = 63;
  CHECK(sync_file_range(fd, page - 1, 2, SYNC_FILE_RANGE_WRITE) == 0);
  CHECK(sync_file_range(
            fd, page + 1, 0,
            SYNC_FILE_RANGE_WAIT_BEFORE | SYNC_FILE_RANGE_WRITE | SYNC_FILE_RANGE_WAIT_AFTER) == 0);
  CHECK(sync_file_range(fd, 0, 0, 0) == 0);
  CHECK(sync_file_range(readonly, 0, length, SYNC_FILE_RANGE_WAIT_AFTER) == 0);
  CHECK(fdatasync(fd) == 0 && fdatasync(readonly) == 0);
  CHECK(lseek(fd, 0, SEEK_CUR) == 19 && lseek(readonly, 0, SEEK_CUR) == 23);
  CHECK(pread(readonly, &byte, 1, 0) == 1 && byte == 31);
  CHECK(pread(readonly, &byte, 1, page) == 1 && byte == 47);
  CHECK(pread(readonly, &byte, 1, 2 * page) == 1 && byte == 63);

  errno = 0;
  CHECK(fdatasync(-1) == -1 && errno == EBADF);
  errno = 0;
  CHECK(fdatasync(pathfd) == -1 && errno == EBADF);
  errno = 0;
  CHECK(readahead(writeonly, 0, 1) == -1 && errno == EBADF);
  errno = 0;
  CHECK(readahead(pathfd, 0, 1) == -1 && errno == EBADF);
  errno = 0;
  CHECK(readahead(readonly, -1, 1) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(readahead(readonly, 0, SIZE_MAX) == -1 && errno == EINVAL);
  errno = EDOM;
  CHECK(posix_fadvise(-1, 0, 1, POSIX_FADV_WILLNEED) == EBADF && errno == EDOM);
  CHECK(posix_fadvise(pathfd, 0, 1, POSIX_FADV_WILLNEED) == EBADF);
  CHECK(posix_fadvise(readonly, -1, 1, POSIX_FADV_WILLNEED) == EINVAL);
  CHECK(posix_fadvise(readonly, 0, -1, POSIX_FADV_WILLNEED) == EINVAL);
  CHECK(posix_fadvise(readonly, 0, 1, 99) == EINVAL);
  for (int advice = POSIX_FADV_NORMAL; advice <= POSIX_FADV_NOREUSE; ++advice)
    if (advice != POSIX_FADV_WILLNEED)
      CHECK(posix_fadvise(readonly, 0, 1, advice) == EOPNOTSUPP);
  errno = 0;
  CHECK(sync_file_range(fd, -1, 1, SYNC_FILE_RANGE_WRITE) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(sync_file_range(fd, 0, -1, SYNC_FILE_RANGE_WRITE) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(sync_file_range(fd, LLONG_MAX, 1, SYNC_FILE_RANGE_WRITE) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(sync_file_range(fd, 0, 1, 8) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(sync_file_range(pathfd, 0, 1, 0) == -1 && errno == EBADF);
  CHECK(pipe(pipes) == 0);
  errno = 0;
  CHECK(fdatasync(pipes[0]) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(readahead(pipes[0], 0, 1) == -1 && errno == EINVAL);
  CHECK(posix_fadvise(pipes[0], 0, 1, POSIX_FADV_WILLNEED) == ESPIPE);
  errno = 0;
  CHECK(sync_file_range(pipes[1], 0, 1, SYNC_FILE_RANGE_WRITE) == -1 && errno == ESPIPE);

  puts("FILE-SYNC-CONTRACT: PASS");
  result = 0;
fail:
  if (mapping != MAP_FAILED)
    munmap(mapping, length);
  if (fd >= 0)
    close(fd);
  if (readonly >= 0)
    close(readonly);
  if (writeonly >= 0)
    close(writeonly);
  if (pathfd >= 0)
    close(pathfd);
  if (pipes[0] >= 0)
    close(pipes[0]);
  if (pipes[1] >= 0)
    close(pipes[1]);
  if (fd >= 0)
    unlink(path);
  return result;
}
