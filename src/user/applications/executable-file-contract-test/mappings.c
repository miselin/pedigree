/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <fcntl.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/stat.h>

int ef_mutations(void) {
  int failed = 0, fd = -1, other = -1, probe = -1;
  char path[PATH_MAX] = {0}, alias[PATH_MAX] = {0};
  unsigned char* map = MAP_FAILED;
  CHECK((fd = ef_create("mutations", path)) >= 0);
  if (ef_ext2) {
    snprintf(alias, sizeof(alias), "%s/hardlink", ef_directory);
    CHECK(!link(path, alias));
  }
  CHECK((other = open(ef_ext2 ? alias : path, O_RDWR | O_CLOEXEC)) >= 0);
  map = mmap(NULL, 3 * ef_page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
  CHECK(map != MAP_FAILED && map[0] == 0x31);
  CHECK(lseek(fd, 17, SEEK_SET) == 17);
  CHECK(write(fd, "x", 1) == -1 && errno == ETXTBSY);
  CHECK(lseek(fd, 0, SEEK_CUR) == 17);
  CHECK(pwrite(other, "x", 1, ef_page + 3) == -1 && errno == ETXTBSY);
  CHECK(ftruncate(other, 0) == -1 && errno == ETXTBSY);
  CHECK(ftruncate(fd, 4 * ef_page) == -1 && errno == ETXTBSY);
  CHECK(fallocate(other, 0, 0, 4 * ef_page) == -1 && errno == ETXTBSY);
  probe = open(ef_ext2 ? alias : path, O_WRONLY | O_TRUNC);
  CHECK(probe == -1 && errno == ETXTBSY);
  CHECK((probe = open(path, O_WRONLY | O_CLOEXEC)) >= 0);
  CHECK(write(probe, "x", 1) == -1 && errno == ETXTBSY);
  struct stat metadata;
  unsigned char bytes[32];
  CHECK(!fstat(other, &metadata) && metadata.st_size == (off_t)(3 * ef_page));
  CHECK(pread(other, bytes, sizeof(bytes), ef_page) == sizeof(bytes));
  for (size_t n = 0; n < sizeof(bytes); ++n)
    CHECK(bytes[n] == 0x31);
  CHECK(!munmap(map, 3 * ef_page));
  map = MAP_FAILED;
  CHECK(pwrite(other, "y", 1, 0) == 1 && !ftruncate(fd, ef_page));
out:
  if (map != MAP_FAILED)
    munmap(map, 3 * ef_page);
  if (probe >= 0)
    close(probe);
  if (other >= 0)
    close(other);
  if (fd >= 0)
    close(fd);
  if (*alias)
    unlink(alias);
  if (*path)
    unlink(path);
  return failed;
}

int ef_lifetime(void) {
  int failed = 0, fd = -1, ready[2] = {-1, -1}, gate[2] = {-1, -1};
  char path[PATH_MAX] = {0};
  unsigned char* map = MAP_FAILED;
  pid_t child = -1;
  CHECK((fd = ef_create("lifetime", path)) >= 0);
  map = mmap(NULL, 3 * ef_page, PROT_READ, MAP_PRIVATE, fd, 0);
  CHECK(map != MAP_FAILED);
  CHECK(!mprotect(map, 3 * ef_page, PROT_READ | PROT_EXEC));
  CHECK(!munmap(map + ef_page, ef_page));
  CHECK(!mprotect(map, ef_page, PROT_READ));
  CHECK(!pipe(ready) && !pipe(gate));
  CHECK((child = fork()) >= 0);
  if (!child) {
    alarm(10);
    close(ready[0]);
    close(gate[1]);
    if (ef_send(ready[1], 'r') || ef_receive(gate[0], 'g') || map[2 * ef_page] != 0x31 ||
        munmap(map, ef_page) || mprotect(map + 2 * ef_page, ef_page, PROT_READ) ||
        munmap(map + 2 * ef_page, ef_page))
      _exit(1);
    _exit(0);
  }
  close(ready[1]);
  ready[1] = -1;
  close(gate[0]);
  gate[0] = -1;
  CHECK(!ef_receive(ready[0], 'r'));
  CHECK(!munmap(map, ef_page));
  CHECK(pwrite(fd, "x", 1, 0) == -1 && errno == ETXTBSY);
  CHECK(!munmap(map + 2 * ef_page, ef_page));
  map = MAP_FAILED;
  CHECK(pwrite(fd, "x", 1, 0) == -1 && errno == ETXTBSY);
  CHECK(!ef_send(gate[1], 'g'));
  int status = ef_reap(child);
  child = -1;
  CHECK(!status && pwrite(fd, "x", 1, 0) == 1);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    ef_reap(child);
  }
  if (map != MAP_FAILED) {
    munmap(map, ef_page);
    munmap(map + 2 * ef_page, ef_page);
  }
  for (int n = 0; n < 2; ++n) {
    if (ready[n] >= 0)
      close(ready[n]);
    if (gate[n] >= 0)
      close(gate[n]);
  }
  if (fd >= 0)
    close(fd);
  if (*path)
    unlink(path);
  return failed;
}

int ef_conflicts(void) {
  int failed = 0, fd = -1, readonly = -1;
  char path[PATH_MAX] = {0};
  void *shared = MAP_FAILED, *private = MAP_FAILED, *probe = MAP_FAILED;
  CHECK((fd = ef_create("conflicts", path)) >= 0);
  CHECK((readonly = open(path, O_RDONLY | O_CLOEXEC)) >= 0);
  shared = mmap(NULL, ef_page, PROT_READ, MAP_SHARED, fd, 0);
  CHECK(shared != MAP_FAILED);
  private = mmap(NULL, ef_page, PROT_READ, MAP_PRIVATE, fd, 0);
  CHECK(private != MAP_FAILED);
  probe = mmap(NULL, ef_page, PROT_READ | PROT_EXEC, MAP_PRIVATE, fd, 0);
  CHECK(probe == MAP_FAILED && errno == ETXTBSY);
  CHECK(mprotect(private, ef_page, PROT_READ | PROT_EXEC) == -1 && errno == ETXTBSY);
  CHECK(pwrite(fd, "x", 1, 0) == 1);
  CHECK(!mprotect(shared, ef_page, PROT_READ | PROT_WRITE));
  ((volatile unsigned char*)shared)[0] = 0x32;
  CHECK(!munmap(shared, ef_page));
  shared = MAP_FAILED;
  CHECK(!mprotect(private, ef_page, PROT_READ | PROT_EXEC));
  probe = mmap(NULL, ef_page, PROT_READ, MAP_SHARED, fd, 0);
  CHECK(probe == MAP_FAILED && errno == ETXTBSY);
  probe = mmap(NULL, ef_page, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
  CHECK(probe == MAP_FAILED && errno == ETXTBSY);
  shared = mmap(NULL, ef_page, PROT_READ, MAP_SHARED, readonly, 0);
  CHECK(shared != MAP_FAILED);
  CHECK(mprotect(shared, ef_page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES);
  CHECK(!munmap(shared, ef_page) && !munmap(private, ef_page));
  shared = private = MAP_FAILED;
  CHECK(pwrite(fd, "x", 1, 0) == 1);
  probe = mmap(NULL, ef_page, PROT_READ | PROT_WRITE | PROT_EXEC, MAP_SHARED, fd, 0);
  CHECK(probe != MAP_FAILED);
  ((volatile unsigned char*)probe)[0] = 0x34;
  CHECK(pwrite(fd, "x", 1, 0) == 1);
out:
  if (probe != MAP_FAILED)
    munmap(probe, ef_page);
  if (shared != MAP_FAILED)
    munmap(shared, ef_page);
  if (private != MAP_FAILED)
    munmap(private, ef_page);
  if (readonly >= 0)
    close(readonly);
  if (fd >= 0)
    close(fd);
  if (*path)
    unlink(path);
  return failed;
}

int ef_ordinary(void) {
  int failed = 0, fd = -1;
  char path[PATH_MAX] = {0};
  unsigned char* map = MAP_FAILED;
  CHECK((fd = ef_create("ordinary", path)) >= 0);
  map = mmap(NULL, 3 * ef_page, PROT_READ | PROT_WRITE, MAP_PRIVATE, fd, 0);
  CHECK(map != MAP_FAILED && map[0] == 0x31);
  map[0] = 0x43;
  CHECK(pwrite(fd, "z", 1, 1) == 1);
  unsigned char byte;
  CHECK(pread(fd, &byte, 1, 0) == 1 && byte == 0x31);
  CHECK(map[0] == 0x43);
  CHECK(!ftruncate(fd, ef_page) && !ftruncate(fd, 3 * ef_page));
  CHECK(map[2 * ef_page] == 0 && map[0] == 0x43);
out:
  if (map != MAP_FAILED)
    munmap(map, 3 * ef_page);
  if (fd >= 0)
    close(fd);
  if (*path)
    unlink(path);
  return failed;
}
