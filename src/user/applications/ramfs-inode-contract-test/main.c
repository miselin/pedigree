/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

#define WORKERS 4
#define FILES_PER_WORKER 32
#define CHECK(expression)                                                                \
  do {                                                                                   \
    if (!(expression)) {                                                                 \
      fprintf(stderr, "RAMFS-INODE-CONTRACT: FAIL line=%d errno=%d\n", __LINE__, errno); \
      failed = 1;                                                                        \
      goto out;                                                                          \
    }                                                                                    \
  } while (0)

static int same_inode(const struct stat* a, const struct stat* b) {
  return a->st_dev == b->st_dev && a->st_ino == b->st_ino;
}

static int inspect_node(const char* directory, const char* name, mode_t kind,
                        const struct stat* target, struct stat* result) {
  char path[PATH_MAX];
  int length = snprintf(path, sizeof(path), "%s/%s", directory, name);
  if (length < 0 || (size_t)length >= sizeof(path)) {
    errno = ENAMETOOLONG;
    return 0;
  }
  struct stat descriptor, followed;
  if (lstat(path, result) || stat(path, &followed))
    return 0;
  int fd = open(path, O_PATH | O_NOFOLLOW);
  if (fd < 0)
    return 0;
  int ok = fstat(fd, &descriptor) == 0;
  if (close(fd))
    ok = 0;
  return ok && result->st_ino != 0 && (result->st_mode & S_IFMT) == kind &&
         same_inode(result, &descriptor) && same_inode(target ? target : result, &followed);
}

struct worker {
  int directory;
  pthread_mutex_t* gate;
  struct stat files[FILES_PER_WORKER];
  int error;
};

static void* create_files(void* argument) {
  struct worker* worker = argument;
  worker->error = pthread_mutex_lock(worker->gate);
  if (worker->error)
    return NULL;
  worker->error = pthread_mutex_unlock(worker->gate);
  if (worker->error)
    return NULL;
  for (int i = 0; i < FILES_PER_WORKER; ++i) {
    char name[24];
    snprintf(name, sizeof(name), "parallel-%d", i);
    int fd = openat(worker->directory, name, O_CREAT | O_EXCL | O_RDWR, 0600);
    if (fd < 0) {
      worker->error = errno;
      return NULL;
    }
    struct stat by_name;
    int ok =
        fstat(fd, &worker->files[i]) == 0 && fstatat(worker->directory, name, &by_name, 0) == 0;
    int saved_error = errno;
    if (close(fd)) {
      ok = 0;
      saved_error = errno;
    }
    if (!ok || !worker->files[i].st_ino || !same_inode(&worker->files[i], &by_name)) {
      worker->error = ok ? EINVAL : saved_error;
      return NULL;
    }
  }
  return NULL;
}

int main(int argc, char** argv) {
  const char* base = argc > 1 ? argv[1] : "/tmp";
  char directory[PATH_MAX];
  int length = snprintf(directory, sizeof(directory), "%s/ramfs-inode-contract.XXXXXX", base);
  int failed = 0, created = 0, root = -1, retained = -1, reopened = -1, replacement = -1;
  int gate_locked = 0, started = 0, joined = 0;
  pthread_mutex_t gate = PTHREAD_MUTEX_INITIALIZER;
  pthread_t threads[WORKERS];
  struct worker workers[WORKERS];
  memset(workers, 0, sizeof(workers));
  for (int i = 0; i < WORKERS; ++i)
    workers[i].directory = -1;
  const char* names[] = {"first", "second", "dir-0", "dir-1", "dir-2", "dir-3", "link-0", "link-1"};
  struct stat nodes[11], observed;
  CHECK(argc <= 2 && length >= 0 && (size_t)length < sizeof(directory));
  CHECK(mkdtemp(directory) != NULL);
  created = 1;
  root = open(directory, O_RDONLY | O_DIRECTORY);
  CHECK(root >= 0 && stat(base, &nodes[0]) == 0 && fstat(root, &nodes[1]) == 0);
  printf("RAMFS-INODE-CONTRACT: base dev=%ju ino=%ju directory dev=%ju ino=%ju\n",
         (uintmax_t)nodes[0].st_dev, (uintmax_t)nodes[0].st_ino, (uintmax_t)nodes[1].st_dev,
         (uintmax_t)nodes[1].st_ino);
  CHECK(nodes[0].st_ino != 0 && nodes[1].st_ino != 0 && !same_inode(&nodes[0], &nodes[1]));
  retained = openat(root, "first", O_CREAT | O_EXCL | O_RDWR, 0600);
  reopened = openat(root, "second", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(retained >= 0 && reopened >= 0);
  CHECK(write(retained, "old", 3) == 3 && close(reopened) == 0);
  reopened = -1;
  for (int i = 0; i < WORKERS; ++i)
    CHECK(mkdirat(root, names[2 + i], 0700) == 0);
  CHECK(symlinkat("first", root, "link-0") == 0 && symlinkat("second", root, "link-1") == 0);
  for (int i = 0; i < 8; ++i) {
    mode_t kind = i < 2 ? S_IFREG : i < 6 ? S_IFDIR : S_IFLNK;
    CHECK(
        inspect_node(directory, names[i], kind, i >= 6 ? &nodes[2 + i - 6] : NULL, &nodes[2 + i]));
  }
  for (int i = 1; i < 10; ++i) {
    CHECK(nodes[i].st_dev == nodes[0].st_dev);
    for (int j = 0; j < i; ++j)
      CHECK(!same_inode(&nodes[i], &nodes[j]));
  }
  puts("RAMFS-INODE-CONTRACT: PASS distinct-kinds");

  reopened = openat(root, "first", O_RDONLY);
  CHECK(reopened >= 0 && fstat(reopened, &observed) == 0 && same_inode(&nodes[2], &observed));
  CHECK(close(reopened) == 0);
  reopened = -1;
  CHECK(renameat(root, "first", root, "renamed") == 0);
  CHECK(inspect_node(directory, "renamed", S_IFREG, NULL, &observed) &&
        same_inode(&nodes[2], &observed));
  reopened = openat(root, "renamed", O_RDONLY);
  CHECK(reopened >= 0 && fstat(reopened, &observed) == 0 && same_inode(&nodes[2], &observed));
  CHECK(close(reopened) == 0 && fstat(retained, &observed) == 0 &&
        same_inode(&nodes[2], &observed));
  reopened = -1;
  CHECK(renameat(root, "dir-3", root, "dir-renamed") == 0);
  CHECK(inspect_node(directory, "dir-renamed", S_IFDIR, NULL, &observed) &&
        same_inode(&nodes[7], &observed));
  CHECK(renameat(root, "dir-renamed", root, "dir-3") == 0);
  CHECK(renameat(root, "link-1", root, "link-renamed") == 0);
  CHECK(inspect_node(directory, "link-renamed", S_IFLNK, &nodes[3], &observed) &&
        same_inode(&nodes[9], &observed));
  CHECK(renameat(root, "link-renamed", root, "link-1") == 0);
  CHECK(unlinkat(root, "renamed", 0) == 0);
  replacement = openat(root, "renamed", O_CREAT | O_EXCL | O_RDWR, 0600);
  CHECK(replacement >= 0 && fstat(replacement, &observed) == 0 && observed.st_ino != 0);
  nodes[10] = observed;
  CHECK(nodes[10].st_dev == nodes[2].st_dev);
  for (int i = 0; i < 10; ++i)
    CHECK(!same_inode(&nodes[10], &nodes[i]));
  CHECK(write(replacement, "new", 3) == 3 && fstat(retained, &observed) == 0 &&
        same_inode(&nodes[2], &observed));
  char contents[3];
  CHECK(pread(retained, contents, sizeof(contents), 0) == sizeof(contents) &&
        memcmp(contents, "old", sizeof(contents)) == 0);
  CHECK(pread(replacement, contents, sizeof(contents), 0) == sizeof(contents) &&
        memcmp(contents, "new", sizeof(contents)) == 0);
  puts("RAMFS-INODE-CONTRACT: PASS stable-and-unlinked");

  CHECK(pthread_mutex_lock(&gate) == 0);
  gate_locked = 1;
  for (int i = 0; i < WORKERS; ++i) {
    workers[i].directory = openat(root, names[2 + i], O_RDONLY | O_DIRECTORY);
    workers[i].gate = &gate;
    CHECK(workers[i].directory >= 0);
    int error = pthread_create(&threads[i], NULL, create_files, &workers[i]);
    if (error)
      errno = error;
    CHECK(error == 0);
    ++started;
  }
  CHECK(pthread_mutex_unlock(&gate) == 0);
  gate_locked = 0;
  for (; joined < started; ++joined) {
    int error = pthread_join(threads[joined], NULL);
    if (error) {
      fprintf(stderr, "RAMFS-INODE-CONTRACT: FAIL join=%d\n", error);
      _Exit(1);
    }
  }
  for (int i = 0; i < WORKERS; ++i) {
    if (workers[i].error)
      errno = workers[i].error;
    CHECK(workers[i].error == 0);
    for (int j = 0; j < FILES_PER_WORKER; ++j) {
      struct stat* current = &workers[i].files[j];
      CHECK(current->st_ino != 0 && current->st_dev == nodes[0].st_dev);
      for (int k = 0; k < 11; ++k)
        CHECK(!same_inode(current, &nodes[k]));
      for (int k = 0; k <= i; ++k)
        for (int n = 0; n < (k == i ? j : FILES_PER_WORKER); ++n)
          CHECK(!same_inode(current, &workers[k].files[n]));
    }
  }
  puts("RAMFS-INODE-CONTRACT: PASS concurrent-create");

out:
  if (gate_locked)
    pthread_mutex_unlock(&gate);
  for (; joined < started; ++joined) {
    int error = pthread_join(threads[joined], NULL);
    if (error) {
      fprintf(stderr, "RAMFS-INODE-CONTRACT: FAIL cleanup-join=%d\n", error);
      _Exit(1);
    }
  }
  pthread_mutex_destroy(&gate);
  if (retained >= 0)
    close(retained);
  if (reopened >= 0)
    close(reopened);
  if (replacement >= 0)
    close(replacement);
  for (int i = 0; i < WORKERS; ++i) {
    if (workers[i].directory >= 0) {
      for (int j = 0; j < FILES_PER_WORKER; ++j) {
        char name[24];
        snprintf(name, sizeof(name), "parallel-%d", j);
        unlinkat(workers[i].directory, name, 0);
      }
      close(workers[i].directory);
    }
  }
  if (root >= 0) {
    unlinkat(root, "first", 0);
    unlinkat(root, "second", 0);
    unlinkat(root, "renamed", 0);
    unlinkat(root, "link-0", 0);
    unlinkat(root, "link-1", 0);
    unlinkat(root, "link-renamed", 0);
    unlinkat(root, "dir-renamed", AT_REMOVEDIR);
    for (int i = 0; i < WORKERS; ++i)
      unlinkat(root, names[2 + i], AT_REMOVEDIR);
    close(root);
  }
  if (created && rmdir(directory)) {
    fprintf(stderr, "RAMFS-INODE-CONTRACT: FAIL cleanup errno=%d\n", errno);
    failed = 1;
  }
  printf("RAMFS-INODE-CONTRACT: END %s base=%s\n", failed ? "FAIL" : "PASS", base);
  return failed ? 1 : 0;
}
