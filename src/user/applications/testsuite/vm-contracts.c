/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define _GNU_SOURCE

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/wait.h>

extern void fail(void) __attribute__((noreturn));

static void require(int condition, const char* operation) {
  if (!condition) {
    printf("VM-CONTRACT: FAIL %s errno=%d\n", operation, errno);
    fail();
  }
}

static void anonymous_ranges(size_t page) {
  unsigned char* memory =
      mmap(NULL, page * 4, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(memory != MAP_FAILED, "anonymous mapping");
  for (size_t i = 4; i; --i) {
    memory[(i - 1) * page] = (unsigned char)(0x40 + i);
    memory[i * page - 1] = (unsigned char)(0x60 + i);
  }
  require(munmap(memory, page) == 0, "remove reverse-faulted prefix");
  unsigned char* prefix = mmap(memory, page, PROT_READ | PROT_WRITE,
                               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  require(prefix == memory && prefix[0] == 0 && prefix[page - 1] == 0,
          "prefix removed every resident page");
  memory[0] = 0x41;
  memory[page - 1] = 0x61;
  require(mprotect(memory + page, page * 2, PROT_NONE) == 0, "protect middle none");
  require(mprotect(memory + page, page * 2, PROT_READ | PROT_WRITE) == 0,
          "restore middle protections");
  for (size_t i = 1; i <= 4; ++i) {
    require(memory[(i - 1) * page] == 0x40 + i && memory[i * page - 1] == 0x60 + i,
            "anonymous protection preserves contents");
  }
  require(munmap(memory + page, page) == 0, "create range hole");
  errno = 0;
  require(mprotect(memory, page * 4, PROT_READ) == -1 && errno == ENOMEM,
          "protection rejects uncovered range");
  memory[0] = 0xA1;
  memory[page * 2] = 0xA2;
  errno = 0;
  require(msync(memory, page * 4, MS_SYNC) == -1 && errno == ENOMEM,
          "sync rejects uncovered range");
  errno = 0;
  require(mprotect(memory, page, 0x40000000) == -1 && errno == EINVAL,
          "protection rejects unknown flags");
  // musl rounds SIZE_MAX to a zero-length request before entering the kernel.
  errno = 0;
  require(syscall(SYS_mprotect, memory, SIZE_MAX, PROT_NONE) == -1 && errno == ENOMEM &&
              memory[0] == 0xA1,
          "syscall length-rounding overflow preserves mapping");
  errno = 0;
  require(mprotect(memory, SIZE_MAX & ~(page - 1), PROT_NONE) == -1 && errno == ENOMEM &&
              memory[0] == 0xA1,
          "address-range overflow preserves mapping");
  require(munmap(memory, page * 4) == 0, "unmap range with hole");
  puts("VM-CONTRACT: PASS anonymous-ranges");
}

static void protected_fork(size_t page) {
  unsigned char* memory =
      mmap(NULL, page * 2, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(memory != MAP_FAILED, "fork mapping");
  memory[0] = 0x31;
  memory[page] = 0x32;
  require(mprotect(memory, page, PROT_READ) == 0 && mprotect(memory + page, page, PROT_NONE) == 0,
          "protect before fork");
  int ready[2], proceed[2];
  require(pipe(ready) == 0 && pipe(proceed) == 0, "fork barriers");
  pid_t child = fork();
  require(child >= 0, "fork protected mapping");
  if (!child) {
    close(ready[0]);
    close(proceed[1]);
    if (mprotect(memory, page * 2, PROT_READ | PROT_WRITE) != 0 || memory[0] != 0x31 ||
        memory[page] != 0x32)
      _exit(21);
    memory[0] = 0x51;
    memory[page] = 0x52;
    char byte = 'x';
    if (write(ready[1], &byte, 1) != 1 || read(proceed[0], &byte, 1) != 1 || memory[0] != 0x51 ||
        memory[page] != 0x52)
      _exit(22);
    _exit(0);
  }
  close(ready[1]);
  close(proceed[0]);
  char byte = 0;
  require(read(ready[0], &byte, 1) == 1, "child restored protected pages");
  require(mprotect(memory, page * 2, PROT_READ | PROT_WRITE) == 0 && memory[0] == 0x31 &&
              memory[page] == 0x32,
          "child writes preserve parent protected contents");
  memory[0] = 0x71;
  memory[page] = 0x72;
  require(write(proceed[1], &byte, 1) == 1, "release protected child");
  int status = 0;
  require(waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
          "parent writes preserve child contents");
  close(ready[0]);
  close(proceed[1]);
  require(munmap(memory, page * 2) == 0, "unmap protected fork mapping");
  puts("VM-CONTRACT: PASS protected-fork");
}

static void require_sparse(int condition, const char* operation, unsigned scenario,
                           const void* address) {
  if (!condition) {
    printf("VM-CONTRACT: FAIL sparse case=%u stage=%s address=%p errno=%d\n", scenario, operation,
           address, errno);
    fail();
  }
}

static void sparse_file_splits(int file, size_t page, const unsigned char* pattern) {
  const unsigned resident_masks[] = {4, 2, 5, 7};
  for (unsigned scenario = 0; scenario < 24; ++scenario) {
    unsigned mask = resident_masks[scenario / 6];
    unsigned reverse_split = (scenario / 3) % 2;
    unsigned removal = scenario % 3;
    unsigned char* arena =
        mmap(NULL, page * 7, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require_sparse(arena != MAP_FAILED, "guarded reservation", scenario, arena);
    arena[0] = 0x91;
    arena[page * 4] = 0x92;
    arena[page * 6] = 0x93;
    unsigned char* memory = arena + page;
    require_sparse(munmap(memory, page * 3) == 0, "file reservation hole", scenario, memory);
    require_sparse(
        mmap(memory, page * 3, PROT_READ, MAP_SHARED | MAP_FIXED_NOREPLACE, file, 0) == memory,
        "file placement", scenario, memory);
    for (size_t i = 3; i; --i) {
      size_t offset = (i - 1) * page;
      if (mask & (1U << (i - 1))) {
        require_sparse(memory[offset] == pattern[offset] &&
                           memory[offset + page - 1] == pattern[offset + page - 1],
                       "resident file contents", scenario, memory);
      }
    }
    require_sparse(mprotect(memory + (reverse_split ? 2 : 1) * page, (reverse_split ? 1 : 2) * page,
                            PROT_READ) == 0 &&
                       mprotect(memory + (reverse_split ? 1 : 2) * page, page, PROT_READ) == 0,
                   "protection split", scenario, memory);

    if (removal) {
      size_t first = removal == 1 ? 0 : page * 2;
      require_sparse(munmap(memory + first, page) == 0, "first fragment removal", scenario, memory);
    }
    // An unrelated hole separates the file fragments in allocator storage.
    require_sparse(munmap(arena + page * 5, page) == 0, "unrelated hole", scenario, memory);
    if (!removal) {
      require_sparse(munmap(memory, page * 3) == 0, "whole split removal", scenario, memory);
    } else {
      size_t last = removal == 1 ? page * 2 : 0;
      require_sparse(munmap(memory + last, page) == 0 && munmap(memory + page, page) == 0,
                     "remaining fragment removal", scenario, memory);
    }

    errno = 0;
    unsigned char* reclaimed = mmap(memory, page * 3, PROT_READ | PROT_WRITE,
                                    MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    if (reclaimed != memory) {
      printf("VM-CONTRACT: sparse case=%u requested=%p returned=%p errno=%d\n", scenario,
             (void*)memory, (void*)reclaimed, errno);
      require_sparse(0, "released reservation", scenario, memory);
    }
    for (size_t offset = 0; offset < page * 3; ++offset) {
      unsigned char value = reclaimed[offset];
      if (value) {
        printf("VM-CONTRACT: sparse case=%u offset=%zu value=%u\n", scenario, offset, value);
        require_sparse(0, "reclaimed zero contents", scenario, memory);
      }
    }
    require_sparse(arena[0] == 0x91 && arena[page * 4] == 0x92 && arena[page * 6] == 0x93,
                   "neighbor contents", scenario, memory);
    require_sparse(munmap(arena, page * 7) == 0, "guarded cleanup", scenario, arena);
  }
  puts("VM-CONTRACT: PASS sparse-splits cases=24");
}

static void file_mappings(size_t page) {
  char path[80];
  snprintf(path, sizeof(path), "/tmp/vm-contract-%d", getpid());
  int file = open(path, O_CREAT | O_TRUNC | O_RDWR, 0600);
  require(file >= 0, "create mapped file");
  unsigned char* pattern = malloc(page * 3);
  require(pattern != NULL, "file pattern allocation");
  for (size_t i = 0; i < page * 3; ++i)
    pattern[i] = (unsigned char)(1 + i % 251);
  require(write(file, pattern, page * 3) == (ssize_t)(page * 3), "populate mapped file");

  unsigned char* private = mmap(NULL, page + 17, PROT_READ | PROT_WRITE, MAP_PRIVATE, file, 0);
  require(private != MAP_FAILED, "private partial-length mapping");
  require(private[page + 25] == pattern[page + 25], "mapping covers final complete page");
  private[0] = 0xA1;
  private[page + 25] = 0xA2;
  require(mprotect(private, page * 2, PROT_NONE) == 0 &&
              mprotect(private, page * 2, PROT_READ | PROT_WRITE) == 0 && private[0] == 0xA1 &&
              private[page + 25] == 0xA2,
          "private file protection preserves modifications");
  require(msync(private, page * 2, MS_SYNC | MS_INVALIDATE) == 0 && private[0] == 0xA1 &&
              private[page + 25] == 0xA2,
          "invalidation preserves private modifications");
  unsigned char byte;
  require(pread(file, &byte, 1, 0) == 1 && byte == pattern[0], "private writes preserve file");
  require(munmap(private, page * 2) == 0, "unmap private file");

  unsigned char* sparse = mmap(NULL, page * 3, PROT_READ, MAP_SHARED, file, 0);
  require(sparse != MAP_FAILED && sparse[page * 2] == pattern[page * 2],
          "fault last sparse file page");
  require(mprotect(sparse + page, page * 2, PROT_READ) == 0 && munmap(sparse, page * 3) == 0,
          "unmap split with untouched prefix page");
  errno = 0;
  unsigned char* reclaimed = mmap(sparse, page * 3, PROT_READ | PROT_WRITE,
                                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
  if (reclaimed != sparse)
    printf("VM-CONTRACT: original sparse requested=%p returned=%p errno=%d\n", (void*)sparse,
           (void*)reclaimed, errno);
  require(reclaimed == sparse, "sparse split released address reservation");
  unsigned char reclaimed_value = reclaimed[page * 2];
  if (reclaimed_value)
    printf("VM-CONTRACT: original sparse address=%p offset=%zu value=%u\n", (void*)sparse, page * 2,
           reclaimed_value);
  require(reclaimed_value == 0, "sparse split released every resident file page");
  require(munmap(reclaimed, page * 3) == 0, "unmap reclaimed sparse range");
  sparse_file_splits(file, page, pattern);

  unsigned char* shared = mmap(NULL, page * 3, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
  require(shared != MAP_FAILED, "shared file mapping");
  shared[page * 2] = 0xD1;
  require(mprotect(shared + page, page, PROT_READ) == 0, "split with untouched file page");
  require(mprotect(shared + page * 2, page, PROT_NONE) == 0 &&
              msync(shared + page * 2, page, MS_SYNC) == 0 &&
              mprotect(shared + page * 2, page, PROT_READ) == 0 && shared[page * 2] == 0xD1,
          "sync after removing shared write access");
  require(pread(file, &byte, 1, (off_t)(page * 2)) == 1 && byte == 0xD1,
          "shared modifications reach backing view");
  errno = 0;
  require(msync(shared, page, MS_SYNC | MS_ASYNC) == -1 && errno == EINVAL,
          "sync rejects conflicting flags");
  require(munmap(shared, page * 3) == 0, "unmap sparse split file mapping");

  int readonly = open(path, O_RDONLY);
  require(readonly >= 0, "open read-only mapping descriptor");
  shared = mmap(NULL, page, PROT_READ, MAP_SHARED, readonly, 0);
  require(shared != MAP_FAILED && shared[0] == pattern[0], "read-only shared mapping");
  errno = 0;
  require(mprotect(shared, page, PROT_READ | PROT_WRITE) == -1 && errno == EACCES,
          "shared mapping remembers descriptor permissions");
  require(munmap(shared, page) == 0, "unmap read-only shared mapping");
  errno = 0;
  require(mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_SHARED, readonly, 0) == MAP_FAILED &&
              errno == EACCES,
          "shared mapping rejects read-only descriptor writes");
  private = mmap(NULL, page, PROT_READ | PROT_WRITE, MAP_PRIVATE, readonly, 0);
  require(private != MAP_FAILED, "read-only descriptor allows private copy");
  private[0] = 0xE1;
  require(pread(readonly, &byte, 1, 0) == 1 && byte == pattern[0], "private copy remains isolated");
  require(munmap(private, page) == 0, "unmap read-only descriptor private copy");
  close(readonly);
  int writeonly = open(path, O_WRONLY);
  require(writeonly >= 0, "open write-only mapping descriptor");
  errno = 0;
  require(mmap(NULL, page, PROT_NONE, MAP_PRIVATE, writeonly, 0) == MAP_FAILED && errno == EACCES,
          "file mapping requires readable descriptor");
  close(writeonly);
  close(file);
  require(unlink(path) == 0, "remove mapped file");
  free(pattern);
  puts("VM-CONTRACT: PASS file-mappings");
}

static volatile unsigned char* bus_address;

static void mapped_bus_handler(int signal, siginfo_t* info, void* context) {
  (void)context;
  _exit(signal == SIGBUS && info->si_code == BUS_ADRERR && info->si_addr == (void*)bus_address
            ? 0
            : 61);
}

struct shrink_child {
  pid_t pid;
  int proceed;
};

static struct shrink_child wait_for_shrink(unsigned char* address, int check_info) {
  int ready[2], proceed[2];
  require(pipe(ready) == 0 && pipe(proceed) == 0, "shrink barriers");
  pid_t child = fork();
  require(child >= 0, "fork mapped shrink observer");
  if (!child) {
    close(ready[0]);
    close(proceed[1]);
    bus_address = address;
    if (check_info) {
      struct sigaction action = {0};
      action.sa_sigaction = mapped_bus_handler;
      action.sa_flags = SA_SIGINFO;
      sigemptyset(&action.sa_mask);
      if (sigaction(SIGBUS, &action, NULL))
        _exit(62);
    } else {
      signal(SIGBUS, SIG_DFL);
    }
    volatile unsigned char before = *bus_address;
    (void)before;
    char byte = 'x';
    if (write(ready[1], &byte, 1) != 1 || read(proceed[0], &byte, 1) != 1)
      _exit(63);
    volatile unsigned char after = *bus_address;
    (void)after;
    _exit(64);
  }
  close(ready[1]);
  close(proceed[0]);
  char byte;
  require(read(ready[0], &byte, 1) == 1, "child has resident EOF page");
  close(ready[0]);
  return (struct shrink_child){child, proceed[1]};
}

static void reap_shrink_child(struct shrink_child child, int check_info) {
  char byte = 'x';
  require(write(child.proceed, &byte, 1) == 1, "release mapped shrink observer");
  close(child.proceed);
  int status = 0;
  require(waitpid(child.pid, &status, 0) == child.pid, "wait for mapped EOF signal");
  require(check_info ? (WIFEXITED(status) && WEXITSTATUS(status) == 0)
                     : (WIFSIGNALED(status) && WTERMSIG(status) == SIGBUS),
          check_info ? "shared EOF signal address and BUS_ADRERR" : "private EOF terminal SIGBUS");
}

static void mapped_shrink(size_t page) {
  char path[80];
  snprintf(path, sizeof(path), "/tmp/vm-shrink-%d", getpid());
  int file = open(path, O_CREAT | O_EXCL | O_RDWR, 0600);
  require(file >= 0, "create shrink file");
  unsigned char* bytes = malloc(page * 3);
  require(bytes != NULL, "shrink pattern");
  memset(bytes, 0x49, page * 3);
  require(write(file, bytes, page * 3) == (ssize_t)(page * 3), "populate shrink file");
  free(bytes);
  unsigned char* shared = mmap(NULL, page * 3, PROT_READ | PROT_WRITE, MAP_SHARED, file, 0);
  unsigned char* private = mmap(NULL, page * 3, PROT_READ | PROT_WRITE, MAP_PRIVATE, file, 0);
  unsigned char* offset = mmap(NULL, page, PROT_READ, MAP_SHARED, file, (off_t)page);
  require(shared != MAP_FAILED && private != MAP_FAILED && offset != MAP_FAILED, "shrink mappings");
  shared[0] = 0x51;
  shared[page + 37] = 0x52;
  private[page + 37] = 0xA5;
  private[page * 2] = 0xA6;
  require(offset[37] == 0x52, "offset alias before shrink");
  struct shrink_child shared_child = wait_for_shrink(shared + page * 2 + 11, 1);
  struct shrink_child private_child = wait_for_shrink(private + page * 2 + 13, 0);
  require(mprotect(shared + page, page, PROT_NONE) == 0, "inaccessible borrowed shrink page");
  require(ftruncate(file, (off_t)(page + 17)) == 0,
          "shrink with shared and private resident pages");
  reap_shrink_child(shared_child, 1);
  reap_shrink_child(private_child, 0);
  require(mprotect(shared + page, page, PROT_READ | PROT_WRITE) == 0, "restore shrunken tail");
  require(shared[0] == 0x51 && shared[page + 16] == 0x49 && offset[16] == 0x49,
          "shrink preserves preceding shared contents");
  for (size_t i = 17; i < page; ++i)
    require(shared[page + i] == 0 && offset[i] == 0, "shrink zeroes partial borrowed page");
  require(private[page + 37] == 0xA5 && private[page + 16] == 0x49,
          "shrink preserves partial private COW page");
  errno = 0;
  require(pread(file, shared + page * 2, 1, 0) == -1 && errno == EFAULT,
          "usercopy into mapped EOF returns EFAULT");
  require(ftruncate(file, (off_t)(page * 3)) == 0, "regrow mapped file");
  require(shared[page * 2] == 0 && private[page * 2] == 0,
          "regrowth cannot resurrect discarded private or shared whole pages");
  require(
      munmap(shared, page * 3) == 0 && munmap(private, page * 3) == 0 && munmap(offset, page) == 0,
      "unmap shrunken file aliases");
  close(file);
  require(unlink(path) == 0, "remove shrink file");
  puts("VM-CONTRACT: PASS mapped-shrink");
}

void test_vm_contracts(void) {
  long configured_page_size = sysconf(_SC_PAGESIZE);
  require(configured_page_size > 0, "page size");
  size_t page = (size_t)configured_page_size;
  anonymous_ranges(page);
  protected_fork(page);
  file_mappings(page);
  mapped_shrink(page);
  puts("VM-CONTRACT: PASS all");
}
