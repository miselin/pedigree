/*
 * Copyright (c) 2026, Pedigree Developers
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <gtest/gtest.h>

#include "pedigree/kernel/Atomic.h"

#include <thread>
#include <vector>

TEST(PedigreeAtomic, ValueStoresCopiesAndLoads)
{
    Atomic<size_t> value(7);
    EXPECT_EQ(static_cast<size_t>(value), 7U);
    EXPECT_EQ(value.value(), 7U);

    value = 11;
    Atomic<size_t> copied(value);
    Atomic<size_t> assigned;
    assigned = copied;

    EXPECT_EQ(static_cast<size_t>(value), 11U);
    EXPECT_EQ(static_cast<size_t>(copied), 11U);
    EXPECT_EQ(static_cast<size_t>(assigned), 11U);

    Atomic<bool> flag(false);
    flag = true;
    Atomic<bool> copiedFlag(flag);
    EXPECT_TRUE(static_cast<bool>(flag));
    EXPECT_TRUE(static_cast<bool>(copiedFlag));
}

TEST(PedigreeAtomic, ConcurrentReadModifyWriteDoesNotLoseUpdates)
{
    constexpr size_t ThreadCount = 8;
    constexpr size_t Iterations = 10000;
    Atomic<size_t> value(0);
    std::vector<std::thread> workers;
    workers.reserve(ThreadCount);

    for (size_t thread = 0; thread < ThreadCount; ++thread)
    {
        workers.emplace_back([&value] {
            for (size_t iteration = 0; iteration < Iterations; ++iteration)
            {
                value += 1;
            }
        });
    }

    for (std::thread &worker : workers)
    {
        worker.join();
    }

    EXPECT_EQ(
        static_cast<size_t>(value), ThreadCount * Iterations);
}
