/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "pedigree/kernel/utilities/IntrusiveMpscQueue.h"

#include <atomic>
#include <cstddef>
#include <thread>
#include <vector>

namespace
{
struct QueueNode
{
    QueueNode *next = nullptr;
    size_t producer = 0;
    size_t sequence = 0;
};

using Queue = IntrusiveMpscQueue<QueueNode, &QueueNode::next>;
using QueueTestAccess =
    IntrusiveMpscQueueTestAccess<QueueNode, &QueueNode::next>;
}  // namespace

TEST(IntrusiveMpscQueue, EmptyAndSingleNodeReuse)
{
    QueueNode stub;
    QueueNode node;
    Queue queue(stub);
    QueueNode *out = &node;

    EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
    EXPECT_EQ(out, nullptr);

    for (size_t iteration = 0; iteration < 32; ++iteration)
    {
        queue.push(node);
        EXPECT_EQ(queue.pop(out), Queue::PopResult::Item);
        EXPECT_EQ(out, &node);
        EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
        EXPECT_EQ(out, nullptr);
    }
}

TEST(IntrusiveMpscQueue, PreservesFifoOrder)
{
    QueueNode stub;
    QueueNode first;
    QueueNode second;
    QueueNode third;
    Queue queue(stub);
    QueueNode *out = nullptr;

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

TEST(IntrusiveMpscQueue, IncompleteProducerPublicationIsTransient)
{
    QueueNode stub;
    QueueNode node;
    Queue queue(stub);
    QueueNode *out = &node;

    const QueueTestAccess::Publication publication =
        QueueTestAccess::beginPush(queue, node);

    EXPECT_EQ(queue.pop(out), Queue::PopResult::Transient);
    EXPECT_EQ(out, nullptr);

    QueueTestAccess::finishPush(queue, publication);
    ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
    EXPECT_EQ(out, &node);
    EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
}

TEST(IntrusiveMpscQueue, ExchangeOrderWinsWhenProducersLinkOutOfOrder)
{
    QueueNode stub;
    QueueNode first;
    QueueNode second;
    Queue queue(stub);
    QueueNode *out = nullptr;

    const QueueTestAccess::Publication firstPublication =
        QueueTestAccess::beginPush(queue, first);
    const QueueTestAccess::Publication secondPublication =
        QueueTestAccess::beginPush(queue, second);

    QueueTestAccess::finishPush(queue, secondPublication);
    EXPECT_EQ(queue.pop(out), Queue::PopResult::Transient);

    QueueTestAccess::finishPush(queue, firstPublication);
    ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
    EXPECT_EQ(out, &first);
    ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
    EXPECT_EQ(out, &second);
    EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
}

TEST(IntrusiveMpscQueue, LastNodeWaitsForPredecessorPublicationBeforeReuse)
{
    QueueNode stub;
    QueueNode first;
    QueueNode last;
    QueueNode following;
    Queue queue(stub);
    QueueNode *out = nullptr;

    queue.push(first);
    queue.push(last);
    ASSERT_EQ(queue.pop(out), Queue::PopResult::Item);
    ASSERT_EQ(out, &first);
    ASSERT_TRUE(QueueTestAccess::consumerSeesLastNode(queue));

    // The producer wins immediately after the consumer observes head == tail.
    const QueueTestAccess::Publication publication =
        QueueTestAccess::beginPush(queue, following);
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

TEST(IntrusiveMpscQueue, HighContentionProducersLoseNoWork)
{
    constexpr size_t ProducerCount = 8;
    constexpr size_t NodesPerProducer = 4096;
    constexpr size_t TotalNodes = ProducerCount * NodesPerProducer;

    QueueNode stub;
    Queue queue(stub);
    std::vector<QueueNode> nodes(TotalNodes);
    std::vector<std::thread> producers;
    std::vector<QueueNode *> received;
    std::atomic<size_t> ready{0};
    std::atomic<bool> start{false};

    producers.reserve(ProducerCount);
    received.reserve(TotalNodes);
    for (size_t producer = 0; producer < ProducerCount; ++producer)
    {
        for (size_t sequence = 0; sequence < NodesPerProducer; ++sequence)
        {
            QueueNode &node = nodes[producer * NodesPerProducer + sequence];
            node.producer = producer;
            node.sequence = sequence;
        }

        producers.emplace_back([&, producer] {
            ready.fetch_add(1, std::memory_order_release);
            while (!start.load(std::memory_order_acquire))
            {
                std::this_thread::yield();
            }

            for (size_t sequence = 0; sequence < NodesPerProducer; ++sequence)
            {
                queue.push(nodes[producer * NodesPerProducer + sequence]);
            }
        });
    }

    while (ready.load(std::memory_order_acquire) != ProducerCount)
    {
        std::this_thread::yield();
    }
    start.store(true, std::memory_order_release);

    while (received.size() != TotalNodes)
    {
        QueueNode *node = nullptr;
        if (queue.pop(node) == Queue::PopResult::Item)
        {
            received.push_back(node);
        }
        else
        {
            std::this_thread::yield();
        }
    }

    for (std::thread &producer : producers)
    {
        producer.join();
    }

    std::vector<size_t> expectedSequence(ProducerCount, 0);
    std::vector<bool> seen(TotalNodes, false);
    for (QueueNode *node : received)
    {
        ASSERT_LT(node->producer, ProducerCount);
        ASSERT_LT(node->sequence, NodesPerProducer);

        const size_t index = node->producer * NodesPerProducer + node->sequence;
        EXPECT_EQ(node, &nodes[index]);
        EXPECT_FALSE(seen[index]);
        seen[index] = true;
        EXPECT_EQ(node->sequence, expectedSequence[node->producer]);
        ++expectedSequence[node->producer];
    }

    for (size_t producer = 0; producer < ProducerCount; ++producer)
    {
        EXPECT_EQ(expectedSequence[producer], NodesPerProducer);
    }

    QueueNode *out = nullptr;
    EXPECT_EQ(queue.pop(out), Queue::PopResult::Empty);
}
