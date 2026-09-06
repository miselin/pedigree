/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/stat.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define IMAGE_LIMIT (1024U * 1024U)
static const char module_name[] = "init-module-contract";
static int loaded;

static long load_module(const void* image, size_t size, const char* parameters) {
  return syscall(SYS_init_module, image, size, parameters);
}
static long unload_module(void) {
  return syscall(SYS_delete_module, module_name, O_NONBLOCK);
}
static int load_error(const void* image, size_t size, const char* parameters, int expected,
                      const char* detail) {
  errno = 0;
  long result = load_module(image, size, parameters);
  if (result != -1 || errno != expected) {
    fprintf(stderr, "INIT-MODULE-CONTRACT: FAIL %s result=%ld errno=%d expected=%d\n", detail,
            result, errno, expected);
    if (!result)
      loaded = 1;
    return 1;
  }
  return 0;
}
static int unload_error(int expected, const char* detail) {
  errno = 0;
  long result = unload_module();
  if (result != -1 || errno != expected) {
    fprintf(stderr, "INIT-MODULE-CONTRACT: FAIL %s result=%ld errno=%d expected=%d\n", detail,
            result, errno, expected);
    return 1;
  }
  return 0;
}
static unsigned char* read_image(const char* path, size_t* size) {
  int fd = open(path, O_RDONLY);
  struct stat info;
  if (fd < 0)
    return NULL;
  if (fstat(fd, &info) || info.st_size <= 0 || info.st_size > IMAGE_LIMIT) {
    close(fd);
    return NULL;
  }
  *size = (size_t)info.st_size;
  unsigned char* image = malloc(*size);
  if (!image) {
    close(fd);
    return NULL;
  }
  size_t copied = 0;
  while (copied < *size) {
    ssize_t amount = read(fd, image + copied, *size - copied);
    if (amount < 0 && errno == EINTR)
      continue;
    if (amount <= 0) {
      free(image);
      close(fd);
      return NULL;
    }
    copied += (size_t)amount;
  }
  close(fd);
  return image;
}
static int credentials(const unsigned char* image, size_t size) {
  pid_t child = fork();
  if (!child) {
    if (setuid(65534))
      _exit(2);
    _exit(load_error(image, size, "", EPERM, "unprivileged load") ||
          unload_error(EPERM, "unprivileged unload"));
  }
  int status = 0;
  pid_t waited;
  do {
    waited = child < 0 ? -1 : waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  return child < 0 || waited != child || !WIFEXITED(status) || WEXITSTATUS(status);
}
static int run(unsigned char* image, size_t size) {
  if (unload_error(ENOENT, "fixture unexpectedly boot-loaded") ||
      load_error(NULL, size, "", EFAULT, "null input") ||
      load_error(image, size, NULL, EFAULT, "null parameters") ||
      load_error(image, size, "unsupported=1", EOPNOTSUPP, "nonempty parameters") ||
      load_error(image, 0, "", ENOEXEC, "empty image") ||
      load_error(image, IMAGE_LIMIT + 1, "", ENOEXEC, "image bound") ||
      load_error(image, 16, "", ENOEXEC, "truncated header"))
    return 1;
  unsigned char magic = image[0];
  image[0] = 0;
  int invalid = load_error(image, size, "", ENOEXEC, "invalid ELF magic");
  image[0] = magic;
  if (invalid)
    return 1;
  long page = sysconf(_SC_PAGESIZE);
  if (page <= 0)
    return 1;
  unsigned char* range =
      mmap(NULL, (size_t)page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (range == MAP_FAILED)
    return 1;
  memcpy(range + page - 8, image, 8);
  int copied = mprotect(range + page, (size_t)page, PROT_NONE) ||
               load_error(range + page - 8, 16, "", EFAULT, "inaccessible input tail");
  int unmapped = munmap(range, (size_t)page * 2);
  if (copied || unmapped)
    return 1;
  puts("INIT-MODULE-CONTRACT: PASS input-copy-policy");

  errno = 0;
  if (load_module(image, size, "")) {
    fprintf(stderr, "INIT-MODULE-CONTRACT: FAIL load errno=%d\n", errno);
    return 1;
  }
  loaded = 1;
  if (errno || load_error(image, size, "", EEXIST, "duplicate live module"))
    return 1;
  puts("INIT-MODULE-CONTRACT: PASS load-duplicate");
  if (credentials(image, size))
    return 1;
  puts("INIT-MODULE-CONTRACT: PASS credentials");
  if (unload_module())
    return 1;
  loaded = 0;
  if (unload_error(ENOENT, "repeated unload"))
    return 1;
  puts("INIT-MODULE-CONTRACT: PASS unload");

  if (load_module(image, size, ""))
    return 1;
  loaded = 1;
  if (unload_module())
    return 1;
  loaded = 0;
  puts("INIT-MODULE-CONTRACT: PASS reload");
  return 0;
}
int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  alarm(60);
  puts("INIT-MODULE-CONTRACT: BEGIN");
  const char* path = argc == 2 ? argv[1] : "/tests/init-module-contract-fixture.o";
  size_t size = 0;
  unsigned char* image = geteuid() == 0 && argc <= 2 ? read_image(path, &size) : NULL;
  if (!image) {
    fprintf(stderr, "INIT-MODULE-CONTRACT: FAIL requires root and readable fixture %s\n", path);
    puts("INIT-MODULE-CONTRACT: END FAIL");
    return 1;
  }
  int failed = run(image, size);
  if (loaded) {
    if (unload_module())
      fprintf(stderr, "INIT-MODULE-CONTRACT: cleanup unload failed errno=%d\n", errno);
  }
  free(image);
  puts(failed ? "INIT-MODULE-CONTRACT: END FAIL" : "INIT-MODULE-CONTRACT: END PASS");
  return failed;
}
