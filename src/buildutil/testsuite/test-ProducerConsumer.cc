/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "pedigree/kernel/utilities/ProducerConsumer.h"

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <vector>

namespace
{
class RecordingConsumer : public ProducerConsumer
{
  public:
    ~RecordingConsumer() override
    {
        destroy();
    }

    bool waitForCount(size_t count)
    {
        std::unique_lock<std::mutex> guard(m_Lock);
        return m_Condition.wait_for(
            guard, std::chrono::seconds(2),
            [this, count] { return m_Values.size() >= count; });
    }

    std::vector<uint64_t> values()
    {
        std::lock_guard<std::mutex> guard(m_Lock);
        return m_Values;
    }

  private:
    void consume(
        uint64_t p0, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t,
        uint64_t, uint64_t, uint64_t) override
    {
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
}  // namespace

TEST(PedigreeProducerConsumer, DrainsWorkQueuedBeforeConsumerSleeps)
{
    RecordingConsumer consumer;

    consumer.produce(41);
    consumer.produce(42);
    ASSERT_TRUE(consumer.initialise());
    ASSERT_TRUE(consumer.waitForCount(2));

    EXPECT_EQ(consumer.values(), (std::vector<uint64_t>{41, 42}));
}
