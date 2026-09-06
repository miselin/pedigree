#define _GNU_SOURCE
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/xattr.h>

static int cycle(int backend) {
  int failed = 0;
  struct xa_file file = {.fd = -1};
  unsigned char value[257], output[274];
  unsigned char* fault = MAP_FAILED;
  const char* names[] = {"user.binary", "user.empty"};
  for (size_t n = 0; n < sizeof(value); ++n)
    value[n] = (unsigned char)n;
  CHECK(!xa_create(&file, backend, 0));
  fault = mmap(NULL, 2 * xa_page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(fault != MAP_FAILED && !mprotect(fault + xa_page, xa_page, PROT_NONE));
  fault[xa_page - 1] = 0x56;
  void* bad = fault + xa_page;
  const int first = backend == XA_MEMFD ? XA_FD : XA_PATH;
  for (int how = first; how <= XA_FD; ++how) {
    CHECK(!xa_names(&file, how, NULL, 0));
    CHECK(xa_list(&file, how, bad, 1) == 0);
    CHECK(!xa_set(&file, how, names[0], value, sizeof(value), XATTR_CREATE));
    for (int read_how = first; read_how <= XA_FD; ++read_how)
      CHECK(!xa_value(&file, read_how, names[0], value, sizeof(value)));
    CHECK(xa_get(&file, how, names[0], bad, 0) == sizeof(value));
    memset(output, 0xa5, sizeof(output));
    CHECK(xa_get(&file, how, names[0], output, sizeof(value)) == sizeof(value));
    CHECK(!memcmp(output, value, sizeof(value)) && output[sizeof(value)] == 0xa5);
    memset(output, 0xa5, sizeof(output));
    CHECK(xa_get(&file, how, names[0], output, sizeof(value) - 1) == -1 && errno == ERANGE);
    for (size_t n = 0; n < sizeof(output); ++n)
      CHECK(output[n] == 0xa5);
    CHECK(xa_get(&file, how, names[0], output, SIZE_MAX) == sizeof(value));
    CHECK(!memcmp(output, value, sizeof(value)) && output[sizeof(value)] == 0xa5);
    CHECK(xa_set(&file, how, names[0], "x", 1, XATTR_CREATE) == -1 && errno == EEXIST);
    CHECK(xa_set(&file, how, names[0], "x", 1, XATTR_CREATE | XATTR_REPLACE) == -1 &&
          errno == EEXIST);
    CHECK(xa_set(&file, how, names[0], "x", 1, 4) == -1 && errno == EINVAL);
    CHECK(xa_set(&file, how, names[0], bad, 1, XATTR_REPLACE) == -1 && errno == EFAULT);
    CHECK(xa_set(&file, how, names[0], fault + xa_page - 1, 2, 0) == -1 && errno == EFAULT);
    CHECK(!xa_value(&file, how, names[0], value, sizeof(value)));
    CHECK(xa_get(&file, how, names[0], bad, sizeof(value)) == -1 && errno == EFAULT);
    CHECK(!xa_value(&file, how, names[0], value, sizeof(value)));
    CHECK(!xa_set(&file, how, names[1], bad, 0, 0));
    CHECK(xa_get(&file, how, names[1], bad, 1) == 0);
    CHECK(xa_set(&file, how, names[1], NULL, 0, XATTR_CREATE) == -1 && errno == EEXIST);
    CHECK(!xa_names(&file, how, names, 2));
    const size_t list_size = strlen(names[0]) + strlen(names[1]) + 2;
    CHECK(xa_list(&file, how, bad, 0) == (ssize_t)list_size);
    memset(output, 0xa5, sizeof(output));
    CHECK(xa_list(&file, how, (char*)output, list_size - 1) == -1 && errno == ERANGE);
    for (size_t n = 0; n < sizeof(output); ++n)
      CHECK(output[n] == 0xa5);
    CHECK(xa_list(&file, how, (char*)output, SIZE_MAX) == (ssize_t)list_size);
    CHECK(output[list_size] == 0xa5);
    CHECK(xa_list(&file, how, bad, list_size) == -1 && errno == EFAULT);
    CHECK(!xa_names(&file, how, names, 2));
    CHECK(!xa_set(&file, how, names[0], value, 191, XATTR_REPLACE));
    CHECK(!xa_value(&file, how, names[0], value, 191));
    CHECK(!xa_set(&file, how, names[0], "last", 4, 0));
    CHECK(!xa_value(&file, how, names[0], "last", 4));
    CHECK(!xa_remove(&file, how, names[0]));
    CHECK(xa_remove(&file, how, names[0]) == -1 && errno == ENODATA);
    CHECK(xa_set(&file, how, names[0], "x", 1, XATTR_REPLACE) == -1 && errno == ENODATA);
    CHECK(xa_set(&file, how, names[0], "x", 1, XATTR_CREATE | XATTR_REPLACE) == -1 &&
          errno == ENODATA);
    CHECK(!xa_remove(&file, how, names[1]));
    CHECK(!xa_names(&file, how, NULL, 0));
  }
out:
  if (fault != MAP_FAILED)
    munmap(fault, 2 * xa_page);
  xa_close(&file);
  if (failed)
    fprintf(stderr, "XATTR-CONTRACT: values backend=%d\n", backend);
  return failed;
}

static int memory_limits(int backend) {
  int failed = 0;
  struct xa_file file = {.fd = -1};
  unsigned char* large = malloc(65537);
  CHECK(large != NULL && !xa_create(&file, backend, 0));
  for (size_t n = 0; n < 65537; ++n)
    large[n] = (unsigned char)(n * 29 + (n >> 8));
  CHECK(!fsetxattr(file.fd, "user.large", large, 65536, 0));
  CHECK(!xa_value(&file, XA_FD, "user.large", large, 65536));
  CHECK(fsetxattr(file.fd, "user.large", large, 65537, 0) == -1 && errno == E2BIG);
  CHECK(!xa_value(&file, XA_FD, "user.large", large, 65536));
  CHECK(!fsetxattr(file.fd, "user.second", large, 65536, 0));
  CHECK(!fsetxattr(file.fd, "user.third", large, 65536, 0));
  CHECK(fsetxattr(file.fd, "user.fourth", large, 65536, 0) == -1 && errno == ENOSPC);
  CHECK(!xa_value(&file, XA_FD, "user.large", large, 65536));
  CHECK(!fremovexattr(file.fd, "user.large"));
  CHECK(!fsetxattr(file.fd, "user.small", large, 32768, 0));
  CHECK(!fsetxattr(file.fd, "user.fourth", large, 65536, 0));
  CHECK(fsetxattr(file.fd, "user.small", large, 65536, XATTR_REPLACE) == -1 && errno == ENOSPC);
  CHECK(!xa_value(&file, XA_FD, "user.small", large, 32768));
  xa_close(&file);
  CHECK(!xa_create(&file, backend, 0));
  char names[128][24];
  const char* pointers[128];
  for (size_t n = 0; n < 128; ++n) {
    snprintf(names[n], sizeof(names[n]), "user.entry%03zu", n);
    pointers[n] = names[n];
    CHECK(!fsetxattr(file.fd, names[n], NULL, 0, XATTR_CREATE));
  }
  CHECK(!xa_names(&file, XA_FD, pointers, 128));
  CHECK(fsetxattr(file.fd, "user.overflow", NULL, 0, 0) == -1 && errno == ENOSPC);
  CHECK(!fremovexattr(file.fd, names[17]));
  CHECK(!fsetxattr(file.fd, "user.overflow", NULL, 0, 0));
  pointers[17] = "user.overflow";
  CHECK(!xa_names(&file, XA_FD, pointers, 128));
out:
  free(large);
  xa_close(&file);
  return failed;
}

int xa_values(void) {
  for (int backend = XA_MEMFD; backend <= XA_EXT2; ++backend) {
    if (cycle(backend))
      return 1;
  }
  return memory_limits(XA_MEMFD) || memory_limits(XA_RAMFS);
}
