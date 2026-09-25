/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1
#include "pedigree/kernel/utilities/RequestQueue.h"

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <vector>

namespace {
[[noreturn]] void fail(size_t operation, const char* check) {
  std::fprintf(stderr, "RequestQueue fuzz mismatch at operation %zu: %s\n", operation, check);
  std::abort();
}

class RecordingQueue : public RequestQueue {
 public:
  RecordingQueue() : RequestQueue(String("fuzz")) {}

  ~RecordingQueue() override {
    destroy();
  }

  const std::vector<uint64_t>& values() const {
    return m_Values;
  }

 private:
  uint64_t executeRequest(uint64_t p1, uint64_t p2, uint64_t, uint64_t, uint64_t, uint64_t,
                          uint64_t, uint64_t) override {
    const uint64_t value = p1 ^ (p2 << 8);
    m_Values.push_back(value);
    return value;
  }

  std::vector<uint64_t> m_Values;
};

void released(void* context) {
  ++*static_cast<size_t*>(context);
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 512) {
    return 0;
  }

  RecordingQueue queue;
  std::vector<uint64_t> expected;
  size_t releases = 0;
  size_t expectedReleases = 0;
  RequestQueue::PreallocatedRequest token(released, &releases);
  if (!queue.resume()) {
    fail(0, "resume");
  }

  const size_t operations = size / 3 < 64 ? size / 3 : 64;
  for (size_t operation = 0; operation < operations; ++operation) {
    const size_t offset = operation * 3;
    const size_t priority = data[offset] % REQUEST_QUEUE_NUM_PRIORITIES;
    const uint64_t value = data[offset + 1] ^ (uint64_t(data[offset + 2]) << 8);
    switch (data[offset] % 3) {
      case 0:
        if (queue.addRequest(priority, data[offset + 1], data[offset + 2]) != value) {
          fail(operation, "synchronous result");
        }
        break;
      case 1:
        if (queue.addAsyncRequest(priority, data[offset + 1], data[offset + 2]) != 1) {
          fail(operation, "asynchronous acceptance");
        }
        break;
      case 2:
        if (queue.publishPreallocated(token, priority, data[offset + 1], data[offset + 2]) !=
                RequestQueue::PreallocatedPublishResult::Accepted ||
            !token.isAvailable() || !queue.waitForPreallocated(token)) {
          fail(operation, "preallocated publication");
        }
        ++expectedReleases;
        if (releases != expectedReleases) {
          fail(operation, "release callback");
        }
        break;
    }
    expected.push_back(value);
    if (queue.values() != expected) {
      fail(operation, "execution order");
    }
  }

  if (!queue.drain() || !queue.halt()) {
    fail(operations, "lifecycle");
  }
  return 0;
}
