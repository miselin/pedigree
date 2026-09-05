/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include <sys/stat.h>

extern void fail(void) __attribute__((noreturn));

static void require(int condition, const char* operation) {
  if (!condition) {
    printf("FS-PERSISTENCE-CONTRACT: FAIL %s errno=%d\n", operation, errno);
    fail();
  }
}

static int open_directory(const char* path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY);
  require(fd >= 0, "open directory");
  return fd;
}

static void require_absent(const char* path) {
  struct stat status;
  errno = 0;
  require(lstat(path, &status) == -1 && errno == ENOENT, "retired name remains absent");
}

void test_fs_persistence_contracts(const char* base, int write_phase) {
  unsigned char contents[32768 + 113];
  for (size_t i = 0; i < sizeof(contents); ++i) {
    contents[i] = (unsigned char)(i * 29 + 7);
  }
  if (write_phase) {
    require(mkdir(base, 0700) == 0, "create isolated persistence directory");
  }
  require(chdir(base) == 0, "enter persistence directory");
  int directory = open_directory(".");
  if (write_phase) {
    require(mkdir("left", 0700) == 0 && mkdir("right", 0700) == 0, "create rename parents");
    int left = open_directory("left");
    int right = open_directory("right");
    int file = open("left/staged", O_CREAT | O_EXCL | O_RDWR, 0600);
    require(file >= 0 && write(file, contents, sizeof(contents)) == sizeof(contents),
            "write staged file");
    require(link("left/staged", "left/alias") == 0, "create temporary hardlink");
    require(fsync(file) == 0, "flush staged file");
    require(close(file) == 0, "close staged file");
    file = open("right/result", O_CREAT | O_EXCL | O_RDWR, 0600);
    require(file >= 0 && write(file, "old", 3) == 3 && fsync(file) == 0 && close(file) == 0,
            "persist replacement target");
    require(mkdir("left/moved", 0700) == 0 && mkdir("left/removed", 0700) == 0,
            "create directory mutations");
    require(rename("left/staged", "right/result") == 0 && unlink("left/alias") == 0 &&
                rename("left/moved", "right/moved") == 0 && rmdir("left/removed") == 0,
            "commit namespace mutations");
    // Parent sync must include the namespace metadata changed by rename and
    // unlink, including the moved child's on-disk parent entry.
    require(fsync(left) == 0 && fsync(right) == 0 && fsync(directory) == 0,
            "flush mutated directories");
    require(close(left) == 0 && close(right) == 0, "close rename parents");
    file = open("complete", O_CREAT | O_EXCL | O_WRONLY, 0600);
    require(file >= 0 && write(file, "ready", 5) == 5 && fsync(file) == 0 && close(file) == 0 &&
                fsync(directory) == 0,
            "persist completion marker");
    int parent = open_directory("..");
    require(fsync(parent) == 0 && close(parent) == 0, "flush containing directory");
  } else {
    unsigned char observed[sizeof(contents)];
    int file = open("right/result", O_RDONLY);
    struct stat status;
    require(file >= 0 && fstat(file, &status) == 0 && status.st_size == sizeof(contents) &&
                status.st_nlink == 1 && status.st_blocks > 0,
            "reboot preserves file metadata");
    require(read(file, observed, sizeof(observed)) == sizeof(observed) &&
                !memcmp(contents, observed, sizeof(contents)),
            "reboot preserves every saved byte");
    require(close(file) == 0, "close verified file");
    require_absent("left/staged");
    require_absent("left/alias");
    require_absent("left/moved");
    require_absent("left/removed");
    require(stat("right", &status) == 0, "read new parent inode");
    DIR* moved = opendir("right/moved");
    require(moved != NULL, "open moved directory");
    int found_parent = 0;
    struct dirent* entry;
    while ((entry = readdir(moved))) {
      if (!strcmp(entry->d_name, "..")) {
        require(entry->d_ino == status.st_ino, "reboot preserves moved parent entry");
        found_parent = 1;
      }
    }
    require(found_parent && closedir(moved) == 0, "read moved parent entry");
    file = open("complete", O_RDONLY);
    char marker[5];
    require(file >= 0 && read(file, marker, sizeof(marker)) == sizeof(marker) &&
                !memcmp(marker, "ready", sizeof(marker)) && close(file) == 0,
            "reboot preserves completion marker");
  }
  require(close(directory) == 0, "close persistence directory");
  printf("FS-PERSISTENCE-CONTRACT: PASS %s\n", write_phase ? "write" : "read");
  fflush(stdout);
}
