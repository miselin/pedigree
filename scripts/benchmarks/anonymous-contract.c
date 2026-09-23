#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/wait.h>

static size_t page_bytes;

static void require(int value, const char* operation) {
  if (!value) {
    fprintf(stderr, "ANON-CONTRACT FAIL operation=%s errno=%d\n", operation, errno);
    exit(1);
  }
}

static unsigned char pattern(size_t page) {
  return (unsigned char)(page % 251 + 1);
}

static uint64_t nanoseconds(const struct timespec* value) {
  return (uint64_t)value->tv_sec * UINT64_C(1000000000) + value->tv_nsec;
}

static uint64_t microseconds(const struct timeval* value) {
  return (uint64_t)value->tv_sec * UINT64_C(1000000) + value->tv_usec;
}

static void scaling(size_t megabytes) {
  const size_t bytes = megabytes * 1024 * 1024;
  const size_t pages = bytes / page_bytes;
  volatile unsigned char* memory =
      mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(memory != MAP_FAILED, "scaling mmap");
  struct timespec start, end;
  struct rusage before, after;
  require(getrusage(RUSAGE_SELF, &before) == 0, "scaling getrusage before");
  require(clock_gettime(CLOCK_MONOTONIC, &start) == 0, "scaling clock before");
  for (size_t page = 0; page < pages; ++page)
    memory[page * page_bytes] = pattern(page);
  require(clock_gettime(CLOCK_MONOTONIC, &end) == 0, "scaling clock after");
  require(getrusage(RUSAGE_SELF, &after) == 0, "scaling getrusage after");
  for (size_t page = 0; page < pages; ++page)
    require(memory[page * page_bytes] == pattern(page), "scaling contents");
  printf("ANONBENCH bytes=%zu pages=%zu wall_ns=%llu user_us=%llu system_us=%llu\n", bytes, pages,
         (unsigned long long)(nanoseconds(&end) - nanoseconds(&start)),
         (unsigned long long)(microseconds(&after.ru_utime) - microseconds(&before.ru_utime)),
         (unsigned long long)(microseconds(&after.ru_stime) - microseconds(&before.ru_stime)));
  require(munmap((void*)memory, bytes) == 0, "scaling munmap");
}

static void lifecycle(void) {
  const size_t pages = 256, bytes = pages * page_bytes, quarter = bytes / 4;
  volatile unsigned char* memory =
      mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(memory != MAP_FAILED, "lifecycle mmap");
  for (size_t n = 0; n < pages; ++n) {
    const size_t page = (n * 153 + 17) % pages;
    require(memory[page * page_bytes] == 0, "shuffled zero page");
    memory[page * page_bytes] = pattern(page);
    memory[(page + 1) * page_bytes - 1] = (unsigned char)~pattern(page);
  }
  pid_t child = fork();
  require(child >= 0, "fork");
  if (!child) {
    for (size_t page = 0; page < pages; ++page) {
      if (memory[page * page_bytes] != pattern(page))
        _exit(2);
      memory[page * page_bytes] = 0;
    }
    _exit(0);
  }
  int status;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  require(waited == child && WIFEXITED(status) && WEXITSTATUS(status) == 0, "child contents");
  for (size_t page = 0; page < pages; ++page) {
    require(memory[page * page_bytes] == pattern(page), "parent copy-on-write contents");
    require(memory[(page + 1) * page_bytes - 1] == (unsigned char)~pattern(page),
            "parent page tail");
  }
  require(mprotect((void*)memory, quarter, PROT_READ) == 0, "split protection");
  require(mprotect((void*)memory, quarter, PROT_READ | PROT_WRITE) == 0, "restore protection");
  require(munmap((void*)(memory + quarter), 2 * quarter) == 0, "partial unmap");
  require(mmap((void*)(memory + quarter), 2 * quarter, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void*)(memory + quarter),
          "replace middle");
  for (size_t page = 0; page < pages; ++page) {
    const unsigned char expected = page >= pages / 4 && page < pages * 3 / 4 ? 0 : pattern(page);
    require(memory[page * page_bytes] == expected, "partial unmap surviving contents");
  }
  void* destination = mmap(NULL, quarter, PROT_NONE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  require(destination != MAP_FAILED, "move reservation");
  volatile unsigned char* moved = mremap((void*)(memory + 3 * quarter), quarter, quarter,
                                         MREMAP_MAYMOVE | MREMAP_FIXED, destination);
  require(moved == destination, "move suffix");
  for (size_t page = 0; page < pages / 4; ++page) {
    require(moved[page * page_bytes] == pattern(page + pages * 3 / 4), "moved contents");
    moved[page * page_bytes] = 0xA5;
  }
  require(mmap((void*)(memory + 3 * quarter), quarter, PROT_READ | PROT_WRITE,
               MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED, -1, 0) == (void*)(memory + 3 * quarter),
          "reuse moved suffix");
  for (size_t page = pages * 3 / 4; page < pages; ++page)
    require(memory[page * page_bytes] == 0, "reused suffix zero contents");
  require(munmap((void*)moved, quarter) == 0, "moved munmap");
  require(munmap((void*)memory, bytes) == 0, "lifecycle munmap");
  puts("ANON-CONTRACT PASS shuffled-zero-write fork-cow split-discard move-reuse");
}

static void disjoint_mappings(void) {
  enum { Count = 256 };
  volatile unsigned char* mappings[Count];
  unsigned char live[Count] = {0};
  struct timespec start, end;
  struct rusage before, after;
  require(getrusage(RUSAGE_SELF, &before) == 0, "disjoint getrusage before");
  require(clock_gettime(CLOCK_MONOTONIC, &start) == 0, "disjoint clock before");
  for (size_t i = 0; i < Count; ++i) {
    mappings[i] =
        mmap(NULL, page_bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(mappings[i] != MAP_FAILED, "disjoint mmap");
    require(mappings[i][0] == 0, "disjoint zero");
    mappings[i][0] = pattern(i);
    live[i] = 1;
  }
  for (size_t n = 0; n < Count / 2; ++n) {
    const size_t i = (n * 153 + 17) % Count;
    require(munmap((void*)mappings[i], page_bytes) == 0, "disjoint shuffled unmap");
    live[i] = 0;
  }
  for (size_t i = 0; i < Count; ++i) {
    if (live[i]) {
      require(mappings[i][0] == pattern(i), "disjoint survivor");
      continue;
    }
    require(mmap((void*)mappings[i], page_bytes, PROT_READ | PROT_WRITE,
                 MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void*)mappings[i],
            "disjoint reservation reuse");
    require(mappings[i][0] == 0, "disjoint reused zero");
    mappings[i][0] = pattern(i);
  }
  for (size_t i = 0; i < Count; ++i) {
    require(mappings[i][0] == pattern(i), "disjoint final contents");
    require(munmap((void*)mappings[i], page_bytes) == 0, "disjoint final unmap");
  }
  require(clock_gettime(CLOCK_MONOTONIC, &end) == 0, "disjoint clock after");
  require(getrusage(RUSAGE_SELF, &after) == 0, "disjoint getrusage after");
  printf("ANONBENCH mapping_churn=%u wall_ns=%llu user_us=%llu system_us=%llu\n", Count,
         (unsigned long long)(nanoseconds(&end) - nanoseconds(&start)),
         (unsigned long long)(microseconds(&after.ru_utime) - microseconds(&before.ru_utime)),
         (unsigned long long)(microseconds(&after.ru_stime) - microseconds(&before.ru_stime)));
  puts("ANON-CONTRACT PASS disjoint-append whole-removal reservation-reuse survivors");
}

struct concurrent_context {
  pthread_barrier_t* gate;
  unsigned worker;
};

static void concurrent_gate(pthread_barrier_t* gate) {
  int result = pthread_barrier_wait(gate);
  require(result == 0 || result == PTHREAD_BARRIER_SERIAL_THREAD, "concurrent barrier");
}

static void* concurrent_mapping_worker(void* argument) {
  const struct concurrent_context* context = argument;
  const size_t pages = 32, bytes = pages * page_bytes, half = bytes / 2;
  for (unsigned round = 0; round < 16; ++round) {
    const int mutate = (context->worker + round) & 1;
    volatile unsigned char* memory =
        mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
    require(memory != MAP_FAILED, "concurrent mmap");
    if (mutate) {
      for (size_t page = 0; page < pages; ++page) {
        memory[page * page_bytes] = pattern(round * pages + page);
        memory[(page + 1) * page_bytes - 1] = (unsigned char)~pattern(round * pages + page);
      }
    }
    concurrent_gate(context->gate);

    if (mutate) {
      require(mprotect((void*)memory, bytes, PROT_READ) == 0, "concurrent protect");
      for (size_t page = 0; page < pages; ++page) {
        require(memory[page * page_bytes] == pattern(round * pages + page) &&
                    memory[(page + 1) * page_bytes - 1] ==
                        (unsigned char)~pattern(round * pages + page),
                "concurrent protected contents");
      }
      require(mprotect((void*)memory, bytes, PROT_READ | PROT_WRITE) == 0,
              "concurrent restore protection");
      require(munmap((void*)memory, half) == 0, "concurrent partial unmap");
      require(mmap((void*)memory, half, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0) == (void*)memory,
              "concurrent reservation reuse");
    }
    for (size_t page = 0; page < pages; ++page) {
      const unsigned char expected =
          mutate && page >= pages / 2 ? pattern(round * pages + page) : 0;
      const unsigned char tail = mutate && page >= pages / 2 ? (unsigned char)~expected : 0;
      require(memory[page * page_bytes] == expected && memory[(page + 1) * page_bytes - 1] == tail,
              "concurrent zero and survivor contents");
      memory[page * page_bytes] = pattern(round * pages + page);
      memory[(page + 1) * page_bytes - 1] = (unsigned char)~pattern(round * pages + page);
    }
    for (size_t page = 0; page < pages; ++page) {
      require(
          memory[page * page_bytes] == pattern(round * pages + page) &&
              memory[(page + 1) * page_bytes - 1] == (unsigned char)~pattern(round * pages + page),
          "concurrent written contents");
    }
    // Keep either worker from allocating into the other's temporary unmap gap.
    concurrent_gate(context->gate);
    require(munmap((void*)memory, bytes) == 0, "concurrent final unmap");
  }
  return NULL;
}

static void concurrent_mappings(void) {
  pthread_barrier_t gate;
  pthread_t workers[2];
  struct concurrent_context contexts[] = {{&gate, 0}, {&gate, 1}};
  alarm(30);
  require(pthread_barrier_init(&gate, NULL, 2) == 0, "concurrent barrier init");
  for (size_t i = 0; i < 2; ++i) {
    require(pthread_create(&workers[i], NULL, concurrent_mapping_worker, &contexts[i]) == 0,
            "concurrent worker start");
  }
  for (size_t i = 0; i < 2; ++i) {
    require(pthread_join(workers[i], NULL) == 0, "concurrent worker join");
  }
  require(pthread_barrier_destroy(&gate) == 0, "concurrent barrier destroy");
  alarm(0);
  puts("ANON-CONTRACT PASS concurrent-fault protection-unmap-reuse survivors");
}

int main(int argc, char** argv) {
  setvbuf(stdout, NULL, _IONBF, 0);
  const long page_size = sysconf(_SC_PAGESIZE);
  require(page_size > 0 && (1024 * 1024) % page_size == 0, "page size");
  page_bytes = (size_t)page_size;
  if (argc == 2) {
    char* end;
    errno = 0;
    const unsigned long megabytes = strtoul(argv[1], &end, 10);
    require(!errno && *argv[1] && !*end && megabytes >= 1 && megabytes <= 64, "size 1..64 MiB");
    scaling(megabytes);
  } else {
    require(argc == 1, "optional MiB argument");
    const size_t sizes[] = {1, 4, 16, 64};
    for (size_t i = 0; i < sizeof(sizes) / sizeof(sizes[0]); ++i)
      scaling(sizes[i]);
  }
  lifecycle();
  disjoint_mappings();
  concurrent_mappings();
  puts("ANON-CONTRACT END status=0");
  return 0;
}
