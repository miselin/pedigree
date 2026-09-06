#define _GNU_SOURCE
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/stat.h>

static int anonymous_residency(void) {
  int failed = 0;
  const size_t p = vm_page;
  unsigned char* area =
      mmap(NULL, 4 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char* output =
      mmap(NULL, 2 * p, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  unsigned char vector[6];
  CHECK(area != MAP_FAILED && output != MAP_FAILED);
  memset(vector, 0xa5, sizeof(vector));
  CHECK(mincore(area, 4 * p - 1, vector + 1) == 0 && vector[0] == 0xa5 && vector[5] == 0xa5);
  CHECK(vm_uniform(vector + 1, 4, 0));
  CHECK(mincore(area, 4 * p, vector + 1) == 0 && vm_uniform(vector + 1, 4, 0));
  area[p] = 0xb1;
  CHECK(mprotect(area + p, 2 * p, PROT_NONE) == 0);
  CHECK(mincore(area, 4 * p, vector) == 0 && vector[0] == 0 && vector[1] == 1 && vector[2] == 0 &&
        vector[3] == 0);
  CHECK(area[0] == 0);
  CHECK(mincore(area, 4 * p, vector) == 0 && vector[0] == 1 && vector[1] == 1 && vector[2] == 0 &&
        vector[3] == 0);
  CHECK(mprotect(area + p, p, PROT_READ | PROT_WRITE) == 0 && area[p] == 0xb1);
  CHECK(mincore(area + 1, p, vector) == -1 && errno == EINVAL);
  CHECK(mprotect(output + p, p, PROT_NONE) == 0);
  CHECK(mincore(area, p, output + p) == -1 && errno == EFAULT);
  output[p - 1] = 0xa5;
  CHECK(mincore(area, 2 * p, output + p - 1) == -1 && errno == EFAULT && output[p - 1] == 0xa5);
  CHECK(mincore(area, 0, output + p) == 0);
  CHECK(mincore(area, 4 * p, vector) == 0 && vector[2] == 0 && vector[3] == 0);
  CHECK(munmap(area + 3 * p, p) == 0);
  CHECK(mincore(area, 4 * p, vector) == -1 && errno == ENOMEM);
  CHECK(mincore(area + 3 * p, p, vector) == -1 && errno == ENOMEM);
out:
  if (area != MAP_FAILED)
    munmap(area, 4 * p);
  if (output != MAP_FAILED)
    munmap(output, 2 * p);
  return failed;
}

static int file_residency(void) {
  int failed = 0, fd = -1;
  const size_t p = vm_page;
  pid_t child = -1;
  unsigned char *area = MAP_FAILED, *alias = MAP_FAILED;
  unsigned char vector[3], byte;
  struct stat metadata;
  CHECK((fd = vm_file(1, NULL)) >= 0);
  CHECK(fchmod(fd, 0600) == 0);
  CHECK(fstat(fd, &metadata) == 0 && (metadata.st_mode & 0777) == 0600);
  CHECK((area = mmap(NULL, 3 * p, PROT_NONE, MAP_PRIVATE, fd, 0)) != MAP_FAILED);
  CHECK((alias = mmap(NULL, p, PROT_READ, MAP_SHARED, fd, 0)) != MAP_FAILED);
  CHECK(pread(fd, &byte, 1, 0) == 1 && byte == 0x20);
  CHECK(mincore(area, 3 * p, vector) == 0 && vector[0] == 1 && vector[1] == 0 && vector[2] == 0);
  CHECK(alias[0] == 0x20);
  CHECK(madvise(alias, p, MADV_DONTNEED) == 0);
  CHECK(mincore(alias, p, vector) == 0 && vector[0] == 1);
  CHECK(mincore(area, 3 * p, vector) == 0 && vector[0] == 1 && vector[1] == 0 && vector[2] == 0);
  if (geteuid() == 0) {
    CHECK((child = fork()) >= 0);
    if (!child) {
      alarm(3);
      if (setgid(65534) || setuid(65534))
        _exit(10);
      errno = EDOM;
      const int before = errno;
      const int result = mincore(area, 3 * p, vector);
      const int after = errno;
      if (result || after != EDOM || !vm_uniform(vector, 3, 1)) {
        fprintf(stderr,
                "VM-EXPANSION-CONTRACT: residency masked mincore=%d errno=%d->%d "
                "uid=%lu euid=%lu gid=%lu egid=%lu vector=%u,%u,%u\n",
                result, before, after, (unsigned long)getuid(), (unsigned long)geteuid(),
                (unsigned long)getgid(), (unsigned long)getegid(), (unsigned)vector[0],
                (unsigned)vector[1], (unsigned)vector[2]);
        _exit(11);
      }
      _exit(0);
    }
    int status = vm_reap(child, 4000);
    child = -1;
    CHECK(status == 0);
    CHECK(mincore(area, 3 * p, vector) == 0 && vector[1] == 0 && vector[2] == 0);
  } else {
    puts("VM-EXPANSION-CONTRACT: SKIP residency credential masking requires root");
  }
out:
  if (child > 0) {
    kill(child, SIGKILL);
    vm_reap(child, 1000);
  }
  if (fd >= 0)
    close(fd);
  if (area != MAP_FAILED)
    munmap(area, 3 * p);
  if (alias != MAP_FAILED)
    munmap(alias, p);
  return failed;
}

int vm_test_residency(void) {
  return anonymous_residency() || file_residency();
}
