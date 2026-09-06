#define _GNU_SOURCE
#include <fcntl.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/stat.h>
#include <sys/xattr.h>

struct paths {
  char base[192], parent[192], original[224], renamed[224], alias[224];
  char directory[224], empty[224], completion[224];
};

static const unsigned char binary[] = {0, 0xff, 0x17, 0, 0x93, 4, 0x80};
static const unsigned char replacement[] = {0x51, 0, 0xe7, 0x22};
static const char* const file_names[] = {"user.binary", "user.empty", "user.replaced"};

static int path_init(struct paths* paths, const char* base) {
  const size_t length = strlen(base);
  if (length < 2 || length >= sizeof(paths->base) || base[0] != '/' || base[length - 1] == '/')
    return -1;
  strcpy(paths->base, base);
  strcpy(paths->parent, base);
  char* end = strrchr(paths->parent, '/');
  if (end == paths->parent)
    end[1] = 0;
  else
    *end = 0;
  snprintf(paths->original, sizeof(paths->original), "%s/payload-original", base);
  snprintf(paths->renamed, sizeof(paths->renamed), "%s/payload-renamed", base);
  snprintf(paths->alias, sizeof(paths->alias), "%s/payload-link", base);
  snprintf(paths->directory, sizeof(paths->directory), "%s/directory", base);
  snprintf(paths->empty, sizeof(paths->empty), "%s/ea-only", base);
  snprintf(paths->completion, sizeof(paths->completion), "%s/complete", base);
  return 0;
}

static void payload(unsigned char bytes[513]) {
  for (size_t n = 0; n < 513; ++n)
    bytes[n] = (unsigned char)(n * 29 + 3);
}

static int sync_directory(const char* path) {
  int fd = open(path, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  if (fd < 0)
    return -1;
  int result = fsync(fd), saved = errno;
  if (close(fd) && !result) {
    saved = errno;
    result = -1;
  }
  errno = saved;
  return result;
}

static int create_fixtures(const struct paths* paths) {
  int failed = 0, fd = -1, directory = -1, empty = -1;
  struct stat status;
  unsigned char bytes[513];
  payload(bytes);
  errno = 0;
  CHECK(lstat(paths->base, &status) == -1 && errno == ENOENT);
  CHECK(!mkdir(paths->base, 0700));
  CHECK(!mkdir(paths->directory, 0700));
  directory = open(paths->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(directory >= 0);
  CHECK(!fsetxattr(directory, "user.directory", binary, sizeof(binary), XATTR_CREATE));
  fd = open(paths->original, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  CHECK(fd >= 0);
  CHECK(!xa_write_all(fd, bytes, sizeof(bytes)));
  CHECK(!fsetxattr(fd, "user.binary", binary, sizeof(binary), XATTR_CREATE));
  CHECK(!fsetxattr(fd, "user.empty", NULL, 0, XATTR_CREATE));
  CHECK(!fsetxattr(fd, "user.replaced", "old", 3, XATTR_CREATE));
  CHECK(!fsetxattr(fd, "user.replaced", replacement, sizeof(replacement), XATTR_REPLACE));
  CHECK(!fsetxattr(fd, "user.removed", "gone", 4, XATTR_CREATE));
  CHECK(!fremovexattr(fd, "user.removed"));
  CHECK(!link(paths->original, paths->alias));
  CHECK(!rename(paths->original, paths->renamed));
  empty = open(paths->empty, O_RDWR | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
  CHECK(empty >= 0);
  CHECK(!fstat(empty, &status) && !status.st_size && !status.st_blocks);
  CHECK(!fsetxattr(empty, "user.only", binary, sizeof(binary), XATTR_CREATE));
  CHECK(!fstat(empty, &status) && !status.st_size &&
        status.st_blocks == (blkcnt_t)(xa_block / 512));
  CHECK(!fsync(fd) && !fsync(empty) && !fsync(directory));
  CHECK(!sync_directory(paths->base) && !sync_directory(paths->parent));
out:
  if (empty >= 0 && close(empty))
    failed = 1;
  if (directory >= 0 && close(directory))
    failed = 1;
  if (fd >= 0 && close(fd))
    failed = 1;
  // Leave a failed fixture in place for disk and serial inspection.
  return failed;
}

static int verify_fixtures(const struct paths* paths) {
  int failed = 0, renamed = -1, directory = -1, empty = -1;
  struct xa_file file = {.fd = -1, .backend = XA_EXT2};
  struct stat alias_status, renamed_status, status;
  unsigned char expected[513], actual[513], extra;
  payload(expected);
  CHECK(!stat(paths->base, &status) && S_ISDIR(status.st_mode));
  errno = 0;
  CHECK(lstat(paths->original, &status) == -1 && errno == ENOENT);
  file.fd = open(paths->alias, O_RDONLY | O_CLOEXEC);
  renamed = open(paths->renamed, O_RDONLY | O_CLOEXEC);
  CHECK(file.fd >= 0 && renamed >= 0);
  CHECK(!fstat(file.fd, &alias_status) && !fstat(renamed, &renamed_status));
  CHECK(S_ISREG(alias_status.st_mode) && S_ISREG(renamed_status.st_mode) &&
        alias_status.st_dev == renamed_status.st_dev &&
        alias_status.st_ino == renamed_status.st_ino && alias_status.st_nlink == 2 &&
        renamed_status.st_nlink == 2 && alias_status.st_size == sizeof(expected));
  CHECK(!xa_read_all(file.fd, actual, sizeof(actual)) && !memcmp(actual, expected, sizeof(actual)));
  CHECK(read(file.fd, &extra, 1) == 0);
  CHECK(!xa_value(&file, XA_FD, "user.binary", binary, sizeof(binary)));
  CHECK(!xa_value(&file, XA_FD, "user.empty", NULL, 0));
  CHECK(!xa_value(&file, XA_FD, "user.replaced", replacement, sizeof(replacement)));
  errno = 0;
  CHECK(fgetxattr(file.fd, "user.removed", NULL, 0) == -1 && errno == ENODATA);
  CHECK(!xa_names(&file, XA_FD, file_names, sizeof(file_names) / sizeof(file_names[0])));
  struct xa_file second = {.fd = renamed, .backend = XA_EXT2};
  CHECK(!xa_value(&second, XA_FD, "user.binary", binary, sizeof(binary)));
  CHECK(!xa_names(&second, XA_FD, file_names, sizeof(file_names) / sizeof(file_names[0])));
  directory = open(paths->directory, O_RDONLY | O_DIRECTORY | O_CLOEXEC);
  CHECK(directory >= 0 && !fstat(directory, &status) && S_ISDIR(status.st_mode));
  struct xa_file dir = {.fd = directory, .backend = XA_EXT2, .directory = 1};
  const char* directory_names[] = {"user.directory"};
  CHECK(!xa_value(&dir, XA_FD, "user.directory", binary, sizeof(binary)));
  CHECK(!xa_names(&dir, XA_FD, directory_names, 1));
  empty = open(paths->empty, O_RDONLY | O_CLOEXEC);
  CHECK(empty >= 0 && !fstat(empty, &status));
  CHECK(S_ISREG(status.st_mode) && !status.st_size &&
        status.st_blocks == (blkcnt_t)(xa_block / 512));
  struct xa_file ea_only = {.fd = empty, .backend = XA_EXT2};
  const char* empty_names[] = {"user.only"};
  CHECK(!xa_value(&ea_only, XA_FD, "user.only", binary, sizeof(binary)));
  CHECK(!xa_names(&ea_only, XA_FD, empty_names, 1));
out:
  if (empty >= 0)
    close(empty);
  if (directory >= 0)
    close(directory);
  if (renamed >= 0)
    close(renamed);
  if (file.fd >= 0)
    close(file.fd);
  return failed;
}

static int completion(const struct paths* paths, const char* expected, int write_stage) {
  int failed = 0, fd = -1;
  const size_t length = strlen(expected);
  char actual[256], extra;
  struct stat status;
  CHECK(length < sizeof(actual));
  if (write_stage) {
    fd = open(paths->completion, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    CHECK(fd >= 0);
    CHECK(!xa_write_all(fd, expected, length) && !fsync(fd));
    CHECK(!close(fd));
    fd = -1;
    CHECK(!sync_directory(paths->base) && !sync_directory(paths->parent));
  } else {
    fd = open(paths->completion, O_RDONLY | O_NOFOLLOW | O_CLOEXEC);
    CHECK(fd >= 0 && !fstat(fd, &status) && S_ISREG(status.st_mode) &&
          status.st_size == (off_t)length);
    CHECK(!xa_read_all(fd, actual, length) && !memcmp(actual, expected, length));
    CHECK(read(fd, &extra, 1) == 0);
  }
out:
  if (fd >= 0)
    close(fd);
  return failed;
}

int xa_persistence(const char* base, const char* token, int write_stage) {
  struct paths paths;
  char manifest[256];
  const size_t token_length = strlen(token);
  if (path_init(&paths, base) || !token_length || token_length > 96)
    return 1;
  for (size_t n = 0; n < token_length; ++n) {
    const unsigned char byte = token[n];
    if (!((byte >= 'a' && byte <= 'z') || (byte >= 'A' && byte <= 'Z') ||
          (byte >= '0' && byte <= '9') || byte == '-' || byte == '_' || byte == '.'))
      return 1;
  }
  int length =
      snprintf(manifest, sizeof(manifest),
               "PEDIGREE-XATTR-PERSIST 1\nfixture=1\ntoken=%s\nblock=%zu\n", token, xa_block);
  if (length < 0 || (size_t)length >= sizeof(manifest))
    return 1;
  if (write_stage) {
    if (create_fixtures(&paths) || verify_fixtures(&paths))
      return 1;
    return completion(&paths, manifest, 1);
  }
  // The completion record is checked before any fixture is opened. The read
  // stage never creates, repairs, removes, or mutates an attribute.
  return completion(&paths, manifest, 0) || verify_fixtures(&paths);
}
