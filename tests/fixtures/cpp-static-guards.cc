// SPDX-License-Identifier: ISC
// Real compiler-generated local-static guards use the included kernel runtime.
#include <atomic>
#include <cassert>
#include <chrono>
#include <cstdint>
#include <thread>
#include <vector>

#define EXPORTED_PUBLIC
#ifndef HAS_THREAD_SANITIZER
#define HAS_THREAD_SANITIZER 0
#endif
#ifndef THREADS
#define THREADS 1
#endif

enum class ExecutionContext { WaitableThread, AtomicThread, HardDeviceIrq };
std::atomic<unsigned> yieldCount{0}, pauseCount{0};
thread_local unsigned initLevel = 2;
thread_local ExecutionContext context = ExecutionContext::WaitableThread;
struct Processor {
  static unsigned isInitialised() {
    return initLevel;
  }
  static ExecutionContext executionContext() {
    return context;
  }
  static void pause() {
    ++pauseCount;
    std::this_thread::yield();
  }
};
struct Scheduler {
  static Scheduler& instance();
  void yield() {
    ++yieldCount;
    std::this_thread::yield();
  }
};
Scheduler scheduler;
Scheduler& Scheduler::instance() {
  return scheduler;
}
#include "cpp-static-guards.inc"

std::atomic<unsigned> serialConstructions{0}, parallelConstructions{0}, attempts{0};
std::atomic<bool> constructorEntered{false}, allowConstructor{false};
struct Serial {
  Serial() {
    ++serialConstructions;
  }
};
Serial& serial() {
  static Serial s;
  return s;
}
struct Parallel {
  uint64_t words[64]{};
  Parallel() {
    assert(++parallelConstructions == 1);
    constructorEntered.store(true, std::memory_order_release);
    while (!allowConstructor.load(std::memory_order_acquire))
      std::this_thread::yield();
    for (unsigned i = 0; i < 64; ++i)
      words[i] = 0xabc00000ULL + i;
  }
};
Parallel& parallel() {
  static Parallel p;
  return p;
}
struct Retry {
  unsigned value;
  Retry() {
    if (++attempts == 1)
      throw 17;
    value = 0x3197;
  }
};
Retry& retry() {
  static Retry r;
  return r;
}

template <class Predicate>
void boundedWait(Predicate predicate) {
  auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
  while (!predicate()) {
    assert(std::chrono::steady_clock::now() < deadline);
    std::this_thread::yield();
  }
}

#if !HAS_THREAD_SANITIZER
void waitContext(unsigned level, ExecutionContext kind, bool expectYield) {
  alignas(8) uint64_t guard = 0;
  assert(__cxa_guard_acquire(&guard) == 1);
  unsigned previousYields = yieldCount.load(), previousPauses = pauseCount.load();
  std::atomic<bool> returned{false};
  std::thread waiter([&] {
    initLevel = level;
    context = kind;
    assert(__cxa_guard_acquire(&guard) == 0);
    returned.store(true, std::memory_order_release);
  });
  boundedWait([&] {
    return (expectYield ? yieldCount.load() : pauseCount.load()) >
           (expectYield ? previousYields : previousPauses);
  });
  assert(!returned.load());
  if (expectYield)
    assert(pauseCount.load() == previousPauses);
  else
    assert(yieldCount.load() == previousYields);
  __cxa_guard_release(&guard);
  waiter.join();
  auto* bytes = reinterpret_cast<unsigned char*>(&guard);
  assert(bytes[0] == 1 && bytes[1] == 0);
}
#endif

int main() {
  Serial* address = &serial();
  for (unsigned i = 0; i < 1000; ++i)
    assert(&serial() == address);
  assert(serialConstructions == 1);

  constexpr unsigned Readers = 8;
  std::atomic<unsigned> started{0}, verified{0};
  std::vector<std::thread> readers;
  for (unsigned i = 0; i < Readers; ++i)
    readers.emplace_back([&] {
      ++started;
      Parallel& p = parallel();
      for (unsigned j = 0; j < 64; ++j)
        assert(p.words[j] == 0xabc00000ULL + j);
      ++verified;
    });
  boundedWait([&] { return started == Readers && constructorEntered.load(); });
#if !HAS_THREAD_SANITIZER
  boundedWait([&] { return (THREADS ? yieldCount.load() : pauseCount.load()) >= Readers - 1; });
#endif
  assert(verified == 0 && parallelConstructions == 1);
  allowConstructor.store(true, std::memory_order_release);
  for (auto& reader : readers)
    reader.join();
  assert(verified == Readers && parallelConstructions == 1);

  bool aborted = false;
  try {
    retry();
  } catch (int error) {
    assert(error == 17);
    aborted = true;
  }
  assert(aborted && attempts == 1);
  assert(retry().value == 0x3197 && attempts == 2);
  assert(retry().value == 0x3197 && attempts == 2);

#if !HAS_THREAD_SANITIZER
  alignas(8) uint64_t guard = 0;
  assert(__cxa_guard_acquire(&guard) == 1);
  __cxa_guard_abort(&guard);
  assert(guard == 0);
  assert(__cxa_guard_acquire(&guard) == 1);
  __cxa_guard_release(&guard);
  assert(__cxa_guard_acquire(&guard) == 0);
  alignas(8) uint64_t abortedGuard = 0;
  assert(__cxa_guard_acquire(&abortedGuard) == 1);
  unsigned waitsBeforeAbort = THREADS ? yieldCount.load() : pauseCount.load();
  std::atomic<bool> retryCompleted{false};
  std::thread afterAbort([&] {
    assert(__cxa_guard_acquire(&abortedGuard) == 1);
    __cxa_guard_release(&abortedGuard);
    retryCompleted = true;
  });
  boundedWait([&] { return (THREADS ? yieldCount.load() : pauseCount.load()) > waitsBeforeAbort; });
  assert(!retryCompleted);
  __cxa_guard_abort(&abortedGuard);
  afterAbort.join();
  assert(retryCompleted && __cxa_guard_acquire(&abortedGuard) == 0);

  waitContext(2, ExecutionContext::WaitableThread, THREADS);
  waitContext(0, ExecutionContext::WaitableThread, false);
  waitContext(2, ExecutionContext::AtomicThread, false);
  waitContext(2, ExecutionContext::HardDeviceIrq, false);
#endif
}
