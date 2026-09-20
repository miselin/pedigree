#define _GNU_SOURCE
#define _POSIX_C_SOURCE 200809L

#include <errno.h>
#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/syscall.h>

static const size_t PageSize = 4096;

static uint64_t now_ns(void) {
  struct timespec value;
  if (clock_gettime(CLOCK_MONOTONIC, &value)) {
    perror("clock_gettime");
    exit(1);
  }
  return (uint64_t)value.tv_sec * 1000000000ULL + (uint64_t)value.tv_nsec;
}

static size_t parse_size(const char* text, const char* name) {
  char* end = NULL;
  unsigned long long value = strtoull(text, &end, 10);
  if (!text[0] || !end || *end || !value || value > SIZE_MAX) {
    fprintf(stderr, "VM-BENCH invalid %s: %s\n", name, text);
    exit(2);
  }
  return (size_t)value;
}

static void fail(const char* phase) {
  fprintf(stderr, "VM-BENCH FAIL phase=%s errno=%d error=%s\n", phase, errno, strerror(errno));
  exit(1);
}

static void metric(const char* phase, uint64_t start, size_t operations, size_t pages,
                   uintptr_t checksum) {
  const uint64_t elapsed = now_ns() - start;
  printf("IOBENCH metric phase=%s elapsed_us=%llu operations=%zu pages=%zu checksum=%llu\n", phase,
         (unsigned long long)(elapsed / 1000), operations, pages, (unsigned long long)checksum);
}

static void run_serial(const char* phase, size_t iterations, size_t first_pages, size_t page_span,
                       int file_backed, int touch) {
  int fd = -1;
  if (file_backed) {
    fd = open("/root/compile-bench/which.cc", O_RDONLY);
    if (fd < 0)
      fail("open-file");
  }

  uint64_t start = now_ns();
  size_t pages = 0;
  uintptr_t checksum = 0;
  for (size_t i = 0; i < iterations; ++i) {
    const size_t count = first_pages + (page_span ? i % page_span : 0);
    const size_t length = count * PageSize;
    void* address = mmap(NULL, length, PROT_READ | PROT_WRITE,
                         file_backed ? MAP_PRIVATE : MAP_PRIVATE | MAP_ANONYMOUS, fd, 0);
    if (address == MAP_FAILED)
      fail(phase);
    if (touch) {
      volatile unsigned char* bytes = address;
      bytes[0] = (unsigned char)i;
      bytes[length - 1] ^= (unsigned char)(i >> 8);
    }
    checksum ^= (uintptr_t)address;
    if (munmap(address, length))
      fail(phase);
    pages += count;
  }
  if (fd >= 0 && close(fd))
    fail("close-file");
  metric(phase, start, iterations, pages, checksum);
}

static void run_fragmented(size_t iterations, size_t slots, size_t pages_per_mapping) {
  if (!slots || slots > 256)
    slots = 64;
  void* addresses[256] = {};
  const size_t length = pages_per_mapping * PageSize;
  uint64_t start = now_ns();
  uintptr_t checksum = 0;
  for (size_t i = 0; i < iterations; ++i) {
    const size_t slot = i % slots;
    if (addresses[slot] && munmap(addresses[slot], length))
      fail("fragmented-unmap");
    addresses[slot] =
        mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (addresses[slot] == MAP_FAILED)
      fail("fragmented-mmap");
    checksum ^= (uintptr_t)addresses[slot];
  }
  for (size_t i = 0; i < slots; ++i) {
    if (addresses[i] && munmap(addresses[i], length))
      fail("fragmented-final-unmap");
  }
  metric("mmap_fragmented", start, iterations + slots, (iterations + slots) * pages_per_mapping,
         checksum);
}

static size_t gcc_pattern_pages(size_t index) {
  // This is a compact approximation of the exact which.cc trace: 1-, 2-, 4-,
  // 8-, 16-, and 17-page anonymous mappings account for nearly all mappings.
  static const unsigned char pattern[] = {
      1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2,  2,  2,  2, 2,
      2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 4, 4, 4, 4, 4, 4,  4,  4,  4, 4,
      4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 4, 8, 8, 8, 8, 8, 16, 16, 17,
  };
  return pattern[index % (sizeof(pattern) / sizeof(pattern[0]))];
}

static void run_gcc_pattern(size_t iterations) {
  uint64_t start = now_ns();
  size_t pages = 0;
  uintptr_t checksum = 0;
  for (size_t i = 0; i < iterations; ++i) {
    const size_t count = gcc_pattern_pages(i);
    const size_t length = count * PageSize;
    void* address = mmap(NULL, length, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    if (address == MAP_FAILED)
      fail("gcc-pattern-mmap");
    checksum ^= (uintptr_t)address;
    if (munmap(address, length))
      fail("gcc-pattern-munmap");
    pages += count;
  }
  metric("mmap_gcc_pattern", start, iterations, pages, checksum);
}

static void run_basic_syscall(const char* phase, size_t iterations, long syscall_number) {
  uint64_t start = now_ns();
  uintptr_t checksum = 0;
  for (size_t i = 0; i < iterations; ++i) {
    const long value = syscall(syscall_number);
    if (value < 0)
      fail(phase);
    checksum ^= (uintptr_t)value;
  }
  metric(phase, start, iterations, 0, checksum);
}

int main(int argc, char** argv) {
  char configured_mode[32] = "anonymous";
  size_t configured_iterations = 100000;
  size_t configured_pages = 1;
  int config = open("/vm-syscall-latency.conf", O_RDONLY);
  if (config >= 0) {
    char text[128] = {};
    const ssize_t length = read(config, text, sizeof(text) - 1);
    close(config);
    if (length > 0 && sscanf(text, "%31s %zu %zu", configured_mode, &configured_iterations,
                             &configured_pages) != 3) {
      fprintf(stderr, "VM-BENCH invalid /vm-syscall-latency.conf\n");
      return 2;
    }
  }
  const int first_argument_is_size = argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9';
  const char* mode = argc > 1 && !first_argument_is_size ? argv[1] : configured_mode;
  const size_t iterations = argc > 2 ? parse_size(argv[2], "iterations") : configured_iterations;
  const size_t pages = argc > 3 ? parse_size(argv[3], "pages") : configured_pages;

  setvbuf(stdout, NULL, _IONBF, 0);
  printf("IOBENCH BEGIN mode=%s iterations=%zu pages=%zu\n", mode, iterations, pages);
  if (!strcmp(mode, "anonymous")) {
    run_serial("mmap_anonymous", iterations, pages, 0, 0, 0);
  } else if (!strcmp(mode, "anonymous-touch")) {
    run_serial("mmap_anonymous_touch", iterations, pages, 0, 0, 1);
  } else if (!strcmp(mode, "staircase")) {
    run_serial("mmap_staircase", iterations, pages, 63, 0, 0);
  } else if (!strcmp(mode, "file")) {
    run_serial("mmap_file", iterations, pages, 0, 1, 0);
  } else if (!strcmp(mode, "fragmented")) {
    run_fragmented(iterations, pages, 1);
  } else if (!strcmp(mode, "gcc-pattern")) {
    run_gcc_pattern(iterations);
  } else if (!strcmp(mode, "getuid")) {
    run_basic_syscall("syscall_getuid", iterations, SYS_getuid);
  } else if (!strcmp(mode, "getpid")) {
    run_basic_syscall("syscall_getpid", iterations, SYS_getpid);
  } else {
    fprintf(stderr,
            "usage: %s [anonymous|anonymous-touch|staircase|file|fragmented|gcc-pattern|getuid|"
            "getpid] "
            "iterations pages\n",
            argv[0]);
    return 2;
  }
  printf("IOBENCH PASS END\n");
  return 0;
}
