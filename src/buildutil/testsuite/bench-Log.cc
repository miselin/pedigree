/*
 * Copyright (c) 2008-2014, Pedigree Developers
 *
 * Please see the CONTRIB file in the root of the source tree for a full
 * list of contributors.
 *
 * Permission to use, copy, modify, and distribute this software for any
 * purpose with or without fee is hereby granted, provided that the above
 * copyright notice and this permission notice appear in all copies.
 *
 * THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
 * WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
 * MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
 * ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
 * WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
 * ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
 * OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.
 */

#define PEDIGREE_EXTERNAL_SOURCE 1

#include <string.h>

#include <benchmark/benchmark.h>

#include "pedigree/kernel/Log.h"
#include "pedigree/kernel/utilities/Cord.h"

class DiscardLogger : public Log::LogCallback
{
  public:
    DiscardLogger() : m_Length(0) {}

    void callback(const LogCord &cord)
    {
        // discard but keep length
        m_Length += cord.length();
    }

    size_t length() const
    {
        return m_Length;
    }

    void reset()
    {
        m_Length = 0;
    }

   private:
    size_t m_Length;
};

class LogFixture : public ::benchmark::Fixture
{
   public:
    void SetUp(const ::benchmark::State& st)
    {
        m_Logger.reset();

        // Reset state, install our logger
        Log::instance().enableTimestamps();
        Log::instance().installCallback(&m_Logger, true);
    }

    void TearDown(const ::benchmark::State&)
    {
        Log::instance().removeCallback(&m_Logger);
    }

    const DiscardLogger &logger() const
    {
        return m_Logger;
    }

   private:
    DiscardLogger m_Logger;
};

BENCHMARK_DEFINE_F(LogFixture, LogThroughputSimple)(benchmark::State& state)
{
    while (state.KeepRunning())
    {
        NOTICE("hello world");
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetBytesProcessed(logger().length());
}

BENCHMARK_DEFINE_F(LogFixture, LogThroughputSimpleNoTimestamps)(benchmark::State& state)
{
    Log::instance().disableTimestamps();

    while (state.KeepRunning())
    {
        NOTICE("hello world");
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetBytesProcessed(logger().length());
}

BENCHMARK_DEFINE_F(LogFixture, LogThroughputAllUnique)(benchmark::State& state)
{
    uint64_t i = 0;
    while (state.KeepRunning())
    {
        NOTICE("hello world " << i++);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetBytesProcessed(logger().length());
}

BENCHMARK_DEFINE_F(LogFixture, LogThroughputAllUniqueNoTimestamps)(benchmark::State& state)
{
    Log::instance().disableTimestamps();

    uint64_t i = 0;
    while (state.KeepRunning())
    {
        NOTICE("hello world " << i++);
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetBytesProcessed(logger().length());
}

BENCHMARK_DEFINE_F(LogFixture, LogThroughputExistingEntry)(benchmark::State& state)
{
    Log::LogEntry entry;
    entry << Log::Notice << "hello world";

    while (state.KeepRunning())
    {
        Log::instance() << entry << Flush;
    }

    state.SetItemsProcessed(int64_t(state.iterations()));
    state.SetBytesProcessed(logger().length());
}

BENCHMARK_REGISTER_F(LogFixture, LogThroughputSimple);
BENCHMARK_REGISTER_F(LogFixture, LogThroughputSimpleNoTimestamps);
BENCHMARK_REGISTER_F(LogFixture, LogThroughputAllUnique);
BENCHMARK_REGISTER_F(LogFixture, LogThroughputAllUniqueNoTimestamps);
BENCHMARK_REGISTER_F(LogFixture, LogThroughputExistingEntry);
