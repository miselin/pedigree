/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/utilities/ProducerConsumer.h"
#include "pedigree/kernel/utilities/UnlikelyLock.h"

#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <thread>
#include <vector>

#include <condition_variable>

namespace {
[[noreturn]] void fail(const char* check) {
  std::fprintf(stderr, "UtilityWorkers fuzz mismatch: %s\n", check);
  std::abort();
}

class RecordingConsumer : public ProducerConsumer {
 public:
  ~RecordingConsumer() override {
    destroy();
  }

  std::vector<uint64_t> waitFor(size_t count) {
    std::unique_lock<std::mutex> guard(m_Lock);
    if (!m_Condition.wait_for(guard, std::chrono::seconds(2),
                              [this, count] { return m_Values.size() == count; })) {
      fail("ProducerConsumer stalled");
    }
    return m_Values;
  }

 private:
  void consume(uint64_t p0, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
               uint64_t) override {
    {
      std::lock_guard<std::mutex> guard(m_Lock);
      m_Values.push_back(p0);
    }
    m_Condition.notify_all();
  }

  std::mutex m_Lock;
  std::condition_variable m_Condition;
  std::vector<uint64_t> m_Values;
};

void fuzzProducerConsumer(const uint8_t* data, size_t size) {
  RecordingConsumer consumer;
  std::vector<uint64_t> expected;
  const size_t count = size < 24 ? size : 24;
  const size_t split = size ? data[0] % (count + 1) : 0;
  for (size_t i = 0; i < split; ++i) {
    const uint64_t value = (i << 8) | data[i];
    consumer.produce(value);
    expected.push_back(value);
  }
  if (!consumer.initialise()) {
    fail("ProducerConsumer initialise");
  }
  for (size_t i = split; i < count; ++i) {
    const uint64_t value = (i << 8) | data[i];
    consumer.produce(value);
    expected.push_back(value);
  }
  if (!consumer.initialise() || consumer.waitFor(count) != expected) {
    fail("ProducerConsumer order");
  }
}

void fuzzUnlikelyLock(const uint8_t* data, size_t size) {
  UnlikelyLock lock;
  std::atomic<unsigned> readers{0};
  std::atomic<unsigned> writers{0};
  std::atomic<bool> violation{false};
  std::vector<std::thread> workers;
  for (size_t worker = 0; worker < 3; ++worker) {
    workers.emplace_back([&, worker] {
      for (size_t step = 0; step < 8; ++step) {
        const uint8_t action = size ? data[(worker * 8 + step) % size] : worker + step;
        if (action & 1) {
          lock.acquire();
          if (readers.load(std::memory_order_seq_cst) ||
              writers.fetch_add(1, std::memory_order_seq_cst)) {
            violation.store(true, std::memory_order_seq_cst);
          }
          if (action & 2) {
            std::this_thread::yield();
          }
          writers.fetch_sub(1, std::memory_order_seq_cst);
          lock.release();
        } else {
          lock.enter();
          readers.fetch_add(1, std::memory_order_seq_cst);
          if (writers.load(std::memory_order_seq_cst)) {
            violation.store(true, std::memory_order_seq_cst);
          }
          if (action & 2) {
            std::this_thread::yield();
          }
          readers.fetch_sub(1, std::memory_order_seq_cst);
          lock.leave();
        }
      }
    });
  }
  for (std::thread& worker : workers) {
    worker.join();
  }
  if (violation.load(std::memory_order_seq_cst) || readers.load() || writers.load()) {
    fail("UnlikelyLock exclusivity");
  }
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 128) {
    return 0;
  }
  fuzzProducerConsumer(data, size);
  fuzzUnlikelyLock(data, size);
  return 0;
}
