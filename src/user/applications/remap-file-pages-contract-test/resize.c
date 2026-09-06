#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

static int resize_backend(int backend) {
  int failed = 0;
  struct rp_file file = {.fd = -1};
  int alias_fd = -1;
  unsigned char* mapping = MAP_FAILED;
  unsigned char* alias = MAP_FAILED;
  const size_t boundary = rp_page + 137;
  CHECK(rp_create(&file, backend) == 0);
  alias_fd = rp_open_alias(&file);
  CHECK(alias_fd >= 0);
  mapping = mmap(NULL, 4 * rp_page, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
  alias = mmap(NULL, 2 * rp_page, PROT_READ | PROT_WRITE, MAP_SHARED, alias_fd, 0);
  CHECK(mapping != MAP_FAILED && alias != MAP_FAILED);
  CHECK(remap_file_pages(mapping, rp_page, 0, 6, 0) == 0);
  CHECK(remap_file_pages(mapping + 2 * rp_page, rp_page, 0, 6, 0) == 0);
  CHECK(remap_file_pages(mapping + 3 * rp_page, rp_page, 0, 0, 0) == 0);
  CHECK(remap_file_pages(alias, rp_page, 0, 6, 0) == 0);
  CHECK(rp_matches(mapping, 6 * rp_page, rp_page, 0));
  CHECK(rp_matches(mapping + rp_page, rp_page, rp_page, 0));
  CHECK(rp_matches(mapping + 2 * rp_page, 6 * rp_page, rp_page, 0));
  CHECK(rp_matches(mapping + 3 * rp_page, 0, rp_page, 0));
  CHECK(rp_matches(alias, 6 * rp_page, rp_page, 0));
  CHECK(rp_matches(alias + rp_page, rp_page, rp_page, 0));
  mapping[19] = 0xa1;
  CHECK(mapping[2 * rp_page + 19] == 0xa1 && alias[19] == 0xa1);

  CHECK(ftruncate(alias_fd, boundary) == 0);
  CHECK(rp_size(file.fd, boundary) == 0 && rp_size(alias_fd, boundary) == 0);
  // Check residency before a read can reload a mistakenly discarded backing page.
  CHECK(rp_resident(mapping, 4, 0xa, "rebiased shrink main") == 0);
  CHECK(rp_resident(alias, 2, 0x2, "rebiased shrink alias") == 0);
  CHECK(rp_fault(mapping, SIGBUS, 0) == 0);
  CHECK(rp_fault(mapping + 2 * rp_page, SIGBUS, 0) == 0);
  CHECK(rp_fault(alias, SIGBUS, 0) == 0);
  CHECK(rp_matches(mapping + 3 * rp_page, 0, rp_page, 0));
  CHECK(rp_matches(mapping + rp_page, rp_page, 137, 0));
  CHECK(rp_matches(alias + rp_page, rp_page, 137, 0));
  CHECK(rp_matches(mapping + boundary, boundary, rp_page - 137, 1));
  CHECK(rp_matches(alias + boundary, boundary, rp_page - 137, 1));
  CHECK(rp_contents(file.fd, 0, boundary, 0) == 0);

  CHECK(ftruncate(alias_fd, 8 * rp_page) == 0 && rp_size(file.fd, 8 * rp_page) == 0);
  CHECK(rp_contents(file.fd, boundary, 8 * rp_page - boundary, 1) == 0);
  CHECK(rp_matches(mapping, 6 * rp_page, rp_page, 1));
  CHECK(rp_matches(mapping + 2 * rp_page, 6 * rp_page, rp_page, 1));
  CHECK(rp_matches(alias, 6 * rp_page, rp_page, 1));
  CHECK(rp_matches(mapping + 3 * rp_page, 0, rp_page, 0));
  CHECK(rp_matches(mapping + rp_page, rp_page, 137, 0));
  CHECK(rp_matches(mapping + boundary, boundary, rp_page - 137, 1));
  mapping[2 * rp_page + 73] = 0xd4;
  CHECK(mapping[73] == 0xd4 && alias[73] == 0xd4);
  alias[74] = 0x9a;
  CHECK(mapping[74] == 0x9a && mapping[2 * rp_page + 74] == 0x9a);
  unsigned char bytes[2] = {0};
  CHECK(pread(alias_fd, bytes, sizeof(bytes), 6 * rp_page + 73) == (ssize_t)sizeof(bytes));
  CHECK(bytes[0] == 0xd4 && bytes[1] == 0x9a);
  CHECK(msync(mapping, 4 * rp_page, MS_SYNC) == 0);
out:
  if (alias != MAP_FAILED)
    munmap(alias, 2 * rp_page);
  if (mapping != MAP_FAILED)
    munmap(mapping, 4 * rp_page);
  if (alias_fd >= 0)
    close(alias_fd);
  rp_close(&file);
  if (failed)
    fprintf(stderr, "REMAP-FILE-PAGES-CONTRACT: resize backend=%d\n", backend);
  return failed;
}

static int sealed_shrink(void) {
  int failed = 0;
  struct rp_file file = {.fd = -1};
  unsigned char* mapping = MAP_FAILED;
  CHECK(rp_create(&file, RP_MEMFD) == 0);
  mapping = mmap(NULL, 2 * rp_page, PROT_READ, MAP_SHARED, file.fd, 0);
  CHECK(mapping != MAP_FAILED && remap_file_pages(mapping, 2 * rp_page, 0, 6, 0) == 0);
  CHECK(rp_matches(mapping, 6 * rp_page, 2 * rp_page, 0));
  CHECK(fcntl(file.fd, F_ADD_SEALS, F_SEAL_SHRINK) == 0);
  errno = 0;
  CHECK(ftruncate(file.fd, rp_page) == -1 && errno == EPERM);
  CHECK(rp_size(file.fd, 8 * rp_page) == 0);
  CHECK(rp_resident(mapping, 2, 3, "sealed shrink preserved aliases") == 0);
  CHECK(rp_matches(mapping, 6 * rp_page, 2 * rp_page, 0));
out:
  if (mapping != MAP_FAILED)
    munmap(mapping, 2 * rp_page);
  rp_close(&file);
  return failed;
}

int rp_resize(void) {
  for (int backend = RP_MEMFD; backend <= RP_EXT2; ++backend) {
    if (resize_backend(backend))
      return 1;
  }
  return sealed_shrink();
}
