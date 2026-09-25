/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include "pedigree/kernel/utilities/IntrusiveMpscQueue.h"

#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <thread>
#include <vector>

#include <gtest/gtest.h>

namespace {
struct QueueNode {
  QueueNode* next = nullptr;
  size_t producer = 0;
  size_t sequence = 0;
};

using Queue = IntrusiveMpscQueue<QueueNode, &QueueNode::next>;
using QueueTestAccess = IntrusiveMpscQueueTestAccess<QueueNode, &QueueNode::next>;
}  // namespace

TEST(IntrusiveMpscQueue, EmptyAndSingleNodeReuse) {
  QueueNode stub;
  QueueNode node;
  Queue queue(stub);
  QueueNode* out = &node;

  EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
  EXPECT_EQ(out, nullptr);

  for (size_t iteration = 0; iteration < 32; ++iteration) {
    queue.push(node);
    EXPECT_EQ(queue.pop(out), Queue::PopResult::Item);
    EXPECT_EQ(out, &node);
    EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
    EXPECT_EQ(out, nullptr);
  }
}

TEST(IntrusiveMpscQueue, PreservesFifoOrder) {
  QueueNode stub;
  QueueNode first;
  QueueNode second;
  QueueNode third;
  Queue queue(stub);
  QueueNode* out = nullptr;

  queue.push(first);
  queue.push(second);
  queue.push(third);

  ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
  EXPECT_EQ(out, &first);
  ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
  EXPECT_EQ(out, &second);
  ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
  EXPECT_EQ(out, &third);
  EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
}

TEST(IntrusiveMpscQueue, IncompleteProducerPublicationIsTransient) {
  QueueNode stub;
  QueueNode node;
  Queue queue(stub);
  QueueNode* out = &node;

  const QueueTestAccess::Publication publication = QueueTestAccess::beginPush(queue, node);

  EXPECT_EQ(queue.pop(out), Queue::PopResult::Transient);
  EXPECT_EQ(out, nullptr);

  QueueTestAccess::finishPush(queue, publication);
  ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
  EXPECT_EQ(out, &node);
  EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
}

TEST(IntrusiveMpscQueue, ExchangeOrderWinsWhenProducersLinkOutOfOrder) {
  QueueNode stub;
  QueueNode first;
  QueueNode second;
  Queue queue(stub);
  QueueNode* out = nullptr;

  const QueueTestAccess::Publication firstPublication = QueueTestAccess::beginPush(queue, first);
  const QueueTestAccess::Publication secondPublication = QueueTestAccess::beginPush(queue, second);

  QueueTestAccess::finishPush(queue, secondPublication);
  EXPECT_EQ(queue.pop(out), Queue::PopResult::Transient);

  QueueTestAccess::finishPush(queue, firstPublication);
  ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
  EXPECT_EQ(out, &first);
  ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
  EXPECT_EQ(out, &second);
  EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
}

TEST(IntrusiveMpscQueue, LastNodeWaitsForPredecessorPublicationBeforeReuse) {
  QueueNode stub;
  QueueNode first;
  QueueNode last;
  QueueNode following;
  Queue queue(stub);
  QueueNode* out = nullptr;

  queue.push(first);
  queue.push(last);
  ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
  ASSERT_EQ(out, &first);
  ASSERT_TRUE(QueueTestAccess::consumerSeesLastNode(queue));

  // The producer wins immediately after the consumer observes head == tail.
  const QueueTestAccess::Publication publication = QueueTestAccess::beginPush(queue, following);
  QueueTestAccess::rotateStub(queue);

  EXPECT_EQ(queue.pop(out), Queue::PopResult::Transient);
  EXPECT_EQ(out, nullptr);

  QueueTestAccess::finishPush(queue, publication);
  ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
  EXPECT_EQ(out, &last);
  ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
  EXPECT_EQ(out, &following);
  EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);

  queue.push(last);
  ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
  EXPECT_EQ(out, &last);
  EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
}

TEST(IntrusiveMpscQueue, HighContentionProducersLoseNoWork) {
  constexpr size_t ProducerCount = 8;
  constexpr size_t NodesPerProducer = 4096;
  constexpr size_t TotalNodes = ProducerCount * NodesPerProducer;

  QueueNode stub;
  Queue queue(stub);
  std::vector<QueueNode> nodes(TotalNodes);
  std::vector<std::thread> producers;
  std::vector<QueueNode*> received;
  std::atomic<size_t> ready{0};
  std::atomic<bool> start{false};

  producers.reserve(ProducerCount);
  received.reserve(TotalNodes);
  for (size_t producer = 0; producer < ProducerCount; ++producer) {
    for (size_t sequence = 0; sequence < NodesPerProducer; ++sequence) {
      QueueNode& node = nodes[producer * NodesPerProducer + sequence];
      node.producer = producer;
      node.sequence = sequence;
    }

    producers.emplace_back([&, producer] {
      ready.fetch_add(1, std::memory_order_release);
      while (!start.load(std::memory_order_acquire)) {
        std::this_thread::yield();
      }

      for (size_t sequence = 0; sequence < NodesPerProducer; ++sequence) {
        queue.push(nodes[producer * NodesPerProducer + sequence]);
      }
    });
  }

  while (ready.load(std::memory_order_acquire) != ProducerCount) {
    std::this_thread::yield();
  }
  start.store(true, std::memory_order_release);

  while (received.size() != TotalNodes) {
    QueueNode* node = nullptr;
    if (queue.pop(node) == Queue::PopResult::Item) {
      received.push_back(node);
    } else {
      std::this_thread::yield();
    }
  }

  for (std::thread& producer : producers) {
    producer.join();
  }

  std::vector<size_t> expectedSequence(ProducerCount, 0);
  std::vector<bool> seen(TotalNodes, false);
  for (QueueNode* node : received) {
    ASSERT_LT(node->producer, ProducerCount);
    ASSERT_LT(node->sequence, NodesPerProducer);

    const size_t index = node->producer * NodesPerProducer + node->sequence;
    EXPECT_EQ(node, &nodes[index]);
    EXPECT_FALSE(seen[index]);
    seen[index] = true;
    EXPECT_EQ(node->sequence, expectedSequence[node->producer]);
    ++expectedSequence[node->producer];
  }

  for (size_t producer = 0; producer < ProducerCount; ++producer) {
    EXPECT_EQ(expectedSequence[producer], NodesPerProducer);
  }

  QueueNode* out = nullptr;
  EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
}

TEST(IntrusiveMpscQueue, RecycledNodesRemainOrderedAcrossContendedRounds) {
  constexpr size_t ProducerCount = 4;
  constexpr size_t NodesPerProducer = 4;
  constexpr size_t PushesPerProducer = 512;
  constexpr size_t TotalNodes = ProducerCount * NodesPerProducer;
  constexpr size_t TotalPushes = ProducerCount * PushesPerProducer;

  for (size_t round = 0; round < 6; ++round) {
    QueueNode stub;
    Queue queue(stub);
    std::array<QueueNode, TotalNodes> nodes{};
    std::array<std::atomic<size_t>, TotalNodes> returned{};
    for (auto& count : returned) {
      count.store(0, std::memory_order_relaxed);
    }

    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};
    std::atomic<bool> stop{false};
    std::vector<std::thread> producers;
    producers.reserve(ProducerCount);
    for (size_t producer = 0; producer < ProducerCount; ++producer) {
      producers.emplace_back([&, producer] {
        ready.fetch_add(1, std::memory_order_release);
        while (!start.load(std::memory_order_acquire)) {
          std::this_thread::yield();
        }
        for (size_t sequence = 0; sequence < PushesPerProducer; ++sequence) {
          if (stop.load(std::memory_order_relaxed)) {
            return;
          }
          const size_t index = producer * NodesPerProducer + sequence % NodesPerProducer;
          if (sequence >= NodesPerProducer) {
            const size_t previous = sequence - NodesPerProducer + 1;
            while (returned[index].load(std::memory_order_acquire) != previous) {
              if (stop.load(std::memory_order_relaxed)) {
                return;
              }
              std::this_thread::yield();
            }
          }
          QueueNode& node = nodes[index];
          node.producer = producer;
          node.sequence = sequence;
          queue.push(node);
          if (((sequence + round * 11 + producer * 7) & 7) == 0) {
            std::this_thread::yield();
          }
        }
      });
    }

    const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
    while (ready.load(std::memory_order_acquire) != ProducerCount &&
           std::chrono::steady_clock::now() < deadline) {
      std::this_thread::yield();
    }
    const bool allReady = ready.load(std::memory_order_acquire) == ProducerCount;
    if (!allReady) {
      stop.store(true, std::memory_order_relaxed);
    }
    start.store(true, std::memory_order_release);

    std::array<size_t, ProducerCount> expected{};
    size_t received = 0;
    bool invalidItem = false;
    while (allReady && received != TotalPushes && std::chrono::steady_clock::now() < deadline) {
      QueueNode* node = nullptr;
      if (queue.pop(node) != Queue::PopResult::Item) {
        std::this_thread::yield();
        continue;
      }
      const size_t producer = node->producer;
      const size_t sequence = node->sequence;
      if (producer >= ProducerCount || sequence >= PushesPerProducer ||
          node != &nodes[producer * NodesPerProducer + sequence % NodesPerProducer] ||
          sequence != expected[producer]) {
        invalidItem = true;
        break;
      }
      ++expected[producer];
      returned[producer * NodesPerProducer + sequence % NodesPerProducer].store(
          sequence + 1, std::memory_order_release);
      ++received;
    }

    stop.store(true, std::memory_order_relaxed);
    for (auto& producer : producers) {
      producer.join();
    }

    ASSERT_TRUE(allReady) << "round " << round;
    ASSERT_FALSE(invalidItem) << "round " << round << ", item " << received;
    ASSERT_EQ(received, TotalPushes) << "round " << round;
    for (size_t producer = 0; producer < ProducerCount; ++producer) {
      EXPECT_EQ(expected[producer], PushesPerProducer) << "round " << round;
    }
    QueueNode* node = nullptr;
    EXPECT_EQ(queue.pop(node), Queue::PopResult::Empty) << "round " << round;
  }
}
