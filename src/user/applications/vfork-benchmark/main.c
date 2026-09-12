/* Copyright (c) 2026, Pedigree Developers. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>
#include <sys/resource.h>
#include <sys/syscall.h>
#include <sys/wait.h>

#define CHILD_STATUS 23
#define MAX_SIZES 8

static char self[PATH_MAX];
static char* child_arguments[] = {self, "--worker", NULL};
static char* child_environment[] = {"LC_ALL=C", "PATH=/usr/bin:/bin", NULL};
static volatile int probe_value;
static int call_fork;

struct options {
  unsigned iterations, samples, warmup;
  unsigned sizes[MAX_SIZES];
  size_t size_count;
  int mode;
  const char* expect;
};

struct usage {
  uint64_t parent_user, parent_system, child_user, child_system;
};

static int failure(const char* operation) {
  fprintf(stderr, "VFORK-BENCH: FAIL operation=%s errno=%d\n", operation, errno);
  return -1;
}

static uint64_t timeval_us(const struct timeval* value) {
  return (uint64_t)value->tv_sec * 1000000 + (uint64_t)value->tv_usec;
}

static int usage_snapshot(struct usage* result) {
  struct rusage parent, children;
  if (getrusage(RUSAGE_SELF, &parent) || getrusage(RUSAGE_CHILDREN, &children))
    return failure("getrusage");
  result->parent_user = timeval_us(&parent.ru_utime);
  result->parent_system = timeval_us(&parent.ru_stime);
  result->child_user = timeval_us(&children.ru_utime);
  result->child_system = timeval_us(&children.ru_stime);
  return 0;
}

static int monotonic_ns(uint64_t* result) {
  struct timespec value;
  // The syscall samples current ticks; the vDSO can use the last timer snapshot.
  if (syscall(SYS_clock_gettime, CLOCK_MONOTONIC, &value))
    return failure("clock_gettime");
  *result = (uint64_t)value.tv_sec * 1000000000 + (uint64_t)value.tv_nsec;
  return 0;
}

static int reap(pid_t child, int expected) {
  int status;
  pid_t waited;
  do {
    waited = waitpid(child, &status, 0);
  } while (waited < 0 && errno == EINTR);
  if (waited != child)
    return failure("waitpid");
  if (!WIFEXITED(status) || WEXITSTATUS(status) != expected) {
    fprintf(stderr, "VFORK-BENCH: FAIL operation=child-status status=%d\n", status);
    return -1;
  }
  return 0;
}

static int spawn(int execute) {
  pid_t child = call_fork ? fork() : vfork();
  if (!child) {
    if (execute) {
      execve(self, child_arguments, child_environment);
      _exit(127);
    }
    _exit(CHILD_STATUS);
  }
  if (child < 0)
    return failure(call_fork ? "fork" : "vfork");
  return reap(child, CHILD_STATUS);
}

static const char* probe(void) {
  // This untimed semantic check is separate from the exec/_exit-only workload.
  probe_value = 0;
  pid_t child = vfork();
  if (!child) {
    probe_value = 1;
    _exit(CHILD_STATUS);
  }
  if (child < 0) {
    failure("vfork-probe");
    return NULL;
  }
  if (reap(child, CHILD_STATUS))
    return NULL;
  return probe_value ? "shared" : "fork";
}

static int warm_executable(void) {
  int fd = open(self, O_RDONLY);
  if (fd < 0)
    return failure("open-self");
  char buffer[8192];
  ssize_t count;
  do {
    count = read(fd, buffer, sizeof(buffer));
  } while (count > 0 || (count < 0 && errno == EINTR));
  int saved_errno = errno;
  if (close(fd))
    return failure("close-self");
  if (count < 0) {
    errno = saved_errno;
    return failure("read-self");
  }
  return spawn(1);
}

static int measure(const char* variant, unsigned mib, int execute, unsigned sample,
                   unsigned iterations) {
  struct usage before, after;
  uint64_t start, end;
  if (usage_snapshot(&before) || monotonic_ns(&start))
    return -1;
  for (unsigned i = 0; i < iterations; ++i)
    if (spawn(execute))
      return -1;
  if (monotonic_ns(&end) || usage_snapshot(&after))
    return -1;
  if (end < start || after.parent_user < before.parent_user ||
      after.parent_system < before.parent_system || after.child_user < before.child_user ||
      after.child_system < before.child_system)
    return failure("nonmonotonic-counter");
  printf("sample,%s,%s,%s,%u,%u,%u,%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 ",%" PRIu64 "\n",
         variant, call_fork ? "fork" : "vfork", execute ? "exec" : "exit", mib, sample, iterations,
         end - start, after.parent_user - before.parent_user,
         after.parent_system - before.parent_system, after.child_user - before.child_user,
         after.child_system - before.child_system);
  return 0;
}

static int number(const char* text, unsigned minimum, unsigned maximum, unsigned* value) {
  char* end;
  errno = 0;
  unsigned long parsed = strtoul(text, &end, 10);
  if (!*text || *end || errno || parsed < minimum || parsed > maximum)
    return -1;
  *value = (unsigned)parsed;
  return 0;
}

static int sizes(const char* text, struct options* options) {
  options->size_count = 0;
  while (*text && options->size_count < MAX_SIZES) {
    char* end;
    errno = 0;
    unsigned long value = strtoul(text, &end, 10);
    if (end == text || errno || value > 1024 || (*end && *end != ','))
      return -1;
    options->sizes[options->size_count++] = (unsigned)value;
    if (!*end)
      return 0;
    text = end + 1;
  }
  return -1;
}

static void help(void) {
  puts(
      "Usage: vfork-benchmark [--iterations N] [--samples N] [--warmup N]\n"
      "       [--sizes 0,16,64] [--mode both|exec|exit] [--expect shared|fork]\n"
      "       [--call vfork|fork]\n"
      "Measures sequential process creation plus child completion. Sizes are parent\n"
      "payload MiB added to the program's baseline memory, not measured total RSS.\n"
      "Payload pages are touched once before warmup and remain untouched in timing.");
}

int main(int argc, char** argv) {
  if (argc == 2 && !strcmp(argv[1], "--worker"))
    _exit(CHILD_STATUS);
  struct options options = {30, 5, 3, {0, 16, 64}, 3, 2, NULL};
  for (int i = 1; i < argc; ++i) {
    const char* name = argv[i];
    if (!strcmp(name, "--help")) {
      help();
      return 0;
    }
    if (++i == argc)
      goto invalid;
    const char* value = argv[i];
    if (!strcmp(name, "--iterations")) {
      if (number(value, 1, 10000, &options.iterations))
        goto invalid;
    } else if (!strcmp(name, "--samples")) {
      if (number(value, 1, 100, &options.samples))
        goto invalid;
    } else if (!strcmp(name, "--warmup")) {
      if (number(value, 0, 1000, &options.warmup))
        goto invalid;
    } else if (!strcmp(name, "--sizes")) {
      if (sizes(value, &options))
        goto invalid;
    } else if (!strcmp(name, "--mode")) {
      options.mode = !strcmp(value, "both")   ? 2
                     : !strcmp(value, "exec") ? 1
                     : !strcmp(value, "exit") ? 0
                                              : -1;
      if (options.mode < 0)
        goto invalid;
    } else if (!strcmp(name, "--expect")) {
      if (strcmp(value, "shared") && strcmp(value, "fork"))
        goto invalid;
      options.expect = value;
    } else if (!strcmp(name, "--call")) {
      if (strcmp(value, "vfork") && strcmp(value, "fork"))
        goto invalid;
      call_fork = !strcmp(value, "fork");
    } else
      goto invalid;
  }
  setvbuf(stdout, NULL, _IOLBF, 0);
  long page_size = sysconf(_SC_PAGESIZE);
  ssize_t path_size = readlink("/proc/self/exe", self, sizeof(self) - 1);
  if (page_size <= 0 || path_size <= 0 || path_size >= (ssize_t)sizeof(self) - 1)
    return failure("startup") != 0;
  self[path_size] = 0;
  const char* variant = probe();
  if (!variant)
    return 1;
  printf("VFORK-BENCH: PROBE variant=%s\n", variant);
  if (options.expect && strcmp(variant, options.expect)) {
    fprintf(stderr, "VFORK-BENCH: FAIL expected=%s observed=%s\n", options.expect, variant);
    return 2;
  }
  if (warm_executable())
    return 1;
  printf(
      "VFORK-BENCH: BEGIN iterations=%u samples=%u warmup=%u page_bytes=%ld "
      "clock=monotonic-syscall payload=touched-once faults=unsupported maxrss=unsupported\n",
      options.iterations, options.samples, options.warmup, page_size);
  puts(
      "kind,variant,call,mode,payload_mib,sample,iterations,wall_ns,parent_user_us,parent_system_"
      "us,"
      "children_user_us,children_system_us");
  for (size_t size = 0; size < options.size_count; ++size) {
    unsigned mib = options.sizes[size];
    size_t bytes = (size_t)mib * 1024 * 1024;
    volatile unsigned char* payload = NULL;
    if (bytes) {
      payload = mmap(NULL, bytes, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
      if (payload == MAP_FAILED)
        return failure("mmap") != 0;
      for (size_t offset = 0; offset < bytes; offset += (size_t)page_size)
        payload[offset] = 0x5a;
      payload[bytes - 1] = 0xa5;
    }
    for (unsigned i = 0; i < options.warmup; ++i)
      for (int mode = 0; mode < 2; ++mode)
        if ((options.mode == 2 || options.mode == mode) && spawn(mode))
          return 1;
    for (unsigned sample = 1; sample <= options.samples; ++sample)
      for (int position = 0; position < (options.mode == 2 ? 2 : 1); ++position) {
        int mode = options.mode == 2 ? ((sample + position) & 1) : options.mode;
        if (measure(variant, mib, mode, sample, options.iterations))
          return 1;
      }
    if (bytes && munmap((void*)payload, bytes))
      return failure("munmap") != 0;
  }
  puts("VFORK-BENCH: PASS");
  return 0;

invalid:
  help();
  return 2;
}
