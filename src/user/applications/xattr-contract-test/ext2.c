#define _GNU_SOURCE
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/stat.h>
#include <sys/xattr.h>

static int sectors(void) {
  int failed = 0;
  struct xa_file file = {.fd = -1};
  struct stat initial, with_ea, after;
  CHECK(!xa_create(&file, XA_EXT2, 0));
  CHECK(!fstat(file.fd, &initial) && !initial.st_size);
  CHECK(!fsetxattr(file.fd, "user.first", "a", 1, XATTR_CREATE));
  CHECK(!fstat(file.fd, &with_ea));
  CHECK(with_ea.st_size == 0 &&
        with_ea.st_blocks == initial.st_blocks + (blkcnt_t)(xa_block / 512));
  CHECK(!fsetxattr(file.fd, "user.empty", NULL, 0, XATTR_CREATE));
  CHECK(!fstat(file.fd, &after) && after.st_blocks == with_ea.st_blocks);
  CHECK(!fremovexattr(file.fd, "user.first"));
  CHECK(!xa_value(&file, XA_FD, "user.empty", NULL, 0));
  CHECK(!fstat(file.fd, &after) && after.st_blocks == with_ea.st_blocks);
  CHECK(!fremovexattr(file.fd, "user.empty"));
  CHECK(!fstat(file.fd, &after) && !after.st_size && after.st_blocks == initial.st_blocks);
  CHECK(!xa_names(&file, XA_FD, NULL, 0));
  CHECK(!fsetxattr(file.fd, "user.reused", "b", 1, XATTR_CREATE));
  CHECK(!fstat(file.fd, &after) && after.st_blocks == with_ea.st_blocks);
out:
  xa_close(&file);
  return failed;
}

static int capacity_and_resize(void) {
  int failed = 0, alias = -1;
  struct xa_file file = {.fd = -1};
  const size_t original_size = xa_block + 137, grown_size = xa_block + 211;
  unsigned char *data = NULL, *actual = NULL, *large = NULL;
  struct stat before, after;
  static const unsigned char value[] = {0, 0xff, 7, 0, 3};
  const char* names[] = {"user.keep"};
  data = malloc(original_size);
  actual = malloc(grown_size);
  large = malloc(xa_block + 1);
  CHECK(data && actual && large);
  for (size_t n = 0; n < original_size; ++n)
    data[n] = (unsigned char)(n * 37 + 11);
  memset(large, 0xa3, xa_block + 1);
  CHECK(!xa_create(&file, XA_EXT2, 0));
  CHECK(!xa_write_all(file.fd, data, original_size));
  CHECK(!fsetxattr(file.fd, "user.keep", value, sizeof(value), XATTR_CREATE));
  CHECK(!fstat(file.fd, &before));
  errno = 0;
  CHECK(fsetxattr(file.fd, "user.keep", large, xa_block + 1, XATTR_REPLACE) == -1 &&
        errno == (xa_block == 65536 ? E2BIG : ERANGE));
  errno = 0;
  CHECK(fsetxattr(file.fd, "user.keep", large, xa_block, XATTR_REPLACE) == -1 && errno == ENOSPC);
  errno = 0;
  CHECK(fsetxattr(file.fd, "user.no-space", large, xa_block, XATTR_CREATE) == -1 &&
        errno == ENOSPC);
  CHECK(!fstat(file.fd, &after));
  CHECK(after.st_size == before.st_size && after.st_blocks == before.st_blocks &&
        after.st_ctim.tv_sec == before.st_ctim.tv_sec &&
        after.st_ctim.tv_nsec == before.st_ctim.tv_nsec);
  CHECK(!xa_value(&file, XA_FD, "user.keep", value, sizeof(value)));
  CHECK(!xa_names(&file, XA_FD, names, 1));
  CHECK(pread(file.fd, actual, original_size, 0) == (ssize_t)original_size &&
        !memcmp(actual, data, original_size));
  alias = xa_open_alias(&file);
  CHECK(alias >= 0);
  CHECK(!ftruncate(alias, 137));
  CHECK(!xa_value(&file, XA_FD, "user.keep", value, sizeof(value)));
  CHECK(!ftruncate(alias, grown_size));
  CHECK(!fstat(file.fd, &after) && after.st_size == (off_t)grown_size);
  memset(actual, 0xa5, grown_size);
  CHECK(pread(file.fd, actual, grown_size, 0) == (ssize_t)grown_size);
  CHECK(!memcmp(actual, data, 137));
  for (size_t n = 137; n < grown_size; ++n)
    CHECK(actual[n] == 0);
  CHECK(!xa_value(&file, XA_PATH, "user.keep", value, sizeof(value)));
  CHECK(!xa_names(&file, XA_PATH, names, 1));
  CHECK(!ftruncate(file.fd, 0));
  CHECK(!fstat(alias, &after) && !after.st_size && after.st_blocks == (blkcnt_t)(xa_block / 512));
  CHECK(!fremovexattr(alias, "user.keep"));
  CHECK(!fstat(file.fd, &after) && !after.st_size && !after.st_blocks);
out:
  if (alias >= 0)
    close(alias);
  xa_close(&file);
  free(large);
  free(actual);
  free(data);
  return failed;
}

int xa_ext2(void) {
  return sectors() || capacity_and_resize();
}
