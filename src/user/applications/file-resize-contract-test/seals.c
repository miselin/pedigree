#define _GNU_SOURCE
#include <fcntl.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

int fr_seals(void) {
  int failed = 0, duplicate = -1;
  struct fr_file file = {.fd = -1};
  const size_t page = fr_page, length = 4 * page;
  unsigned char *shared = MAP_FAILED, *private = MAP_FAILED;
  CHECK(!fr_create(&file, FR_MEMFD) && (duplicate = dup(file.fd)) >= 0);
  shared = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
  private = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_PRIVATE, duplicate, page);
  CHECK(shared != MAP_FAILED && private != MAP_FAILED);
  CHECK(fr_matches(shared, 0, length, 0) && fr_matches(private, page, 3 * page, 0));
  private[23] = 0xa1;
  CHECK(!fr_resident(shared, 4, 15, "sealed shared before rejection") &&
        !fr_resident(private, 3, 7, "sealed private before rejection"));
  CHECK(!fcntl(file.fd, F_ADD_SEALS, F_SEAL_SHRINK));
  const off_t position = 3 * page + 91;
  CHECK(lseek(duplicate, position, SEEK_SET) == position);
  const size_t sizes[] = {page + 137, 0};
  for (unsigned n = 0; n < sizeof(sizes) / sizeof(sizes[0]); ++n) {
    CHECK(ftruncate(duplicate, sizes[n]) == -1 && errno == EPERM);
    CHECK(!fr_resident(shared, 4, 15, "sealed shared after rejection") &&
          !fr_resident(private, 3, 7, "sealed private after rejection"));
    CHECK(!fr_size(file.fd, length) && !fr_size(duplicate, length));
    CHECK(fcntl(file.fd, F_GET_SEALS) == F_SEAL_SHRINK);
    CHECK(lseek(file.fd, 0, SEEK_CUR) == position);
    CHECK(fr_matches(shared, 0, length, 0) && private[23] == 0xa1 &&
          fr_matches(private, page, 23, 0) &&
          fr_matches(private + 24, page + 24, 3 * page - 24, 0));
    CHECK(!fr_contents(file.fd, 0, length, 0));
  }
out:
  if (private != MAP_FAILED)
    munmap(private, 3 * page);
  if (shared != MAP_FAILED)
    munmap(shared, length);
  if (duplicate >= 0)
    close(duplicate);
  fr_close(&file);
  return failed;
}
