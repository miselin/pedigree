#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>
#include <sys/syscall.h>

#define CHECK(condition)                                                             \
  do {                                                                               \
    if (!(condition)) {                                                              \
      fprintf(stderr, "RENAME-FLAGS-CONTRACT: line=%d errno=%d\n", __LINE__, errno); \
      goto fail;                                                                     \
    }                                                                                \
  } while (0)

struct race {
  int directory;
  const char* source;
  pthread_barrier_t* barrier;
  int result, error;
};

static void* rename_racer(void* opaque) {
  struct race* race = opaque;
  pthread_barrier_wait(race->barrier);
  race->result =
      renameat2(race->directory, race->source, race->directory, "winner", RENAME_NOREPLACE);
  race->error = errno;
  return NULL;
}

static int create_byte(int directory, const char* name, unsigned char value) {
  int fd = openat(directory, name, O_CREAT | O_EXCL | O_RDWR, 0600);
  if (fd < 0)
    return -1;
  int success = write(fd, &value, 1) == 1;
  close(fd);
  return success ? 0 : -1;
}

int main(int argc, char** argv) {
  const char* parent = argc > 1 ? argv[1] : "/tmp";
  char path[512], absolute[640];
  snprintf(path, sizeof(path), "%s/rename-flags-%ld", parent, (long)getpid());
  int directory = -1, retained = -1, result = 1;
  struct stat source, target, status;
  unsigned char byte = 0;
  CHECK(mkdir(path, 0700) == 0);
  directory = open(path, O_RDONLY | O_DIRECTORY);
  CHECK(directory >= 0);
  CHECK(create_byte(directory, "source", 31) == 0 && create_byte(directory, "target", 47) == 0);
  CHECK(fstatat(directory, "source", &source, 0) == 0);
  CHECK(fstatat(directory, "target", &target, 0) == 0);
  retained = openat(directory, "source", O_RDONLY);
  CHECK(retained >= 0);
  errno = 0;
  CHECK(renameat2(directory, "source", directory, "target", RENAME_NOREPLACE) == -1 &&
        errno == EEXIST);
  CHECK(fstatat(directory, "source", &status, 0) == 0 && status.st_ino == source.st_ino);
  CHECK(fstatat(directory, "target", &status, 0) == 0 && status.st_ino == target.st_ino);
  errno = 0;
  CHECK(renameat2(directory, "source", directory, "source", RENAME_NOREPLACE) == -1 &&
        errno == EEXIST);
  if (argc > 2 && !strcmp(argv[2], "--hard-links")) {
    CHECK(linkat(directory, "source", directory, "alias", 0) == 0);
    errno = 0;
    CHECK(renameat2(directory, "source", directory, "alias", RENAME_NOREPLACE) == -1 &&
          errno == EEXIST);
    CHECK(unlinkat(directory, "alias", 0) == 0);
  }

  CHECK(renameat2(directory, "source", directory, "moved", RENAME_NOREPLACE) == 0);
  CHECK(fstatat(directory, "moved", &status, 0) == 0 && status.st_ino == source.st_ino);
  CHECK(pread(retained, &byte, 1, 0) == 1 && byte == 31);
  errno = 0;
  CHECK(fstatat(directory, "source", &status, 0) == -1 && errno == ENOENT);
  CHECK(syscall(SYS_renameat2, directory, "moved", directory, "target", 0) == 0);
  CHECK(fstatat(directory, "target", &status, 0) == 0 && status.st_ino == source.st_ino);
  snprintf(absolute, sizeof(absolute), "%s/absolute", path);
  CHECK(renameat2(directory, "target", -1, absolute, RENAME_NOREPLACE) == 0);
  CHECK(mkdirat(directory, "nested", 0700) == 0);
  CHECK(renameat2(directory, "absolute", directory, "nested/file", RENAME_NOREPLACE) == 0);

  errno = 0;
  CHECK(renameat2(directory, "nested/file", directory, "other", RENAME_EXCHANGE) == -1 &&
        errno == EOPNOTSUPP);
  errno = 0;
  CHECK(renameat2(directory, "nested/file", directory, "other", RENAME_WHITEOUT) == -1 &&
        errno == EOPNOTSUPP);
  errno = 0;
  CHECK(renameat2(directory, "nested/file", directory, "other",
                  RENAME_EXCHANGE | RENAME_NOREPLACE) == -1 &&
        errno == EINVAL);
  errno = 0;
  CHECK(renameat2(directory, "nested/file", directory, "other", 8) == -1 && errno == EINVAL);
  errno = 0;
  CHECK(renameat2(-1, "nested/file", directory, "other", RENAME_NOREPLACE) == -1 && errno == EBADF);
  errno = 0;
  CHECK(renameat2(directory, "", directory, "other", RENAME_NOREPLACE) == -1 && errno == ENOENT);

  for (unsigned iteration = 0; iteration < 16; ++iteration) {
    CHECK(create_byte(directory, "first", 61) == 0 && create_byte(directory, "second", 83) == 0);
    pthread_barrier_t barrier;
    pthread_t thread;
    CHECK(pthread_barrier_init(&barrier, NULL, 2) == 0);
    struct race races[2] = {{directory, "first", &barrier, -2, 0},
                            {directory, "second", &barrier, -2, 0}};
    const int created = pthread_create(&thread, NULL, rename_racer, &races[0]);
    if (created) {
      pthread_barrier_destroy(&barrier);
      errno = created;
      CHECK(0);
    }
    rename_racer(&races[1]);
    CHECK(pthread_join(thread, NULL) == 0);
    CHECK(pthread_barrier_destroy(&barrier) == 0);
    CHECK((races[0].result == 0 && races[1].result == -1 && races[1].error == EEXIST) ||
          (races[1].result == 0 && races[0].result == -1 && races[0].error == EEXIST));
    CHECK(unlinkat(directory, "winner", 0) == 0);
    CHECK(unlinkat(directory, races[0].result ? "first" : "second", 0) == 0);
  }
  result = 0;

fail:
  if (retained >= 0)
    close(retained);
  if (directory >= 0) {
    const char* names[] = {"source", "target", "moved",  "absolute", "nested/file",
                           "other",  "first",  "second", "winner",   "alias"};
    for (unsigned i = 0; i < sizeof(names) / sizeof(names[0]); ++i)
      unlinkat(directory, names[i], 0);
    unlinkat(directory, "nested", AT_REMOVEDIR);
    close(directory);
  }
  rmdir(path);
  if (!result)
    puts("RENAME-FLAGS-CONTRACT: PASS");
  return result;
}
