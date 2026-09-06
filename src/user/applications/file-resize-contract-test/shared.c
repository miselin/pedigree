#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

int fr_shared(int backend) {
  int failed = 0, other = -1, duplicate = -1;
  struct fr_file file = {.fd = -1};
  const size_t page = fr_page, length = 4 * page, cut = page + 137;
  unsigned char *shared = MAP_FAILED, *alias = MAP_FAILED, *borrowed = MAP_FAILED;
  unsigned char byte;
  CHECK(!fr_create(&file, backend));
  CHECK((other = fr_open_alias(&file)) >= 0 && (duplicate = dup(file.fd)) >= 0);
  shared = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
  alias = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, other, page);
  borrowed = mmap(NULL, 3 * page, PROT_READ, MAP_PRIVATE, other, page);
  CHECK(shared != MAP_FAILED && alias != MAP_FAILED && borrowed != MAP_FAILED);
  CHECK(fr_matches(shared, 0, length, 0) && fr_matches(alias, page, 3 * page, 0) &&
        fr_matches(borrowed, page, 3 * page, 0));
  CHECK(!fr_resident(shared, 4, 15, "shared before shrink") &&
        !fr_resident(alias, 3, 7, "offset shared before shrink") &&
        !fr_resident(borrowed, 3, 7, "borrowed private before shrink"));
  const off_t position = 3 * page + 91;
  CHECK(lseek(file.fd, position, SEEK_SET) == position);
  if (file.path[0])
    CHECK(lseek(other, 7, SEEK_SET) == 7);
  const off_t other_position = file.path[0] ? 7 : position;

  CHECK(!ftruncate(duplicate, cut));
  /* Reads would conceal a resize implementation that discarded the valid prefix. */
  CHECK(!fr_resident(shared, 4, 3, "shared after partial shrink") &&
        !fr_resident(alias, 3, 1, "offset shared after partial shrink") &&
        !fr_resident(borrowed, 3, 1, "borrowed private after partial shrink"));
  CHECK(!fr_size(file.fd, cut) && !fr_size(other, cut));
  CHECK(lseek(file.fd, 0, SEEK_CUR) == position && lseek(duplicate, 0, SEEK_CUR) == position &&
        lseek(other, 0, SEEK_CUR) == other_position);
  CHECK(pread(other, &byte, 1, cut) == 0);
  CHECK(fr_matches(shared, 0, cut, 0) && fr_matches(shared + cut, cut, 2 * page - cut, 1));
  CHECK(fr_matches(alias, page, 137, 0) && fr_matches(alias + 137, cut, page - 137, 1));
  CHECK(fr_matches(borrowed, page, 137, 0) && fr_matches(borrowed + 137, cut, page - 137, 1));
  CHECK(!fr_contents(file.fd, 0, cut, 0));
  CHECK(!fr_fault(shared + 2 * page) && !fr_fault(alias + page) && !fr_fault(borrowed + 2 * page));

  CHECK(!ftruncate(other, length));
  CHECK(!fr_resident(shared, 2, 3, "shared prefix after regrowth") &&
        !fr_resident(alias, 1, 1, "offset prefix after regrowth"));
  CHECK(!fr_size(file.fd, length) && fr_matches(shared, 0, cut, 0) &&
        fr_matches(shared + cut, cut, length - cut, 1));
  CHECK(fr_matches(alias, page, 137, 0) && fr_matches(alias + 137, cut, length - cut, 1));
  CHECK(fr_matches(borrowed, page, 137, 0) && fr_matches(borrowed + 137, cut, length - cut, 1));
  CHECK(!fr_contents(file.fd, 0, cut, 0) && !fr_contents(other, cut, length - cut, 1));
  alias[137] = 0xd5;
  CHECK(shared[cut] == 0xd5 && borrowed[137] == 0xd5 && pread(file.fd, &byte, 1, cut) == 1 &&
        byte == 0xd5);
  CHECK(lseek(file.fd, 0, SEEK_CUR) == position && lseek(other, 0, SEEK_CUR) == other_position);

  CHECK(!ftruncate(file.fd, page));
  CHECK(!fr_resident(shared, 4, 1, "shared after aligned shrink") &&
        !fr_resident(alias, 3, 0, "offset shared after aligned shrink"));
  CHECK(fr_matches(shared, 0, page, 0) && !fr_fault(shared + page));
  CHECK(!ftruncate(file.fd, length));
  CHECK(!fr_resident(shared, 1, 1, "aligned prefix after regrowth"));
  CHECK(fr_matches(shared, 0, page, 0) && fr_matches(shared + page, page, 3 * page, 1));
  CHECK(fr_matches(alias, page, 3 * page, 1) && fr_matches(borrowed, page, 3 * page, 1));
  CHECK(!fr_contents(file.fd, page, 3 * page, 1));
out:
  if (borrowed != MAP_FAILED)
    munmap(borrowed, 3 * page);
  if (alias != MAP_FAILED)
    munmap(alias, 3 * page);
  if (shared != MAP_FAILED)
    munmap(shared, length);
  if (duplicate >= 0)
    close(duplicate);
  if (other >= 0)
    close(other);
  fr_close(&file);
  return failed;
}
