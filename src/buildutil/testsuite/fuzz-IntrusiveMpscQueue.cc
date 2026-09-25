/* Copyright (c) 2026, Pedigree Developers. SPDX-License-Identifier: ISC */
#define PEDIGREE_EXTERNAL_SOURCE 1
#define TESTSUITE 1
#include "pedigree/kernel/utilities/IntrusiveMpscQueue.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <deque>

namespace {
struct Node {
  Node* next = nullptr;
  size_t index = 0;
};

using Queue = IntrusiveMpscQueue<Node, &Node::next>;
using Access = IntrusiveMpscQueueTestAccess<Node, &Node::next>;

[[noreturn]] void fail(size_t operation, const char* check) {
  std::fprintf(stderr, "IntrusiveMpscQueue fuzz mismatch at operation %zu: %s\n", operation, check);
  std::abort();
}
}  // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  if (size > 512) {
    return 0;
  }

  Node stub;
  Queue queue(stub);
  std::array<Node, 16> nodes{};
  std::array<bool, 16> available{};
  available.fill(true);
  std::deque<Node*> expected;
  std::deque<Access::Publication> pending;
  for (size_t i = 0; i < nodes.size(); ++i) {
    nodes[i].index = i;
  }

  size_t operation = 0;
  for (size_t offset = 0; offset + 1 < size; offset += 2, ++operation) {
    const uint8_t action = data[offset] % 10;
    const size_t index = data[offset + 1] % nodes.size();
    if (action < 5 && available[index]) {
      Node& node = nodes[index];
      available[index] = false;
      expected.push_back(&node);
      if (action < 3) {
        queue.push(node);
      } else {
        pending.push_back(Access::beginPush(queue, node));
      }
    } else if (action < 8 && !pending.empty()) {
      const size_t chosen = index % pending.size();
      const Access::Publication publication = pending[chosen];
      pending.erase(pending.begin() + chosen);
      Access::finishPush(queue, publication);
    } else {
      Node* out = reinterpret_cast<Node*>(1);
      const Queue::PopResult result = queue.pop(out);
      if (result == Queue::PopResult::Item) {
        if (expected.empty() || out != expected.front()) {
          fail(operation, "FIFO order");
        }
        expected.pop_front();
        available[out->index] = true;
      } else if (out != nullptr ||
                 (pending.empty() && result != (expected.empty() ? Queue::PopResult::Empty
                                                                 : Queue::PopResult::Item))) {
        fail(operation, "unexpected empty or transient result");
      }
    }
  }

  while (!pending.empty()) {
    const Access::Publication publication = pending.back();
    pending.pop_back();
    Access::finishPush(queue, publication);
  }
  while (!expected.empty()) {
    Node* out = nullptr;
    if (queue.pop(out) != Queue::PopResult::Item || out != expected.front()) {
      fail(operation, "final drain");
    }
    expected.pop_front();
    available[out->index] = true;
  }
  Node* out = reinterpret_cast<Node*>(1);
  if (queue.pop(out) != Queue::PopResult::Empty || out != nullptr) {
    fail(operation, "final empty");
  }
  return 0;
}
