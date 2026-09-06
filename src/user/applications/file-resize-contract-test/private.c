#define _GNU_SOURCE
#include <signal.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>

static int private_child(int fd, unsigned char* private, unsigned char* borrowed, int ready,
                         int gate) {
  int failed = 0;
  const size_t page = fr_page, length = 4 * page, cut = page + 137;
  unsigned char* independent = mmap(NULL, 3 * page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, page);
  CHECK(independent != MAP_FAILED && private[23] == 0xc1 && private[2 * page + 19] == 0xc2);
  private[23] = 0xd1;
  private[2 * page + 19] = 0xd2;
  CHECK(fr_matches(independent, page, 3 * page, 0) && fr_matches(borrowed, page, 3 * page, 0));
  CHECK(!fr_resident(private, 4, 15, "child private before shrink") &&
        !fr_resident(independent, 3, 7, "independent child alias before shrink"));
  CHECK(!fr_send(ready, 'r') && !fr_receive(gate, 's'));

  CHECK(!fr_resident(private, 4, 3, "child private after shrink") &&
        !fr_resident(borrowed, 3, 1, "inherited child alias after shrink") &&
        !fr_resident(independent, 3, 1, "independent child alias after shrink"));
  CHECK(private[23] == 0xd1 && fr_matches(private, 0, 23, 0) &&
        fr_matches(private + 24, 24, cut - 24, 0));
  CHECK(fr_matches(private + cut, cut, 2 * page - cut, 1));
  CHECK(fr_matches(independent, page, 137, 0) && fr_matches(independent + 137, cut, page - 137, 1));
  CHECK(fr_matches(borrowed, page, 137, 0) && fr_matches(borrowed + 137, cut, page - 137, 1));
  CHECK(!fr_fault(private + 2 * page) && !fr_fault(independent + page));
  CHECK(!fr_send(ready, 's') && !fr_receive(gate, 'g'));

  CHECK(!fr_resident(private, 2, 3, "child private prefix after regrowth") &&
        !fr_resident(independent, 1, 1, "independent child prefix after regrowth"));
  CHECK(private[23] == 0xd1 && fr_matches(private + cut, cut, length - cut, 1));
  CHECK(fr_matches(independent + 137, cut, length - cut, 1) &&
        fr_matches(borrowed + 137, cut, length - cut, 1));
  independent[257] = 0xe7;
  CHECK(private[page + 257] == 0xe7 && borrowed[257] == 0xe7);
  CHECK(!fr_send(ready, 'g'));
out:
  if (independent != MAP_FAILED)
    munmap(independent, 3 * page);
  return failed;
}

int fr_private(int backend) {
  int failed = 0, ready[2] = {-1, -1}, gate[2] = {-1, -1};
  struct fr_file file = {.fd = -1};
  const size_t page = fr_page, length = 4 * page, cut = page + 137;
  pid_t child = -1;
  unsigned char *private = MAP_FAILED, *borrowed = MAP_FAILED;
  unsigned char byte;
  CHECK(!fr_create(&file, backend));
  private = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE, file.fd, 0);
  borrowed = mmap(NULL, 3 * page, PROT_READ, MAP_PRIVATE, file.fd, page);
  CHECK(private != MAP_FAILED && borrowed != MAP_FAILED);
  CHECK(fr_matches(private, 0, length, 0) && fr_matches(borrowed, page, 3 * page, 0));
  private[23] = 0xc1;
  private[2 * page + 19] = 0xc2;
  CHECK(!fr_contents(file.fd, 0, length, 0));
  CHECK(!pipe(ready) && !pipe(gate) && (child = fork()) >= 0);
  if (!child) {
    alarm(15);
    close(ready[0]);
    close(gate[1]);
    int result = private_child(file.fd, private, borrowed, ready[1], gate[0]);
    fflush(stderr);
    _exit(result ? 1 : 0);
  }
  CHECK(!close(ready[1]) && !close(gate[0]));
  ready[1] = gate[0] = -1;
  CHECK(!fr_receive(ready[0], 'r'));
  CHECK(private[23] == 0xc1 && private[2 * page + 19] == 0xc2);
  CHECK(!fr_resident(private, 4, 15, "parent private before shrink"));
  CHECK(!ftruncate(file.fd, cut));
  CHECK(!fr_resident(private, 4, 3, "parent private after shrink") &&
        !fr_resident(borrowed, 3, 1, "parent borrowed after shrink"));
  CHECK(private[23] == 0xc1 && fr_matches(private, 0, 23, 0) &&
        fr_matches(private + 24, 24, cut - 24, 0));
  CHECK(fr_matches(private + cut, cut, 2 * page - cut, 1) &&
        fr_matches(borrowed + 137, cut, page - 137, 1));
  CHECK(!fr_send(gate[1], 's') && !fr_receive(ready[0], 's'));
  CHECK(!fr_fault(private + 2 * page));
  CHECK(!ftruncate(file.fd, length));
  CHECK(!fr_resident(private, 2, 3, "parent private prefix after regrowth"));
  CHECK(private[23] == 0xc1 && fr_matches(private + cut, cut, length - cut, 1));
  CHECK(!fr_send(gate[1], 'g') && !fr_receive(ready[0], 'g'));
  CHECK(private[23] == 0xc1 && private[page + 257] == 0xe7 && borrowed[257] == 0xe7);
  CHECK(pread(file.fd, &byte, 1, page + 257) == 1 && byte == 0xe7);
  int status = fr_reap(child, 5000);
  child = -1;
  CHECK(status == 0);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    fr_reap(child, 1000);
  }
  for (int n = 0; n < 2; ++n) {
    if (ready[n] >= 0)
      close(ready[n]);
    if (gate[n] >= 0)
      close(gate[n]);
  }
  if (borrowed != MAP_FAILED)
    munmap(borrowed, 3 * page);
  if (private != MAP_FAILED)
    munmap(private, length);
  fr_close(&file);
  return failed;
}
