#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

static int child_layout(unsigned char* mapping, int fd, int command, int status) {
  int failed = 0;
  const size_t page = rp_page;
  CHECK(!close(fd));
  CHECK(rp_matches(mapping, 0, page, 0) && rp_matches(mapping + page, 5 * page, page, 0) &&
        rp_matches(mapping + 2 * page, 2 * page, 2 * page, 0));
  CHECK(!rp_send(status, 'R') && !rp_receive(command, 'G'));
  CHECK(mapping[2 * page + 53] == 0xd4);
  mapping[2 * page + 53] = rp_pattern(2 * page + 53);
  CHECK(rp_matches(mapping, 0, page, 0) && rp_matches(mapping + page, 5 * page, page, 0) &&
        rp_matches(mapping + 2 * page, 2 * page, 2 * page, 0));
  CHECK(!remap_file_pages(mapping + 2 * page, page, 0, 7, 0));
  CHECK(rp_matches(mapping + 2 * page, 7 * page, page, 0));
  mapping[page + 33] = 0xe6;
  CHECK(!rp_send(status, 'D') && !rp_receive(command, 'F'));
out:
  return failed;
}

static int last_descriptor(int backend) {
  int failed = 0;
  struct rp_file file = {.fd = -1};
  int command[2] = {-1, -1}, status[2] = {-1, -1};
  pid_t child = -1;
  const size_t page = rp_page;
  unsigned char *mapping = MAP_FAILED, *alias = MAP_FAILED, *moved = MAP_FAILED;
  CHECK(!rp_create(&file, backend));
  mapping = mmap(NULL, 4 * page, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 0);
  alias = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, file.fd, 5 * page);
  CHECK(mapping != MAP_FAILED && alias != MAP_FAILED);
  CHECK(!remap_file_pages(mapping + page, page, 0, 5, 0));
  CHECK(!pipe(command) && !pipe(status));
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(15);
    close(command[1]);
    close(status[0]);
    int result = child_layout(mapping, file.fd, command[0], status[1]);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  close(command[0]);
  command[0] = -1;
  close(status[1]);
  status[1] = -1;
  CHECK(!rp_receive(status[0], 'R'));
  if (file.path[0]) {
    CHECK(!unlink(file.path));
    file.path[0] = 0;
  }
  CHECK(!close(file.fd));
  file.fd = -1;
  CHECK(!remap_file_pages(mapping, 4 * page, 0, 1, 0));
  CHECK(rp_matches(mapping, page, 4 * page, 0));
  mapping[page + 53] = 0xd4;
  CHECK(!rp_send(command[1], 'G') && !rp_receive(status[0], 'D'));
  CHECK(alias[33] == 0xe6 && rp_matches(mapping, page, 4 * page, 0));
  alias[33] = rp_pattern(5 * page + 33);
  CHECK(!rp_send(command[1], 'F'));
  int child_status = rp_reap(child, 5000);
  child = -1;
  CHECK(child_status == 0);

  CHECK(!mprotect(mapping + page, page, PROT_READ));
  moved = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  CHECK(moved != MAP_FAILED);
  CHECK(mremap(mapping + page, page, page, MREMAP_MAYMOVE | MREMAP_FIXED, moved) == moved);
  CHECK(rp_matches(moved, 2 * page, page, 0));
  CHECK(!remap_file_pages(moved, page, 0, 6, MAP_NONBLOCK));
  CHECK(!madvise(moved, page, MADV_DONTNEED) && rp_matches(moved, 6 * page, page, 0));
  CHECK(!mprotect(moved, page, PROT_READ | PROT_WRITE));
  CHECK(!remap_file_pages(mapping, page, 0, 6, 0));
  moved[55] = 0xba;
  CHECK(mapping[55] == 0xba && rp_matches(mapping + 2 * page, 3 * page, 2 * page, 0));
out:
  if (child > 0) {
    kill(child, SIGKILL);
    rp_reap(child, 5000);
  }
  for (int n = 0; n < 2; ++n) {
    if (command[n] >= 0)
      close(command[n]);
    if (status[n] >= 0)
      close(status[n]);
  }
  if (moved != MAP_FAILED)
    munmap(moved, page);
  if (alias != MAP_FAILED)
    munmap(alias, page);
  if (mapping != MAP_FAILED)
    munmap(mapping, 4 * page);
  rp_close(&file);
  if (failed)
    fprintf(stderr, "REMAP-FILE-PAGES-CONTRACT: lifetime backend=%d\n", backend);
  return failed;
}

int rp_lifetime(void) {
  for (int backend = RP_MEMFD; backend <= RP_EXT2; ++backend) {
    if (last_descriptor(backend))
      return 1;
  }
  return 0;
}
