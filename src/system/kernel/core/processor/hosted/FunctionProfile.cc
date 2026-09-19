/* Copyright (c) 2026, Pedigree Developers. */

#include "pedigree/kernel/processor/Processor.h"
#include "pedigree/kernel/processor/hosted/FunctionProfile.h"

#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

#include <sys/mman.h>

namespace {
struct Event {
  uint64_t ticks;
  uint64_t function;
  uint64_t caller;
  uint64_t kind;
};
static_assert(sizeof(Event) == 32);
constexpr size_t Capacity = 2 * 1024 * 1024;
Event* events = nullptr;
const char* directory = nullptr;
size_t limit = 100;
bool active = false;
thread_local bool owner = false;
bool previousInterrupts = false;
bool openCapture = false;
size_t eventCount = 0;
size_t dropped = 0;
unsigned invalidation = 0;
const char* phase = nullptr;
size_t repetition = 0;
size_t iterations = 0;
uint64_t startNs = 0;

uint64_t nowNs() {
  timespec ts = {};
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return uint64_t(ts.tv_sec) * 1000000000ULL + ts.tv_nsec;
}

void record(void* function, void* caller, uint64_t kind) {
  if (!__atomic_load_n(&active, __ATOMIC_RELAXED) || !owner) {
    return;
  }
  if (eventCount == Capacity) {
    ++dropped;
    return;
  }
  uint32_t low, high;
  // TSC ticks measure elapsed time, not CPU cycles. Serialise the read so the
  // compiler hooks have ordered boundaries even around very small inlines.
  asm volatile("lfence; rdtsc; lfence" : "=a"(low), "=d"(high) : : "memory");
  events[eventCount++] = {uint64_t(high) << 32 | low, reinterpret_cast<uintptr_t>(function),
                          reinterpret_cast<uintptr_t>(caller), kind};
}

bool writeEvents(const char* path) {
  int fd = open(path, O_WRONLY | O_CREAT | O_EXCL, 0600);
  if (fd < 0) {
    return false;
  }
  const char* data = reinterpret_cast<const char*>(events);
  size_t remaining = eventCount * sizeof(Event);
  bool ok = true;
  while (remaining) {
    const ssize_t written = write(fd, data, remaining);
    if (written < 0 && errno == EINTR) {
      continue;
    }
    if (written <= 0) {
      ok = false;
      break;
    }
    data += written;
    remaining -= written;
  }
  return close(fd) == 0 && ok;
}
}  // namespace

extern "C" EXPORTED_PUBLIC void __cyg_profile_func_enter(void* function, void* caller) {
  record(function, caller, 1);
}

extern "C" EXPORTED_PUBLIC void __cyg_profile_func_exit(void* function, void* caller) {
  record(function, caller, 2);
}

bool hostedFunctionProfileInitialise() {
  directory = getenv("PEDIGREE_HOSTED_FUNCTION_PROFILE_DIR");
  if (!directory || !*directory) {
    directory = nullptr;
    return true;
  }
  const char* value = getenv("PEDIGREE_HOSTED_FUNCTION_PROFILE_LIMIT");
  if (value) {
    char* end = nullptr;
    const unsigned long requested = strtoul(value, &end, 10);
    if (!*value || *end || !requested || requested > 10000) {
      fprintf(stderr, "Function profile limit must be between 1 and 10000.\n");
      return false;
    }
    limit = requested;
  }
  void* storage = mmap(nullptr, Capacity * sizeof(Event), PROT_READ | PROT_WRITE,
                       MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  if (storage == MAP_FAILED) {
    perror("Function profile buffer");
    return false;
  }
  events = static_cast<Event*>(storage);
  // Fault in storage before a capture. Hooks must never allocate or fault in a
  // Pedigree page while reconstructing another function's entry or exit.
  volatile char* bytes = static_cast<volatile char*>(storage);
  for (size_t offset = 0; offset < Capacity * sizeof(Event); offset += 4096) {
    bytes[offset] = 0;
  }
  return true;
}

size_t hostedFunctionProfileCount(size_t count) {
  return directory && count > limit ? limit : count;
}

void hostedFunctionProfileBegin(const char* name, size_t rep, size_t count) {
  if (!directory) {
    return;
  }
  previousInterrupts = Processor::getInterrupts();
  Processor::setInterrupts(false);
  owner = true;
  phase = name;
  repetition = rep + 1;
  iterations = count;
  eventCount = dropped = invalidation = 0;
  openCapture = true;
  startNs = nowNs();
  __atomic_store_n(&active, true, __ATOMIC_RELEASE);
}

void hostedFunctionProfileInvalidate(HostedProfileInvalidation reason) {
  if (__atomic_load_n(&active, __ATOMIC_RELAXED) && owner) {
    invalidation = static_cast<unsigned>(reason);
    // A suspended stack must not make another thread look like its child.
    __atomic_store_n(&active, false, __ATOMIC_RELEASE);
  }
}

bool hostedFunctionProfileEnd() {
  if (!directory || !openCapture) {
    return true;
  }
  __atomic_store_n(&active, false, __ATOMIC_RELEASE);
  const uint64_t elapsed = nowNs() - startNs;
  owner = false;
  openCapture = false;
  Processor::setInterrupts(previousInterrupts);

  char path[4096];
  int length = snprintf(path, sizeof(path), "%s/%s-%zu.bin", directory, phase, repetition);
  bool saved = length > 0 && size_t(length) < sizeof(path) && writeEvents(path);
  length = snprintf(path, sizeof(path), "%s/%s-%zu.json", directory, phase, repetition);
  FILE* metadata = length > 0 && size_t(length) < sizeof(path) ? fopen(path, "wx") : nullptr;
  if (metadata) {
    const int written =
        fprintf(metadata,
                "{\"format_version\":1,\"phase\":\"%s\",\"repetition\":%zu,\"count\":%zu,"
                "\"events\":%zu,\"dropped\":%zu,\"invalidation\":%u,\"elapsed_ns\":%llu}\n",
                phase, repetition, iterations, eventCount, dropped, invalidation,
                static_cast<unsigned long long>(elapsed));
    const int closed = fclose(metadata);
    saved = saved && written > 0 && closed == 0;
  } else {
    saved = false;
  }
  fprintf(stderr,
          "HOSTED-FUNCTION-PROFILE: %s phase=%s rep=%zu count=%zu events=%zu "
          "dropped=%zu invalidation=%u\n",
          saved && !dropped && !invalidation ? "PASS" : "FAIL", phase, repetition, iterations,
          eventCount, dropped, invalidation);
  return saved && !dropped && !invalidation;
}
