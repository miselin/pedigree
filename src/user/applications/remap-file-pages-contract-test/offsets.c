#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

static int nonlinear(int backend) {
  int failed = 0;
  struct rp_file file = {.fd = -1};
  const size_t page = rp_page;
  unsigned char *mapping = MAP_FAILED, *alias = MAP_FAILED, byte;
  CHECK(!rp_create(&file, backend));
  mapping = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 2 * page);
  alias = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
  CHECK(mapping != MAP_FAILED && alias != MAP_FAILED);
  CHECK(rp_matches(mapping, 2 * page, 4 * page, 0));
  CHECK(lseek(file.fd, 79, SEEK_SET) == 79);
  CHECK(!remap_file_pages(mapping + page, page, 0, 0, 0));
  CHECK(!remap_file_pages(mapping + 2 * page, page, 0, 0, MAP_NONBLOCK));
  CHECK(rp_matches(mapping, 2 * page, page, 0) && rp_matches(mapping + page, 0, page, 0) &&
        rp_matches(mapping + 2 * page, 0, page, 0) &&
        rp_matches(mapping + 3 * page, 5 * page, page, 0));
  mapping[page + 137] = 0xab;
  CHECK(mapping[2 * page + 137] == 0xab && alias[137] == 0xab &&
        pread(file.fd, &byte, 1, 137) == 1 && byte == 0xab);
  alias[137] = rp_pattern(137);
  CHECK(!msync(mapping + page, page, MS_SYNC));

  /* Prior calls created differing-offset fragments of the same open mapping. */
  CHECK(!remap_file_pages(mapping, 4 * page, 0, 3,
                          MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED | (int)0x80000000u));
  CHECK(rp_matches(mapping, 3 * page, 4 * page, 0));
  CHECK(!remap_file_pages(mapping + 17, page + 37, 0, 1, 0));
  CHECK(rp_matches(mapping, page, page, 0) && rp_matches(mapping + page, 4 * page, 3 * page, 0));
  CHECK(!remap_file_pages(mapping + 3 * page, page, 0, 8, 0));
  CHECK(!remap_file_pages(mapping + 3 * page, page, 0, 8, MAP_NONBLOCK));
  CHECK(!rp_fault(mapping + 3 * page, SIGBUS, 0));
  CHECK(rp_matches(mapping, page, page, 0) && rp_matches(mapping + page, 4 * page, 2 * page, 0));
  CHECK(!remap_file_pages(mapping + 3 * page, page, 0, 2, 0));
  CHECK(rp_matches(mapping + 3 * page, 2 * page, page, 0));
  CHECK(lseek(file.fd, 0, SEEK_CUR) == 79 && !rp_size(file.fd, 8 * page));
out:
  if (alias != MAP_FAILED)
    munmap(alias, page);
  if (mapping != MAP_FAILED)
    munmap(mapping, 4 * page);
  rp_close(&file);
  if (failed)
    fprintf(stderr, "REMAP-FILE-PAGES-CONTRACT: offsets backend=%d\n", backend);
  return failed;
}

static int adjacent_open(void) {
  int failed = 0, duplicate = -1;
  struct rp_file file = {.fd = -1};
  const size_t page = rp_page;
  unsigned char* area = MAP_FAILED;
  CHECK(!rp_create(&file, RP_MEMFD));
  CHECK((duplicate = dup(file.fd)) >= 0);
  area = mmap(NULL, 4 * page, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(area != MAP_FAILED);
  CHECK(mmap(area + page, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, file.fd,
             5 * page) == area + page);
  CHECK(mmap(area + 2 * page, page, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED, duplicate,
             page) == area + 2 * page);
  CHECK(!remap_file_pages(area + page, 2 * page, 0, 2, 0));
  CHECK(rp_matches(area + page, 2 * page, 2 * page, 0));
  CHECK(!mprotect(area + 2 * page, page, PROT_READ));
  CHECK(!mprotect(area + 2 * page, page, PROT_READ | PROT_WRITE));
  CHECK(!remap_file_pages(area + 2 * page, page, 0, 7, MAP_NONBLOCK));
  CHECK(!remap_file_pages(area + page, 2 * page, 0, 0, 0));
  CHECK(rp_matches(area + page, 0, 2 * page, 0));
out:
  if (area != MAP_FAILED)
    munmap(area, 4 * page);
  if (duplicate >= 0)
    close(duplicate);
  rp_close(&file);
  return failed;
}

int rp_offsets(void) {
  for (int backend = RP_MEMFD; backend <= RP_EXT2; ++backend) {
    if (nonlinear(backend))
      return 1;
  }
  return adjacent_open();
}
