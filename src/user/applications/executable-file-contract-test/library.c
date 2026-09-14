/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <dlfcn.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "contract.h"
#include <sys/mman.h>
#include <sys/stat.h>

int ef_load(const char* path, int version) {
  int failed = 0;
  void* library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!library)
    fprintf(stderr, "EXECUTABLE-FILE-CONTRACT: dlopen %s: %s\n", path, dlerror());
  CHECK(library);
  int (*warm)(void) = (int (*)(void))dlsym(library, "executable_fixture_warm");
  int (*cold)(void) = (int (*)(void))dlsym(library, "executable_fixture_cold");
  CHECK(warm && cold && warm() == version && cold() == version);
out:
  if (library)
    dlclose(library);
  return failed;
}

int ef_library(void) {
  int failed = 0, fd = -1, probe = -1;
  char path[PATH_MAX], staged[PATH_MAX];
  void* library = NULL;
  pid_t child = -1;
  snprintf(path, sizeof(path), "%s/current.so", ef_directory);
  snprintf(staged, sizeof(staged), "%s/staged.so", ef_directory);
  CHECK(!ef_copy("/libraries/libexecutable-file-fixture-v1.so", path));
  CHECK((fd = open(path, O_RDWR | O_CLOEXEC)) >= 0);
  struct stat before, after;
  CHECK(!fstat(fd, &before));
  library = dlopen(path, RTLD_NOW | RTLD_LOCAL);
  if (!library)
    fprintf(stderr, "EXECUTABLE-FILE-CONTRACT: dlopen %s: %s\n", path, dlerror());
  CHECK(library);
  int (*warm)(void) = (int (*)(void))dlsym(library, "executable_fixture_warm");
  int (*cold)(void) = (int (*)(void))dlsym(library, "executable_fixture_cold");
  const volatile unsigned char* data = dlsym(library, "executable_fixture_data");
  CHECK(warm && cold && data && warm() == 1);
  CHECK(pwrite(fd, "x", 1, 0) == -1 && errno == ETXTBSY);
  CHECK(ftruncate(fd, 0) == -1 && errno == ETXTBSY);
  probe = open(path, O_WRONLY | O_TRUNC);
  CHECK(probe == -1 && errno == ETXTBSY);
  CHECK(!fstat(fd, &after) && before.st_size == after.st_size);
  char magic[4];
  CHECK(pread(fd, magic, sizeof(magic), 0) == sizeof(magic) && !memcmp(magic, "\177ELF", 4));
  CHECK(!ef_copy("/libraries/libexecutable-file-fixture-v2.so", staged));
  uintptr_t code_page = (uintptr_t)cold / ef_page * ef_page;
  uintptr_t data_page = (uintptr_t)(data + 8192) / ef_page * ef_page;
  CHECK(code_page != (uintptr_t)warm / ef_page * ef_page);
  // Retire process mappings so the old inode must satisfy faults after replacement.
  CHECK(!madvise((void*)code_page, ef_page, MADV_DONTNEED));
  CHECK(!madvise((void*)data_page, ef_page, MADV_DONTNEED));
  CHECK(!rename(staged, path));
  CHECK(!stat(path, &after) && before.st_ino != after.st_ino);
  CHECK(cold() == 1 && data[8192] == 1 && warm() == 1);
  CHECK((child = fork()) >= 0);
  if (!child) {
    execl(ef_self, ef_self, "--load", path, "2", NULL);
    _exit(80);
  }
  int status = ef_reap(child);
  child = -1;
  CHECK(!status);
out:
  if (child > 0) {
    kill(child, SIGKILL);
    ef_reap(child);
  }
  if (library)
    dlclose(library);
  if (fd >= 0)
    close(fd);
  if (probe >= 0)
    close(probe);
  unlink(staged);
  unlink(path);
  return failed;
}
