/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <sys/xattr.h>

#define CHECK(expression)                                                                \
  do {                                                                                   \
    if (!(expression)) {                                                                 \
      fprintf(stderr, "GLOBAL-SYNC-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      return 1;                                                                          \
    }                                                                                    \
  } while (0)

static const char closedContents[] = "closed file and namespace metadata";

static unsigned char pattern(size_t offset) {
  return (unsigned char)((offset * 17 + offset / 4096 * 31 + 43) & 255);
}

static int basic(void) {
  int root = open("/", O_RDONLY | O_DIRECTORY);
  int path = open("/", O_PATH);
  int ram = open("/tmp", O_RDONLY | O_DIRECTORY);
  CHECK(root >= 0 && path >= 0 && ram >= 0);
  errno = 0;
  CHECK(syncfs(-1) == -1 && errno == EBADF);
  errno = 0;
  CHECK(syncfs(path) == -1 && errno == EBADF);
  CHECK(syncfs(root) == 0 && syncfs(ram) == 0);
  errno = EDOM;
  sync();
  CHECK(errno == EDOM);

  pid_t child = fork();
  if (!child) {
    if (setuid(65534) || syncfs(root))
      _exit(2);
    sync();
    _exit(0);
  }
  int status = 0;
  CHECK(child > 0 && waitpid(child, &status, 0) == child && WIFEXITED(status) &&
        WEXITSTATUS(status) == 0);
  close(root);
  close(path);
  close(ram);
  puts("GLOBAL-SYNC-CONTRACT: BASIC PASS");
  return 0;
}

static int cold(const char* mode, int verify) {
  char directory[128], source[160], alias[160], nested[160], final[192], closed[160], removed[160];
  snprintf(directory, sizeof(directory), "/global-sync-contract-%s", mode);
  snprintf(source, sizeof(source), "%s/source", directory);
  snprintf(alias, sizeof(alias), "%s/alias", directory);
  snprintf(nested, sizeof(nested), "%s/nested", directory);
  snprintf(final, sizeof(final), "%s/final", nested);
  snprintf(closed, sizeof(closed), "%s/closed", directory);
  snprintf(removed, sizeof(removed), "%s/removed", directory);
  const size_t bytes = 4 * 4096;
  unsigned char attribute[256];
  for (size_t i = 0; i < sizeof(attribute); ++i)
    attribute[i] = pattern(i + 71);

  if (verify) {
    struct stat first, second;
    CHECK(stat(final, &first) == 0 && stat(alias, &second) == 0);
    CHECK(first.st_ino == second.st_ino && first.st_nlink == 2 && first.st_size == (off_t)bytes);
    CHECK(access(source, F_OK) == -1 && errno == ENOENT);
    CHECK(access(removed, F_OK) == -1 && errno == ENOENT);
    unsigned char copiedAttribute[256];
    CHECK(getxattr(alias, "user.global-sync", copiedAttribute, sizeof(copiedAttribute)) ==
          sizeof(copiedAttribute));
    CHECK(!memcmp(attribute, copiedAttribute, sizeof(attribute)));
    int fd = open(final, O_RDONLY);
    CHECK(fd >= 0);
    unsigned char buffer[4096];
    for (size_t offset = 0; offset < bytes; offset += sizeof(buffer)) {
      CHECK(read(fd, buffer, sizeof(buffer)) == sizeof(buffer));
      for (size_t i = 0; i < sizeof(buffer); ++i)
        CHECK(buffer[i] == pattern(offset + i));
    }
    CHECK(close(fd) == 0);
    fd = open(closed, O_RDONLY);
    CHECK(fd >= 0 && read(fd, buffer, sizeof(buffer)) == sizeof(closedContents));
    CHECK(!memcmp(buffer, closedContents, sizeof(closedContents)));
    CHECK(close(fd) == 0);
    printf("GLOBAL-SYNC-CONTRACT: COLD-VERIFIED %s\n", mode);
    return 0;
  }

  CHECK(mkdir(directory, 0700) == 0 && mkdir(nested, 0700) == 0);
  int fd = open(source, O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(fd >= 0 && ftruncate(fd, bytes) == 0);
  CHECK(link(source, alias) == 0 && rename(source, final) == 0);
  CHECK(setxattr(final, "user.global-sync", attribute, sizeof(attribute), 0) == 0);
  unsigned char* mapping = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK(mapping != MAP_FAILED && close(fd) == 0);
  for (size_t i = 0; i < bytes; ++i)
    mapping[i] = pattern(i);

  fd = open(closed, O_CREAT | O_EXCL | O_WRONLY, 0600);
  CHECK(fd >= 0 && write(fd, closedContents, sizeof(closedContents)) == sizeof(closedContents));
  CHECK(close(fd) == 0);
  fd = open(removed, O_CREAT | O_EXCL | O_WRONLY, 0600);
  CHECK(fd >= 0 && write(fd, "discard", 7) == 7 && close(fd) == 0 && unlink(removed) == 0);

  // The only supplied descriptor names a directory. The dirty file has no
  // numeric descriptor; keep its mapping alive across the external VM stop.
  int root = open("/", O_RDONLY | O_DIRECTORY);
  CHECK(root >= 0);
  if (!strcmp(mode, "syncfs"))
    CHECK(syncfs(root) == 0);
  else
    sync();
  alarm(0);
  printf("GLOBAL-SYNC-CONTRACT: COLD-READY %s\n", mode);
  for (;;)
    pause();
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(90);
  if (argc == 1 || (argc == 2 && !strcmp(argv[1], "basic")))
    return basic();
  CHECK(argc == 3 && (!strcmp(argv[1], "prepare") || !strcmp(argv[1], "verify")) &&
        (!strcmp(argv[2], "sync") || !strcmp(argv[2], "syncfs")));
  return cold(argv[2], !strcmp(argv[1], "verify"));
}
