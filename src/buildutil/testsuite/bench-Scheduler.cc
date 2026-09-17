/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE. IN NO EVENT SHALL
 * THE AUTHOR BE LIABLE FOR ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL
 * DAMAGES OR ANY DAMAGES WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR
 * PROFITS, WHETHER IN AN ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS
 * ACTION, ARISING OUT OF OR IN CONNECTION WITH THE USE OR PERFORMANCE OF
 * THIS SOFTWARE.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <vector>

#include <benchmark/benchmark.h>

namespace {

constexpr size_t kMaxPriorities = 8;

class BenchmarkSpinlock {
 public:
  void acquire() {
    while (m_Flag.test_and_set(std::memory_order_acquire)) {
    }
  }

  void release() {
    m_Flag.clear(std::memory_order_release);
  }

 private:
  std::atomic_flag m_Flag = ATOMIC_FLAG_INIT;
};

class BenchmarkSpinlockGuard {
 public:
  explicit BenchmarkSpinlockGuard(BenchmarkSpinlock& lock) : m_Lock(lock) {
    m_Lock.acquire();
  }

  ~BenchmarkSpinlockGuard() {
    m_Lock.release();
  }

 private:
  BenchmarkSpinlock& m_Lock;
};

struct BenchmarkThread {
  size_t priority = 1;
  bool ready = true;
  bool queued = false;
  BenchmarkThread* previous = nullptr;
  BenchmarkThread* next = nullptr;
};

/** Host-only model of RoundRobin::getNext for selection-scaling measurements. */
class RoundRobinSelectionModel {
 public:
  explicit RoundRobinSelectionModel(size_t threadCount, bool mixedPriorities)
      : m_Threads(threadCount) {
    for (size_t i = 0; i < threadCount; ++i) {
      m_Threads[i].priority = mixedPriorities ? i % kMaxPriorities : 1;
    }
  }

  void refill() {
    for (BenchmarkThread& thread : m_Threads) {
      if (!thread.queued) {
        enqueue(&thread);
      }
    }
  }

  BenchmarkThread* selectNext() {
    BenchmarkSpinlockGuard guard(m_Lock);

    for (size_t priority = 0; priority < kMaxPriorities; ++priority) {
      size_t candidates = queueCount(priority);
      while (candidates--) {
        BenchmarkThread* thread = m_Heads[priority];
        unlink(thread);

        if (thread == nullptr || !thread->ready) {
          continue;
        }

        if (thread->priority != priority) {
          enqueue(thread);
          continue;
        }

        return thread;
      }
    }

    return nullptr;
  }

 private:
  size_t queueCount(size_t priority) const {
#if PEDIGREE_READY_QUEUE_COUNTS
    return m_QueueCounts[priority];
#else
    size_t count = 0;
    for (BenchmarkThread* thread = m_Heads[priority]; thread; thread = thread->next) {
      ++count;
    }
    return count;
#endif
  }

  void enqueue(BenchmarkThread* thread) {
    const size_t priority = thread->priority;
    thread->previous = m_Tails[priority];
    thread->next = nullptr;
    thread->queued = true;
    if (m_Tails[priority]) {
      m_Tails[priority]->next = thread;
    } else {
      m_Heads[priority] = thread;
    }
    m_Tails[priority] = thread;
#if PEDIGREE_READY_QUEUE_COUNTS
    ++m_QueueCounts[priority];
#endif
  }

  void unlink(BenchmarkThread* thread) {
    if (!thread || !thread->queued) {
      return;
    }

    const size_t priority = thread->priority;
    if (thread->previous) {
      thread->previous->next = thread->next;
    } else {
      m_Heads[priority] = thread->next;
    }
    if (thread->next) {
      thread->next->previous = thread->previous;
    } else {
      m_Tails[priority] = thread->previous;
    }
    thread->previous = nullptr;
    thread->next = nullptr;
    thread->queued = false;
#if PEDIGREE_READY_QUEUE_COUNTS
    --m_QueueCounts[priority];
#endif
  }

  std::vector<BenchmarkThread> m_Threads;
  std::array<BenchmarkThread*, kMaxPriorities> m_Heads{};
  std::array<BenchmarkThread*, kMaxPriorities> m_Tails{};
#if PEDIGREE_READY_QUEUE_COUNTS
  std::array<size_t, kMaxPriorities> m_QueueCounts{};
#endif
  BenchmarkSpinlock m_Lock;
};

static void BM_RoundRobinSelectionSamePriority(benchmark::State& state) {
  RoundRobinSelectionModel model(static_cast<size_t>(state.range(0)), false);

  while (state.KeepRunning()) {
    state.PauseTiming();
    model.refill();
    state.ResumeTiming();

    BenchmarkThread* next = model.selectNext();
    benchmark::DoNotOptimize(next);
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
  state.counters["ready_threads"] = static_cast<double>(state.range(0));
}

static void BM_RoundRobinSelectionMixedPriorities(benchmark::State& state) {
  RoundRobinSelectionModel model(static_cast<size_t>(state.range(0)), true);

  while (state.KeepRunning()) {
    state.PauseTiming();
    model.refill();
    state.ResumeTiming();

    BenchmarkThread* next = model.selectNext();
    benchmark::DoNotOptimize(next);
  }

  state.SetItemsProcessed(static_cast<int64_t>(state.iterations()));
  state.counters["ready_threads"] = static_cast<double>(state.range(0));
  state.counters["priority_levels"] = static_cast<double>(kMaxPriorities);
}

}  // namespace

BENCHMARK(BM_RoundRobinSelectionSamePriority)->RangeMultiplier(2)->Range(1, 512);
BENCHMARK(BM_RoundRobinSelectionMixedPriorities)->RangeMultiplier(2)->Range(1, 512);
