/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <utime.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/time.h>
#include <sys/uio.h>

extern void fail(void) __attribute__((noreturn));

static const char* filesystem;

static void require(int condition, const char* operation) {
  if (!condition) {
    printf("FS-MUTATION-CONTRACT: FAIL %s %s errno=%d\n", filesystem, operation, errno);
    fail();
  }
}

static int create_file(const char* name, const char* contents) {
  int file = open(name, O_CREAT | O_EXCL | O_RDWR, 0600);
  require(file >= 0, "create file");
  const size_t length = strlen(contents);
  require(write(file, contents, length) == (ssize_t)length, "initial file contents");
  return file;
}

static void require_contents(int file, const char* expected, const char* operation) {
  char buffer[64] = {0};
  const size_t length = strlen(expected);
  require(length < sizeof(buffer) && pread(file, buffer, sizeof(buffer), 0) == (ssize_t)length &&
              !memcmp(buffer, expected, length),
          operation);
}

static void replacement_contracts(int hardlinks) {
  int source = create_file("source", "source-data");
  int replaced = create_file("destination", "old-data");
  struct stat original;
  struct stat current;
  require(fstat(source, &original) == 0, "source inode");
  require(rename("source", "destination") == 0, "replace destination");
  require(stat("destination", &current) == 0 && current.st_ino == original.st_ino,
          "rename preserves inode");
  errno = 0;
  require(stat("source", &current) == -1 && errno == ENOENT, "old name removed");
  require_contents(source, "source-data", "open source after rename");
  require_contents(replaced, "old-data", "open replaced file survives");
  require(pwrite(replaced, "OLD", 3, 0) == 3, "write replaced open file");
  require_contents(replaced, "OLD-data", "replaced inode remains writable");
  int destination = open("destination", O_RDONLY);
  require(destination >= 0, "open renamed destination");
  require_contents(destination, "source-data", "destination isolation");
  require(rename("destination", "destination") == 0, "same-name rename");

  if (hardlinks) {
    require(link("destination", "alias") == 0, "create hardlink");
    require(rename("destination", "alias") == 0, "same-inode rename");
    require(stat("destination", &current) == 0 && current.st_ino == original.st_ino,
            "same-inode rename keeps source name");
    require(stat("alias", &current) == 0 && current.st_ino == original.st_ino,
            "same-inode rename keeps destination name");
    int alias = open("alias", O_RDWR);
    require(alias >= 0, "open hardlink alias");
    require(fstat(source, &current) == 0 && current.st_nlink == 2,
            "hardlinks report inode link count");
    require(fchmod(alias, 0640) == 0 && fstat(source, &current) == 0 &&
                (current.st_mode & 0777) == 0640,
            "hardlinks share permission changes");
    require(ftruncate(alias, 6) == 0, "truncate hardlink alias");
    require(fstat(source, &current) == 0 && current.st_size == 6, "hardlinks share size");
    require_contents(source, "source", "hardlinks share truncated contents");
    require(pwrite(alias, "S", 1, 0) == 1, "write hardlink alias");
    require_contents(source, "Source", "hardlinks share writes");
    require(unlink("alias") == 0, "unlink alias with open descriptor");
    require(fstat(alias, &current) == 0 && current.st_nlink == 1,
            "open alias observes decremented link count");
    require_contents(alias, "Source", "unlinked alias retains shared inode");
    close(alias);
  }

  close(destination);
  close(source);
  close(replaced);
  require(unlink("destination") == 0, "remove renamed file");
  printf("FS-MUTATION-CONTRACT: PASS %s replacement-and-open-inodes\n", filesystem);
}

static void hardlink_mapping_contracts(void) {
  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  int first = create_file("mapped-first", "");
  require(ftruncate(first, 2 * page_size) == 0 && link("mapped-first", "mapped-second") == 0,
          "create mapped hardlink layout");
  int second = open("mapped-second", O_RDWR);
  require(second >= 0, "open mapped alias");
  unsigned char* left = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE, MAP_SHARED, first, 0);
  unsigned char* right = mmap(NULL, 2 * page_size, PROT_READ | PROT_WRITE, MAP_SHARED, second, 0);
  require(left != MAP_FAILED && right != MAP_FAILED, "map hardlink aliases");
  require(left[17] == 0 && right[17] == 0, "fault initial alias pages");
  left[17] = 0x6b;
  require(right[17] == 0x6b, "mapped aliases share writes immediately");
  const unsigned char value = 0x9e;
  require(pwrite(second, &value, 1, 29) == 1 && left[29] == value && right[29] == value,
          "descriptor writes update both mapped aliases");
  require(msync(left, 2 * page_size, MS_SYNC) == 0, "sync shared alias changes");
  require(unlink("mapped-first") == 0 && close(first) == 0 && munmap(left, 2 * page_size) == 0,
          "retire first mapped alias");
  struct stat status;
  require(fstat(second, &status) == 0 && status.st_nlink == 1, "surviving mapped alias link count");
  right[31] = 0x4a;
  require(fsync(second) == 0 && munmap(right, 2 * page_size) == 0 && close(second) == 0,
          "fsync surviving alias after first alias retirement");
  second = open("mapped-second", O_RDONLY);
  unsigned char buffer[32] = {0};
  require(second >= 0 && pread(second, buffer, sizeof(buffer), 0) == sizeof(buffer) &&
              buffer[17] == 0x6b && buffer[29] == 0x9e && buffer[31] == 0x4a,
          "reopen retains all alias writes");
  close(second);
  require(unlink("mapped-second") == 0, "mapped alias cleanup");
  printf("FS-MUTATION-CONTRACT: PASS %s hardlink-metadata-and-mapped-cache\n", filesystem);
}

static void directory_contracts(void) {
  require(mkdir("first", 0700) == 0 && mkdir("second", 0700) == 0 &&
              mkdir("first/child", 0700) == 0 && mkdir("second/child", 0700) == 0,
          "create directory move layout");
  int child = open("first/child", O_RDONLY | O_DIRECTORY);
  int old_destination = open("second/child", O_RDONLY | O_DIRECTORY);
  int parent = open("second", O_RDONLY | O_DIRECTORY);
  require(child >= 0 && old_destination >= 0 && parent >= 0, "open directory move layout");
  int leaf = create_file("first/child/leaf", "leaf");
  close(leaf);
  struct stat original;
  struct stat current;
  struct stat parent_status;
  require(fstat(child, &original) == 0 && fstat(parent, &parent_status) == 0,
          "directory identities");
  require(rename("first/child", "second/child") == 0, "cross-parent directory replacement");
  require(stat("second/child", &current) == 0 && current.st_ino == original.st_ino,
          "moved directory retains inode");
  int moved_parent = openat(child, "..", O_RDONLY | O_DIRECTORY);
  require(moved_parent >= 0 && fstat(moved_parent, &current) == 0 &&
              current.st_ino == parent_status.st_ino,
          "open moved directory follows new parent");
  require(fstat(old_destination, &current) == 0 && S_ISDIR(current.st_mode),
          "replaced open directory survives");
  leaf = openat(child, "leaf", O_RDONLY);
  require(leaf >= 0, "open child through retained directory");
  require_contents(leaf, "leaf", "moved directory retains children");
  close(leaf);
  close(moved_parent);
  close(old_destination);
  close(child);
  close(parent);

  require(mkdir("empty", 0700) == 0, "create empty directory");
  errno = 0;
  require(rename("empty", "second/child") == -1 && errno == ENOTEMPTY,
          "nonempty replacement fails");
  require(stat("empty", &current) == 0 && stat("second/child/leaf", &current) == 0,
          "failed replacement preserves both directories");
  errno = 0;
  require(rename("second", "second/child/descendant") == -1 && errno == EINVAL,
          "directory descendant cycle rejected");
  require(stat("second/child/leaf", &current) == 0, "cycle failure preserves namespace");
  errno = 0;
  require(rename("missing", "second/child/leaf") == -1 && errno == ENOENT,
          "missing source leaves destination");
  leaf = open("second/child/leaf", O_RDONLY);
  require(leaf >= 0, "destination after missing-source failure");
  require_contents(leaf, "leaf", "failed rename retains destination contents");
  close(leaf);
  errno = 0;
  require(rename("second/child/leaf", "empty") == -1 && errno == EISDIR,
          "file cannot replace directory");
  errno = 0;
  require(rename("empty", "second/child/leaf") == -1 && errno == ENOTDIR,
          "directory cannot replace file");

  require(unlink("second/child/leaf") == 0 && rmdir("second/child") == 0 && rmdir("second") == 0 &&
              rmdir("first") == 0 && rmdir("empty") == 0,
          "directory cleanup");
  printf("FS-MUTATION-CONTRACT: PASS %s directory-moves-and-failure-preservation\n", filesystem);
}

static void symlink_contracts(void) {
  int target = create_file("target", "unchanged");
  require(symlink("target", "source-link") == 0 && symlink("absent", "destination-link") == 0,
          "create symlink layout");
  require(rename("source-link", "destination-link") == 0, "rename terminal symlink");
  char value[32] = {0};
  require(readlink("destination-link", value, sizeof(value)) == 6 && !memcmp(value, "target", 6),
          "rename moves symlink itself");
  require_contents(target, "unchanged", "rename does not alter symlink target");
  struct stat status;
  errno = 0;
  require(lstat("source-link", &status) == -1 && errno == ENOENT, "old symlink name removed");
  close(target);
  require(unlink("destination-link") == 0 && unlink("target") == 0, "symlink cleanup");
  printf("FS-MUTATION-CONTRACT: PASS %s terminal-symlinks\n", filesystem);
}

static void truncate_contracts(void) {
  const off_t extent = 80 * 1024 + 17;
  const off_t prefix = 4099;
  int file = create_file("resize", "");
  unsigned char buffer[4096];
  memset(buffer, 0x5a, sizeof(buffer));
  for (off_t offset = 0; offset < extent;) {
    size_t amount = extent - offset < (off_t)sizeof(buffer) ? extent - offset : sizeof(buffer);
    require(write(file, buffer, amount) == (ssize_t)amount, "write multi-block file");
    offset += amount;
  }
  require(lseek(file, 55, SEEK_SET) == 55, "set offset before truncate");
  require(ftruncate(file, prefix) == 0, "shrink through indirect blocks");
  require(lseek(file, 0, SEEK_CUR) == 55, "truncate preserves file offset");
  struct stat status;
  require(fstat(file, &status) == 0 && status.st_size == prefix, "shrunk size");
  require(ftruncate(file, extent) == 0, "grow truncated file");
  for (off_t offset = 0; offset < extent;) {
    size_t amount = extent - offset < (off_t)sizeof(buffer) ? extent - offset : sizeof(buffer);
    require(pread(file, buffer, amount, offset) == (ssize_t)amount, "read regrown extent");
    for (size_t i = 0; i < amount; ++i) {
      require(buffer[i] == (offset + (off_t)i < prefix ? 0x5a : 0),
              "truncate preserves prefix and zeroes regrowth");
    }
    offset += amount;
  }
  errno = 0;
  require(ftruncate(file, -1) == -1 && errno == EINVAL, "negative truncate length");
  require(fstat(file, &status) == 0 && status.st_size == extent, "failed truncate preserves size");
  int readonly = open("resize", O_RDONLY);
  require(readonly >= 0, "open read-only truncate descriptor");
  errno = 0;
  require(ftruncate(readonly, 0) == -1 && errno == EINVAL, "read-only truncate rejected");
  close(readonly);
  int truncated = open("resize", O_WRONLY | O_TRUNC);
  require(truncated >= 0 && fstat(file, &status) == 0 && status.st_size == 0,
          "O_TRUNC changes existing open inode");
  close(truncated);
  require(ftruncate(file, 8193) == 0, "grow empty file");
  memset(buffer, 0xff, sizeof(buffer));
  require(pread(file, buffer, sizeof(buffer), 4096) == sizeof(buffer), "read empty-file growth");
  for (size_t i = 0; i < sizeof(buffer); ++i) {
    require(buffer[i] == 0, "empty-file growth is zero filled");
  }
  require(fsync(file) == 0, "sync resized file");
  close(file);
  file = open("resize", O_RDONLY);
  require(file >= 0 && fstat(file, &status) == 0 && status.st_size == 8193, "reopen resized inode");
  close(file);
  require(unlink("resize") == 0, "resize cleanup");
  printf("FS-MUTATION-CONTRACT: PASS %s truncation-and-zero-filling\n", filesystem);
}

static void metadata_contracts(int hardlinks) {
  int file = create_file("metadata", "data");
  int writer = file;
  if (hardlinks) {
    require(link("metadata", "metadata-alias") == 0, "create metadata alias");
    writer = open("metadata-alias", O_RDWR);
    require(writer >= 0, "open metadata alias");
  }
  const struct utimbuf historical = {.actime = 11, .modtime = 22};
  const struct timeval fractional[2] = {{33, 999999}, {44, 999999}};
  struct stat before;
  struct stat after;
  require(syscall(SYS_utime, "metadata", &historical) == 0 && fstat(file, &after) == 0 &&
              after.st_atime == 11 && after.st_mtime == 22,
          "utime stores seconds");
  require(syscall(SYS_utimes, "metadata", fractional) == 0 && fstat(file, &after) == 0 &&
              after.st_atime == 33 && after.st_mtime == 44 && !after.st_atim.tv_nsec &&
              !after.st_mtim.tv_nsec,
          "utimes rounds down to filesystem seconds");
  require(syscall(SYS_futimesat, AT_FDCWD, "metadata", fractional) == 0 &&
              fstat(file, &before) == 0 && before.st_atime == 33 && before.st_mtime == 44,
          "futimesat stores seconds");
  const struct utimbuf invalid_seconds[] = {{-1, 22}, {11, 0x100000000LL}};
  for (size_t i = 0; i < sizeof(invalid_seconds) / sizeof(invalid_seconds[0]); ++i) {
    errno = 0;
    require(syscall(SYS_utime, "metadata", &invalid_seconds[i]) == -1 && errno == EINVAL,
            "utime rejects unrepresentable seconds");
  }
  const struct timeval invalid_fractional[][2] = {{{33, -1}, {44, 0}},
                                                  {{33, 0}, {44, 1000000}},
                                                  {{-1, 0}, {44, 0}},
                                                  {{33, 0}, {0x100000000LL, 0}}};
  for (size_t i = 0; i < sizeof(invalid_fractional) / sizeof(invalid_fractional[0]); ++i) {
    errno = 0;
    require(syscall(SYS_futimesat, AT_FDCWD, "metadata", invalid_fractional[i]) == -1 &&
                errno == EINVAL,
            "futimesat rejects invalid timestamps");
  }
  require(fstat(file, &after) == 0 && after.st_atime == before.st_atime &&
              after.st_mtime == before.st_mtime && after.st_ctime == before.st_ctime,
          "invalid timestamps preserve metadata");
  require(syscall(SYS_futimesat, AT_FDCWD, "metadata", NULL) == 0 && fstat(file, &after) == 0 &&
              after.st_atime > 44 && after.st_atime == after.st_mtime &&
              after.st_mtime <= after.st_ctime,
          "implicit timestamp uses current seconds");

  require(syscall(SYS_utime, "metadata", &historical) == 0 && fstat(file, &before) == 0,
          "seed write timestamps");
  require(
      write(writer, "", 0) == 0 && pwrite(writer, "", 0, 0) == 0 && writev(writer, NULL, 0) == 0,
      "empty writes succeed");
  errno = 0;
  require(pwrite(writer, "x", 1, -1) == -1 && errno == EINVAL, "negative write offset rejected");
  require(fstat(file, &after) == 0 && after.st_atime == before.st_atime &&
              after.st_mtime == before.st_mtime && after.st_ctime == before.st_ctime &&
              after.st_size == before.st_size && after.st_blocks == before.st_blocks,
          "zero-progress writes preserve metadata");

  char bytes[] = "XY";
  struct iovec vector[2] = {{bytes, 1}, {bytes + 1, 1}};
  for (int operation = 0; operation < 4; ++operation) {
    require(syscall(SYS_utime, "metadata", &historical) == 0 && lseek(writer, 0, SEEK_SET) == 0,
            "reset overwrite timestamp");
    ssize_t written;
    if (operation == 1) {
      written = pwrite(writer, bytes, 2, 0);
    } else if (operation == 2) {
      written = writev(writer, vector, 2);
    } else {
      if (operation == 3) {
        require(fcntl(writer, F_SETFL, O_APPEND) == 0, "enable metadata append");
      }
      written = write(writer, bytes, 2);
    }
    require(written == 2 && fstat(file, &after) == 0 && after.st_atime == 11 &&
                after.st_mtime > 22 && after.st_ctime == after.st_mtime,
            "successful writes update modification and change times");
    require(fstat(writer, &before) == 0 && before.st_mtime == after.st_mtime &&
                before.st_ctime == after.st_ctime && before.st_blocks == after.st_blocks &&
                before.st_size == after.st_size,
            "write metadata agrees through both descriptors");
  }
  if (hardlinks) {
    require(fcntl(writer, F_SETFL, 0) == 0 && pwrite(writer, bytes, 1, 3 * after.st_blksize) == 1 &&
                fstat(writer, &before) == 0 && fstat(file, &after) == 0 &&
                before.st_blocks == after.st_blocks &&
                after.st_blocks == 4 * (after.st_blksize / 512),
            "hardlinks share extended allocation counts");
    close(writer);
    require(unlink("metadata-alias") == 0, "remove metadata alias");
  }
  close(file);
  require(unlink("metadata") == 0, "remove metadata file");
  printf("FS-MUTATION-CONTRACT: PASS %s ordinary-write-metadata\n", filesystem);
}

static void ramfs_allocation_contracts(void) {
  const size_t page_size = (size_t)sysconf(_SC_PAGESIZE);
  const blkcnt_t sectors_per_page = page_size / 512;
  int file = create_file("allocation", "");
  struct stat status;
  require(fstat(file, &status) == 0 && status.st_blocks == 0 &&
              ftruncate(file, 3 * page_size) == 0 && fstat(file, &status) == 0 &&
              status.st_blocks == 0,
          "untouched ramfs growth allocates no pages");
  require(pwrite(file, "x", 1, 2 * page_size + 17) == 1 && fstat(file, &status) == 0 &&
              status.st_blocks == sectors_per_page,
          "ramfs accounts one touched page");
  require(pwrite(file, "x", 1, 0) == 1 && fstat(file, &status) == 0 &&
              status.st_blocks == 2 * sectors_per_page,
          "ramfs excludes untouched middle page");
  require(ftruncate(file, page_size) == 0 && fstat(file, &status) == 0 &&
              status.st_blocks == sectors_per_page && ftruncate(file, 0) == 0 &&
              fstat(file, &status) == 0 && status.st_blocks == 0,
          "ramfs truncation retires allocated pages");
  close(file);
  require(unlink("allocation") == 0, "remove allocation file");
  printf("FS-MUTATION-CONTRACT: PASS %s allocated-page-accounting\n", filesystem);
}

static void filesystem_contracts(const char* base, const char* label, int hardlinks) {
  filesystem = label;
  char path[96];
  snprintf(path, sizeof(path), "%s/fs-mutation-contract-%d", base, getpid());
  int previous = open(".", O_RDONLY | O_DIRECTORY);
  require(previous >= 0 && mkdir(path, 0700) == 0 && chdir(path) == 0,
          "create isolated contract directory");
  replacement_contracts(hardlinks);
  directory_contracts();
  if (hardlinks) {
    symlink_contracts();
    hardlink_mapping_contracts();
  } else {
    printf("FS-MUTATION-CONTRACT: SKIP %s terminal-symlinks unsupported-backend\n", filesystem);
  }
  truncate_contracts();
  metadata_contracts(hardlinks);
  if (!hardlinks) {
    ramfs_allocation_contracts();
  }
  require(fchdir(previous) == 0, "restore working directory");
  close(previous);
  require(rmdir(path) == 0, "remove isolated contract directory");
}

void test_fs_mutation_contracts(void) {
  filesystem_contracts("/tmp", "ramfs", 0);
  filesystem_contracts("", "root", 1);
  int device = open("/dev/null", O_WRONLY | O_TRUNC);
  require(device >= 0 && write(device, "x", 1) == 1, "O_TRUNC ignores character device");
  errno = 0;
  require(ftruncate(device, 0) == -1 && errno == EINVAL, "device truncate rejected");
  close(device);
  puts("FS-MUTATION-CONTRACT: PASS all");
}
